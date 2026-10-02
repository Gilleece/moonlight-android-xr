// The virtual surround's convolution. The direct kernel against a double
// precision loop written the obvious way, with the LFE and the channel sum
// checked on their own, then the FFT path against the direct kernel, at the
// block sizes a stream sends: 240 frames (5 ms) and 480 (10 ms).
#include "check.h"
#include "xr_audio.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHANNELS 6
#define TAPS 17
#define FRAMES 37
#define LFE 3
#define STRIDE (TAPS - 1 + FRAMES)

// What the renderer runs at: KEMAR's 128 taps plus the equalisation's 32,
// resampled to 48 kHz
#define KEMAR_TAPS 175

static float history[CHANNELS * STRIDE];
static float filtersLeft[CHANNELS * TAPS];
static float filtersRight[CHANNELS * TAPS];
static float gains[CHANNELS];
static float outLeft[FRAMES];
static float outRight[FRAMES];

static float nextValue(void) {
    return (float) (rand() / (double) RAND_MAX * 2.0 - 1.0);
}

static void fillEverything(void) {
    for (int i = 0; i < CHANNELS * STRIDE; i++) {
        history[i] = 1000.0f * nextValue();
    }
    for (int i = 0; i < CHANNELS * TAPS; i++) {
        filtersLeft[i] = nextValue();
        filtersRight[i] = nextValue();
    }
    for (int channel = 0; channel < CHANNELS; channel++) {
        gains[channel] = channel < 2 ? 1.0f : 0.707f;
    }
}

// The sum straight from the definition, in double
static void reference(double* left, double* right) {
    for (int frame = 0; frame < FRAMES; frame++) {
        left[frame] = 0.0;
        right[frame] = 0.0;
    }
    for (int channel = 0; channel < CHANNELS; channel++) {
        const float* samples = history + channel * STRIDE;
        for (int frame = 0; frame < FRAMES; frame++) {
            if (channel == LFE) {
                double sample = gains[channel] * (double) samples[TAPS - 1 + frame];
                left[frame] += sample;
                right[frame] += sample;
                continue;
            }
            double sumLeft = 0.0;
            double sumRight = 0.0;
            for (int tap = 0; tap < TAPS; tap++) {
                double sample = samples[frame + tap];
                sumLeft += filtersLeft[channel * TAPS + tap] * sample;
                sumRight += filtersRight[channel * TAPS + tap] * sample;
            }
            left[frame] += gains[channel] * sumLeft;
            right[frame] += gains[channel] * sumRight;
        }
    }
}

static void run(void) {
    audioConvolve(history, STRIDE, filtersLeft, filtersRight, TAPS, gains, CHANNELS, LFE,
                  FRAMES, outLeft, outRight);
}

static void testAgainstTheObviousLoop(void) {
    double left[FRAMES];
    double right[FRAMES];
    srand(7);
    for (int round = 0; round < 4; round++) {
        fillEverything();
        reference(left, right);
        run();
        for (int frame = 0; frame < FRAMES; frame++) {
            CHECK_NEAR(outLeft[frame], left[frame], 1e-4 * fabs(left[frame]) + 1e-3);
            CHECK_NEAR(outRight[frame], right[frame], 1e-4 * fabs(right[frame]) + 1e-3);
        }
    }
}

static void testTheLfeIsNotPlaced(void) {
    srand(11);
    fillEverything();
    memset(history, 0, sizeof(history));
    for (int frame = 0; frame < FRAMES; frame++) {
        history[LFE * STRIDE + TAPS - 1 + frame] = 100.0f + frame;
    }
    run();
    for (int frame = 0; frame < FRAMES; frame++) {
        // Both ears get it as it is, at its gain, with no filter
        CHECK_NEAR(outLeft[frame], gains[LFE] * (100.0 + frame), 1e-3);
        CHECK_NEAR(outRight[frame], outLeft[frame], 0.0);
    }
}

static void testChannelsAddUp(void) {
    double first[FRAMES];
    double second[FRAMES];
    srand(13);
    fillEverything();
    float all[CHANNELS * STRIDE];
    memcpy(all, history, sizeof(all));

    memset(history, 0, sizeof(history));
    memcpy(history, all, STRIDE * sizeof(float));
    run();
    for (int frame = 0; frame < FRAMES; frame++) {
        first[frame] = outLeft[frame];
    }

    memset(history, 0, sizeof(history));
    memcpy(history + 4 * STRIDE, all + 4 * STRIDE, STRIDE * sizeof(float));
    run();
    for (int frame = 0; frame < FRAMES; frame++) {
        second[frame] = outLeft[frame];
    }

    memset(history, 0, sizeof(history));
    memcpy(history, all, STRIDE * sizeof(float));
    memcpy(history + 4 * STRIDE, all + 4 * STRIDE, STRIDE * sizeof(float));
    run();
    for (int frame = 0; frame < FRAMES; frame++) {
        CHECK_NEAR(outLeft[frame], first[frame] + second[frame],
                   1e-4 * fabs(first[frame] + second[frame]) + 1e-3);
    }
}

// The FFT path from here on, always against audioConvolve with the same
// filters mixed in the time domain, since the tests above pin that kernel down

#define MOST_CHANNELS 8
// The longest filter a 512 point transform takes
#define MOST_TAPS (AUDIO_FFT_N / 2 - 1)
#define MOST_RING 72
#define MOST_FRAMES 4100
#define MOST_STRIDE (MOST_TAPS - 1 + MOST_FRAMES)

static float ringLeft[MOST_RING * MOST_TAPS];
static float ringRight[MOST_RING * MOST_TAPS];
static float ringHistory[MOST_CHANNELS * MOST_STRIDE];
static float ringGains[MOST_CHANNELS];
static int keyLow[MOST_CHANNELS];
static int keyHigh[MOST_CHANNELS];
static float keyFar[MOST_CHANNELS];
static float blendLeft[MOST_CHANNELS * MOST_TAPS];
static float blendRight[MOST_CHANNELS * MOST_TAPS];
static float gotLeft[MOST_FRAMES];
static float gotRight[MOST_FRAMES];
static float gotOldLeft[MOST_FRAMES];
static float gotOldRight[MOST_FRAMES];
static float wantLeft[MOST_FRAMES];
static float wantRight[MOST_FRAMES];

static void fillRing(int ring, int taps) {
    for (int i = 0; i < ring * taps; i++) {
        ringLeft[i] = nextValue();
        ringRight[i] = nextValue();
    }
}

static void fillHistory(int channels, int stride) {
    for (int i = 0; i < channels * stride; i++) {
        ringHistory[i] = 1000.0f * nextValue();
    }
}

static void fillGains(int channels) {
    for (int channel = 0; channel < channels; channel++) {
        ringGains[channel] = channel < 2 ? 1.0f : 0.707f;
    }
}

static void fillKeys(int channels, int ring) {
    for (int channel = 0; channel < channels; channel++) {
        keyLow[channel] = rand() % ring;
        keyHigh[channel] = (keyLow[channel] + 1) % ring;
        keyFar[channel] = (float) (rand() / (double) RAND_MAX);
    }
}

// Moves every channel's history on by a block: the tail stays, new samples
// follow it, as the Java side does between calls
static void nextBlock(int channels, int taps, int stride, int frames) {
    for (int channel = 0; channel < channels; channel++) {
        float* samples = ringHistory + channel * stride;
        memmove(samples, samples + frames, (size_t) (taps - 1) * sizeof(float));
        for (int frame = 0; frame < frames; frame++) {
            samples[taps - 1 + frame] = 1000.0f * nextValue();
        }
    }
}

// The filters the context should have built, mixed from the same two ring
// entries in the time domain and left reversed for the direct kernel
static void blendFilters(int channels, int lfe, int taps) {
    for (int channel = 0; channel < channels; channel++) {
        if (channel == lfe) {
            continue;
        }
        const float far = keyFar[channel];
        const float near = 1.0f - far;
        const float* lowLeft = ringLeft + keyLow[channel] * taps;
        const float* highLeft = ringLeft + keyHigh[channel] * taps;
        const float* lowRight = ringRight + keyLow[channel] * taps;
        const float* highRight = ringRight + keyHigh[channel] * taps;
        for (int tap = 0; tap < taps; tap++) {
            blendLeft[channel * taps + tap] = near * lowLeft[tap] + far * highLeft[tap];
            blendRight[channel * taps + tap] = near * lowRight[tap] + far * highRight[tap];
        }
    }
}

static void referenceFor(int channels, int lfe, int taps, int stride, int frames) {
    blendFilters(channels, lfe, taps);
    audioConvolve(ringHistory, stride, blendLeft, blendRight, taps, ringGains, channels, lfe,
                  frames, wantLeft, wantRight);
}

// Float arithmetic both ways, so close rather than equal: a part in ten
// thousand of the sample, or of the block's peak where the sample is near 0
static void compare(const char* what, const float* got, const float* want, int frames) {
    double peak = 0.0;
    for (int frame = 0; frame < frames; frame++) {
        if (fabs(want[frame]) > peak) {
            peak = fabs(want[frame]);
        }
    }
    int was = checksFailed;
    for (int frame = 0; frame < frames; frame++) {
        CHECK_NEAR(got[frame], want[frame], 1e-4 * fabs(want[frame]) + 1e-5 * peak);
    }
    if (checksFailed != was) {
        fprintf(stderr, "  in %s\n", what);
    }
}

// One block through each path
static void checkOneBlock(const char* what, int channels, int lfe, int taps, int ring,
                          int frames) {
    int stride = taps - 1 + frames;
    fillRing(ring, taps);
    fillHistory(channels, stride);
    fillGains(channels);
    fillKeys(channels, ring);
    AudioFft* ctx = audioFftCreate(taps, channels, lfe, ring, ringLeft, ringRight);
    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    audioFftConvolve(ctx, ringHistory, stride, keyLow, keyHigh, keyFar, ringGains, frames, 0,
                     gotLeft, gotRight, gotOldLeft, gotOldRight);
    referenceFor(channels, lfe, taps, stride, frames);
    compare(what, gotLeft, wantLeft, frames);
    compare(what, gotRight, wantRight, frames);
    audioFftDestroy(ctx);
}

static void testFftMatchesTheKernel(void) {
    srand(17);
    checkOneBlock("6 channels, 17 taps, 37 frames", 6, LFE, 17, 8, 37);
    // A 5 ms packet is one transform: 174 frames of history and 240 new
    checkOneBlock("5.1, 175 taps, 240 frames", 6, LFE, KEMAR_TAPS, 72, 240);
    checkOneBlock("7.1, 175 taps, 240 frames", 8, LFE, KEMAR_TAPS, 72, 240);
}

static void testFftSplitsLongBlocks(void) {
    srand(19);
    // One transform carries N - taps + 1 frames, 338 at 175 taps: the most
    // that fits in one, the least that needs two, and a 10 ms packet
    checkOneBlock("175 taps, 338 frames", 8, LFE, KEMAR_TAPS, 72, AUDIO_FFT_N - KEMAR_TAPS + 1);
    checkOneBlock("175 taps, 339 frames", 8, LFE, KEMAR_TAPS, 72, AUDIO_FFT_N - KEMAR_TAPS + 2);
    checkOneBlock("7.1, 175 taps, 480 frames", 8, LFE, KEMAR_TAPS, 72, 480);
    // And many pieces, short filters and the longest the transform holds
    checkOneBlock("17 taps, 4100 frames", 6, LFE, 17, 8, 4100);
    checkOneBlock("255 taps, 1000 frames", 6, LFE, MOST_TAPS, 72, 1000);
}

// Five blocks in a row, each carrying the last one's tail
static void testFftCarriesTheTail(void) {
    const int channels = 8;
    const int taps = KEMAR_TAPS;
    const int frames = 240;
    const int stride = taps - 1 + frames;
    srand(23);
    fillRing(MOST_RING, taps);
    fillHistory(channels, stride);
    fillGains(channels);
    fillKeys(channels, MOST_RING);
    AudioFft* ctx = audioFftCreate(taps, channels, LFE, MOST_RING, ringLeft, ringRight);
    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    for (int block = 0; block < 5; block++) {
        if (block > 0) {
            nextBlock(channels, taps, stride, frames);
        }
        audioFftConvolve(ctx, ringHistory, stride, keyLow, keyHigh, keyFar, ringGains, frames, 0,
                         gotLeft, gotRight, gotOldLeft, gotOldRight);
        referenceFor(channels, LFE, taps, stride, frames);
        compare("block in a row", gotLeft, wantLeft, frames);
        compare("block in a row", gotRight, wantRight, frames);
    }
    audioFftDestroy(ctx);
}

// No block delay: with a filter that passes the signal straight through, what
// comes out of a call is exactly what went into that same call, frame for
// frame, at both packet sizes. A path that held a block back to fill a
// transform would hand back the previous block, or silence, here.
static void testNoBlockDelay(void) {
    const int channels = 6;
    const int taps = KEMAR_TAPS;
    const int ring = 4;
    const int sizes[] = { 240, 480 };
    srand(43);
    memset(ringLeft, 0, sizeof(ringLeft));
    memset(ringRight, 0, sizeof(ringRight));
    for (int entry = 0; entry < ring; entry++) {
        // Reversed, so the last tap is the one at time zero
        ringLeft[entry * taps + taps - 1] = 1.0f;
        ringRight[entry * taps + taps - 1] = 1.0f;
    }
    for (int size = 0; size < 2; size++) {
        const int frames = sizes[size];
        const int stride = taps - 1 + frames;
        memset(ringHistory, 0, sizeof(ringHistory));
        for (int channel = 0; channel < channels; channel++) {
            ringGains[channel] = channel == 0 ? 1.0f : 0.0f;
            keyLow[channel] = 0;
            keyHigh[channel] = 1;
            keyFar[channel] = 0.5f;
        }
        AudioFft* ctx = audioFftCreate(taps, channels, LFE, ring, ringLeft, ringRight);
        CHECK(ctx != NULL);
        if (ctx == NULL) {
            return;
        }
        for (int block = 0; block < 3; block++) {
            nextBlock(channels, taps, stride, frames);
            audioFftConvolve(ctx, ringHistory, stride, keyLow, keyHigh, keyFar, ringGains,
                             frames, 0, gotLeft, gotRight, gotOldLeft, gotOldRight);
            const float* in = ringHistory + taps - 1;
            int was = checksFailed;
            for (int frame = 0; frame < frames; frame++) {
                CHECK_NEAR(gotLeft[frame], in[frame], 1e-3);
                CHECK_NEAR(gotRight[frame], in[frame], 1e-3);
            }
            if (checksFailed != was) {
                fprintf(stderr, "  in %d frame block %d\n", frames, block);
            }
        }
        audioFftDestroy(ctx);
    }
}

// A head that has moved: the block comes back through the filters it has now
// and through the ones it had, for the crossfade
static void testFftKeepsTheOldFilters(void) {
    const int channels = 6;
    const int taps = KEMAR_TAPS;
    const int frames = 480;
    const int stride = taps - 1 + frames;
    const int ring = MOST_RING;
    srand(29);
    fillRing(ring, taps);
    fillHistory(channels, stride);
    fillGains(channels);
    fillKeys(channels, ring);
    AudioFft* ctx = audioFftCreate(taps, channels, LFE, ring, ringLeft, ringRight);
    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    int wasLow[MOST_CHANNELS];
    int wasHigh[MOST_CHANNELS];
    float wasFar[MOST_CHANNELS];

    // No block before this one, so the old output is this block's
    audioFftConvolve(ctx, ringHistory, stride, keyLow, keyHigh, keyFar, ringGains, frames, 1,
                     gotLeft, gotRight, gotOldLeft, gotOldRight);
    CHECK(memcmp(gotOldLeft, gotLeft, (size_t) frames * sizeof(float)) == 0);
    CHECK(memcmp(gotOldRight, gotRight, (size_t) frames * sizeof(float)) == 0);
    memcpy(wasLow, keyLow, sizeof(wasLow));
    memcpy(wasHigh, keyHigh, sizeof(wasHigh));
    memcpy(wasFar, keyFar, sizeof(wasFar));

    // A small turn moves one channel's mix
    nextBlock(channels, taps, stride, frames);
    keyFar[0] = keyFar[0] > 0.5f ? keyFar[0] - 0.25f : keyFar[0] + 0.25f;
    audioFftConvolve(ctx, ringHistory, stride, keyLow, keyHigh, keyFar, ringGains, frames, 1,
                     gotLeft, gotRight, gotOldLeft, gotOldRight);
    referenceFor(channels, LFE, taps, stride, frames);
    compare("moved head, filters now", gotLeft, wantLeft, frames);
    compare("moved head, filters now", gotRight, wantRight, frames);

    float nowFar = keyFar[0];
    memcpy(keyLow, wasLow, sizeof(wasLow));
    memcpy(keyHigh, wasHigh, sizeof(wasHigh));
    memcpy(keyFar, wasFar, sizeof(wasFar));
    referenceFor(channels, LFE, taps, stride, frames);
    compare("moved head, filters before", gotOldLeft, wantLeft, frames);
    compare("moved head, filters before", gotOldRight, wantRight, frames);
    keyFar[0] = nowFar;

    // A reset forgets both sets, so again the old output is this block's
    audioFftReset(ctx);
    audioFftConvolve(ctx, ringHistory, stride, keyLow, keyHigh, keyFar, ringGains, frames, 1,
                     gotLeft, gotRight, gotOldLeft, gotOldRight);
    CHECK(memcmp(gotOldLeft, gotLeft, (size_t) frames * sizeof(float)) == 0);
    CHECK(memcmp(gotOldRight, gotRight, (size_t) frames * sizeof(float)) == 0);
    audioFftDestroy(ctx);
}

// A still head reuses the filters it built, so the same block through them
// comes back identical to the bit
static void testFftCachesTheFilters(void) {
    const int channels = 8;
    const int taps = KEMAR_TAPS;
    const int frames = 240;
    const int stride = taps - 1 + frames;
    srand(31);
    fillRing(MOST_RING, taps);
    fillHistory(channels, stride);
    fillGains(channels);
    fillKeys(channels, MOST_RING);
    AudioFft* ctx = audioFftCreate(taps, channels, LFE, MOST_RING, ringLeft, ringRight);
    CHECK(ctx != NULL);
    if (ctx == NULL) {
        return;
    }
    audioFftConvolve(ctx, ringHistory, stride, keyLow, keyHigh, keyFar, ringGains, frames, 0,
                     wantLeft, wantRight, gotOldLeft, gotOldRight);
    audioFftConvolve(ctx, ringHistory, stride, keyLow, keyHigh, keyFar, ringGains, frames, 0,
                     gotLeft, gotRight, gotOldLeft, gotOldRight);
    CHECK(memcmp(gotLeft, wantLeft, (size_t) frames * sizeof(float)) == 0);
    CHECK(memcmp(gotRight, wantRight, (size_t) frames * sizeof(float)) == 0);
    audioFftDestroy(ctx);
}

static void testFftRefusesFiltersItCannotHold(void) {
    srand(37);
    fillRing(4, MOST_TAPS);
    // Half the transform or longer would leave no room for a single frame
    // after the wrap around
    CHECK(audioFftCreate(AUDIO_FFT_N / 2, 6, LFE, 4, ringLeft, ringRight) == NULL);
    CHECK(audioFftCreate(8, 6, LFE, 4, NULL, ringRight) == NULL);
    AudioFft* ctx = audioFftCreate(MOST_TAPS, 6, LFE, 4, ringLeft, ringRight);
    CHECK(ctx != NULL);
    audioFftDestroy(ctx);
    ctx = audioFftCreate(KEMAR_TAPS, 8, LFE, 4, ringLeft, ringRight);
    CHECK(ctx != NULL);
    audioFftDestroy(ctx);
}

// History filled up to what audioHistoryFloats asks for and poisoned after it
static float* poisonedHistory(long need) {
    float* padded = malloc((size_t) (need + 64) * sizeof(float));
    for (long i = 0; i < need + 64; i++) {
        padded[i] = i < need ? 1000.0f * nextValue() : NAN;
    }
    return padded;
}

static int allFinite(const float* values, int count) {
    for (int i = 0; i < count; i++) {
        if (!isfinite(values[i])) {
            return 0;
        }
    }
    return 1;
}

// The size the JNI layer holds a history buffer to is what the kernels read:
// the renderer's own layout fits it exactly, and nothing past it reaches the
// output of either path
static void testHistoryFloatsIsWhatIsRead(void) {
    CHECK(audioHistoryFloats(CHANNELS, STRIDE, TAPS, FRAMES) == CHANNELS * STRIDE);
    CHECK(audioHistoryFloats(8, KEMAR_TAPS - 1 + 480, KEMAR_TAPS, 480)
          == 8L * (KEMAR_TAPS - 1 + 480));
    CHECK(audioHistoryFloats(0, STRIDE, TAPS, FRAMES) == -1);
    CHECK(audioHistoryFloats(CHANNELS, -1, TAPS, FRAMES) == -1);
    CHECK(audioHistoryFloats(CHANNELS, STRIDE, 0, FRAMES) == -1);
    CHECK(audioHistoryFloats(CHANNELS, STRIDE, TAPS, -1) == -1);

    srand(43);
    fillEverything();
    const int wide = STRIDE + 9;
    long need = audioHistoryFloats(CHANNELS, wide, TAPS, FRAMES);
    CHECK(need == (long) (CHANNELS - 1) * wide + TAPS - 1 + FRAMES);
    float* padded = poisonedHistory(need);
    audioConvolve(padded, wide, filtersLeft, filtersRight, TAPS, gains, CHANNELS, LFE, FRAMES,
                  outLeft, outRight);
    CHECK(allFinite(outLeft, FRAMES) && allFinite(outRight, FRAMES));
    free(padded);

    // The FFT path, over more than one transform so every piece's window counts
    const int frames = 480;
    const int stride = KEMAR_TAPS - 1 + frames + 5;
    need = audioHistoryFloats(MOST_CHANNELS, stride, KEMAR_TAPS, frames);
    fillRing(8, KEMAR_TAPS);
    fillGains(MOST_CHANNELS);
    fillKeys(MOST_CHANNELS, 8);
    padded = poisonedHistory(need);
    AudioFft* ctx = audioFftCreate(KEMAR_TAPS, MOST_CHANNELS, LFE, 8, ringLeft, ringRight);
    CHECK(ctx != NULL);
    if (ctx != NULL) {
        audioFftConvolve(ctx, padded, stride, keyLow, keyHigh, keyFar, ringGains, frames, 0,
                         gotLeft, gotRight, gotOldLeft, gotOldRight);
        CHECK(allFinite(gotLeft, frames) && allFinite(gotRight, frames));
        audioFftDestroy(ctx);
    }
    free(padded);
}

static double microseconds(struct timespec from, struct timespec to) {
    return (to.tv_sec - from.tv_sec) * 1e6 + (to.tv_nsec - from.tv_nsec) / 1e3;
}

// Not a check: what a 7.1 packet costs each way on this machine, for the notes
static void timing(int frames) {
    const int channels = 8;
    const int taps = KEMAR_TAPS;
    const int stride = taps - 1 + frames;
    const int blocks = 2000;
    srand(41);
    fillRing(MOST_RING, taps);
    fillHistory(channels, stride);
    fillGains(channels);
    fillKeys(channels, MOST_RING);
    blendFilters(channels, LFE, taps);
    AudioFft* ctx = audioFftCreate(taps, channels, LFE, MOST_RING, ringLeft, ringRight);
    if (ctx == NULL) {
        return;
    }
    struct timespec from;
    struct timespec to;

    clock_gettime(CLOCK_MONOTONIC, &from);
    for (int block = 0; block < blocks; block++) {
        audioConvolve(ringHistory, stride, blendLeft, blendRight, taps, ringGains, channels, LFE,
                      frames, gotLeft, gotRight);
    }
    clock_gettime(CLOCK_MONOTONIC, &to);
    double direct = microseconds(from, to) / blocks;

    clock_gettime(CLOCK_MONOTONIC, &from);
    for (int block = 0; block < blocks; block++) {
        audioFftConvolve(ctx, ringHistory, stride, keyLow, keyHigh, keyFar, ringGains, frames, 0,
                         gotLeft, gotRight, gotOldLeft, gotOldRight);
    }
    clock_gettime(CLOCK_MONOTONIC, &to);
    double still = microseconds(from, to) / blocks;

    // A moving head: new filters every block, and the crossfade
    clock_gettime(CLOCK_MONOTONIC, &from);
    for (int block = 0; block < blocks; block++) {
        for (int channel = 0; channel < channels; channel++) {
            keyFar[channel] = (block % 16) / 16.0f;
        }
        audioFftConvolve(ctx, ringHistory, stride, keyLow, keyHigh, keyFar, ringGains, frames, 1,
                         gotLeft, gotRight, gotOldLeft, gotOldRight);
    }
    clock_gettime(CLOCK_MONOTONIC, &to);
    double moving = microseconds(from, to) / blocks;

    printf("audio timing: 7.1, %d taps, %d frames: direct %.1f us, fft %.1f us,"
           " fft turning with the crossfade %.1f us\n", taps, frames, direct, still, moving);
    audioFftDestroy(ctx);
}

int main(void) {
    testAgainstTheObviousLoop();
    testTheLfeIsNotPlaced();
    testChannelsAddUp();
    testFftMatchesTheKernel();
    testFftSplitsLongBlocks();
    testFftCarriesTheTail();
    testNoBlockDelay();
    testFftKeepsTheOldFilters();
    testFftCachesTheFilters();
    testFftRefusesFiltersItCannotHold();
    testHistoryFloatsIsWhatIsRead();
    timing(240);
    timing(480);
    return checksDone("xr_audio");
}

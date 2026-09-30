#include "xr_audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Output frames handled together in the direct kernel. The result does not
// depend on it; sixteen gave the compiler the best vectors.
#define BLOCK 16

void audioConvolve(const float* history, int historyStride, const float* filtersLeft,
                   const float* filtersRight, int taps, const float* gains, int channelCount,
                   int lfeChannel, int frames, float* outLeft, float* outRight) {
    for (int frame = 0; frame < frames; frame++) {
        outLeft[frame] = 0.0f;
        outRight[frame] = 0.0f;
    }
    for (int channel = 0; channel < channelCount; channel++) {
        const float* restrict samples = history + (long) channel * historyStride;
        const float gain = gains[channel];
        if (channel == lfeChannel) {
            for (int frame = 0; frame < frames; frame++) {
                const float sample = gain * samples[taps - 1 + frame];
                outLeft[frame] += sample;
                outRight[frame] += sample;
            }
            continue;
        }
        const float* restrict left = filtersLeft + (long) channel * taps;
        const float* restrict right = filtersRight + (long) channel * taps;
        int frame = 0;
        // One tap at a time across a run of frames. Each lane sums in the
        // same order the plain loop below would, so the two agree exactly,
        // and the inner loop is a straight vector multiply add.
        for (; frame + BLOCK <= frames; frame += BLOCK) {
            const float* restrict window = samples + frame;
            float sumLeft[BLOCK] = { 0.0f };
            float sumRight[BLOCK] = { 0.0f };
            for (int tap = 0; tap < taps; tap++) {
                const float earLeft = left[tap];
                const float earRight = right[tap];
                for (int lane = 0; lane < BLOCK; lane++) {
                    const float sample = window[tap + lane];
                    sumLeft[lane] += earLeft * sample;
                    sumRight[lane] += earRight * sample;
                }
            }
            for (int lane = 0; lane < BLOCK; lane++) {
                outLeft[frame + lane] += gain * sumLeft[lane];
                outRight[frame + lane] += gain * sumRight[lane];
            }
        }
        for (; frame < frames; frame++) {
            const float* restrict window = samples + frame;
            float sumLeft = 0.0f;
            float sumRight = 0.0f;
            for (int tap = 0; tap < taps; tap++) {
                sumLeft += left[tap] * window[tap];
                sumRight += right[tap] * window[tap];
            }
            outLeft[frame] += gain * sumLeft;
            outRight[frame] += gain * sumRight;
        }
    }
}

// The FFT version of the same sum.
//
// The twiddles, the bit reversal and the spectrum of every filter in the ring
// are all worked out when the context is made. Per block, each channel's
// filter is a mix of the two ring spectra either side of its azimuth (the
// transform is linear, so that is the spectrum of the mixed filter), the input
// goes through overlap save, the products are summed over the channels and a
// single inverse transform brings both ears back. Real signals travel through
// the complex transform in pairs and are separated again by symmetry, which
// halves the forward transforms.

// Bins 0 to N/2 of a real signal's spectrum, and the floats they take as
// interleaved real and imaginary parts
#define BINS (AUDIO_FFT_N / 2 + 1)
#define SPECTRUM (2 * BINS)

struct AudioFft {
    int taps;
    int channelCount;
    int lfeChannel;
    int ringSize;
    // Most output frames one transform can produce
    int pieceMax;
    // Every channel but the LFE
    int* placed;
    int placedCount;

    // Twiddles stage after stage, half the stage length each, N - 1 in all
    float* twiddleRe;
    float* twiddleIm;
    float* twiddleImInverse;
    int* reversal;

    // Ring entry i, ear e, at (2 i + e) SPECTRUM
    float* ringSpectra;
    // Mixed filters: the set in use and the set the block before used.
    // Channel c ear e at (2 c + e) SPECTRUM, with the ring entries and weight
    // each was mixed from, so an unchanged channel is not mixed again.
    float* setSpectra[2];
    int* setLow[2];
    int* setHigh[2];
    float* setFar[2];
    int current;
    int built;
    int haveOld;

    // Scratch, all allocated up front so a block never allocates
    float* workRe;
    float* workIm;
    float* channelSpectra;
    float* spare;
    float* earLeft;
    float* earRight;
};

static float* audioFftFloats(int count) {
    return (float*) malloc((size_t) count * sizeof(float));
}

static void audioFftTransform(const AudioFft* ctx, float* restrict re, float* restrict im,
                              int inverse) {
    const int* restrict reversal = ctx->reversal;
    for (int i = 0; i < AUDIO_FFT_N; i++) {
        const int j = reversal[i];
        if (i < j) {
            const float swapRe = re[i];
            const float swapIm = im[i];
            re[i] = re[j];
            im[i] = im[j];
            re[j] = swapRe;
            im[j] = swapIm;
        }
    }
    // The first two stages only ever multiply by 1 and by i, so they are done
    // together four points at a time as adds and swaps, without reading a
    // twiddle
    const float sign = inverse ? -1.0f : 1.0f;
    for (int i = 0; i < AUDIO_FFT_N; i += 4) {
        const float firstRe = re[i] + re[i + 1];
        const float firstIm = im[i] + im[i + 1];
        const float secondRe = re[i] - re[i + 1];
        const float secondIm = im[i] - im[i + 1];
        const float thirdRe = re[i + 2] + re[i + 3];
        const float thirdIm = im[i + 2] + im[i + 3];
        const float turnRe = sign * (re[i + 2] - re[i + 3]);
        const float turnIm = sign * (im[i + 2] - im[i + 3]);
        re[i] = firstRe + thirdRe;
        im[i] = firstIm + thirdIm;
        re[i + 2] = firstRe - thirdRe;
        im[i + 2] = firstIm - thirdIm;
        re[i + 1] = secondRe + turnIm;
        im[i + 1] = secondIm - turnRe;
        re[i + 3] = secondRe - turnIm;
        im[i + 3] = secondIm + turnRe;
    }
    const float* twiddleRe = ctx->twiddleRe;
    const float* twiddleIm = inverse ? ctx->twiddleImInverse : ctx->twiddleIm;
    // Skip the three twiddles of the two stages already done
    int at = 3;
    for (int length = 8; length <= AUDIO_FFT_N; length <<= 1) {
        const int half = length >> 1;
        const float* restrict wRe = twiddleRe + at;
        const float* restrict wIm = twiddleIm + at;
        for (int start = 0; start < AUDIO_FFT_N; start += length) {
            float* restrict aRe = re + start;
            float* restrict aIm = im + start;
            float* restrict bRe = aRe + half;
            float* restrict bIm = aIm + half;
            for (int k = 0; k < half; k++) {
                const float oddRe = bRe[k] * wRe[k] - bIm[k] * wIm[k];
                const float oddIm = bRe[k] * wIm[k] + bIm[k] * wRe[k];
                bRe[k] = aRe[k] - oddRe;
                bIm[k] = aIm[k] - oddIm;
                aRe[k] = aRe[k] + oddRe;
                aIm[k] = aIm[k] + oddIm;
            }
        }
        at += half;
    }
}

// Two real signals went through one transform, the first as the real part and
// the second as the imaginary part. A real signal's spectrum is conjugate
// symmetric, which is enough to pull the two apart.
static void audioFftSplit(const AudioFft* ctx, float* first, float* second) {
    const float* re = ctx->workRe;
    const float* im = ctx->workIm;
    for (int bin = 0; bin < BINS; bin++) {
        const int mirror = (AUDIO_FFT_N - bin) & (AUDIO_FFT_N - 1);
        const float atRe = re[bin];
        const float atIm = im[bin];
        const float mirrorRe = re[mirror];
        const float mirrorIm = im[mirror];
        first[2 * bin] = 0.5f * (atRe + mirrorRe);
        first[2 * bin + 1] = 0.5f * (atIm - mirrorIm);
        second[2 * bin] = 0.5f * (atIm + mirrorIm);
        second[2 * bin + 1] = 0.5f * (mirrorRe - atRe);
    }
}

// Zero pads two signals into the scratch, transforms them together and splits
// the result. A NULL signal is silence.
static void audioFftPair(AudioFft* ctx, const float* first, const float* second, int length,
                         int reversedIn, float* firstOut, float* secondOut) {
    const float* signals[2] = { first, second };
    float* into[2] = { ctx->workRe, ctx->workIm };
    for (int half = 0; half < 2; half++) {
        float* work = into[half];
        const float* from = signals[half];
        if (from == NULL) {
            memset(work, 0, AUDIO_FFT_N * sizeof(float));
            continue;
        }
        if (reversedIn) {
            // Ring filters are stored reversed; the spectrum needs them in
            // time order
            for (int i = 0; i < length; i++) {
                work[i] = from[length - 1 - i];
            }
        }
        else {
            memcpy(work, from, (size_t) length * sizeof(float));
        }
        memset(work + length, 0, (size_t) (AUDIO_FFT_N - length) * sizeof(float));
    }
    audioFftTransform(ctx, ctx->workRe, ctx->workIm, 0);
    audioFftSplit(ctx, firstOut, secondOut);
}

// One channel's filter pair: the ring spectra either side of its azimuth,
// mixed by how far it sits between them
static void audioFftBlend(AudioFft* ctx, int set, int channel, int low, int high, float far) {
    const float near = 1.0f - far;
    for (int ear = 0; ear < 2; ear++) {
        const float* from = ctx->ringSpectra + (long) (2 * low + ear) * SPECTRUM;
        const float* to = ctx->ringSpectra + (long) (2 * high + ear) * SPECTRUM;
        float* into = ctx->setSpectra[set] + (long) (2 * channel + ear) * SPECTRUM;
        for (int i = 0; i < SPECTRUM; i++) {
            into[i] = near * from[i] + far * to[i];
        }
    }
}

static int audioFftClamp(int index, int count) {
    if (index < 0) {
        return 0;
    }
    return index >= count ? count - 1 : index;
}

// Brings the filters up to date with where each speaker now falls. As soon as
// anything differs the two sets swap roles, so the set the last block used is
// still there for the crossfade without a copy.
static void audioFftFilters(AudioFft* ctx, const int* low, const int* high, const float* far) {
    int changed = 0;
    int set = ctx->current;
    for (int i = 0; i < ctx->placedCount && !changed; i++) {
        const int channel = ctx->placed[i];
        changed = ctx->setLow[set][channel] != low[channel]
                || ctx->setHigh[set][channel] != high[channel]
                || ctx->setFar[set][channel] != far[channel];
    }
    if (!changed) {
        return;
    }
    if (ctx->built) {
        ctx->current = 1 - ctx->current;
        ctx->haveOld = 1;
        set = ctx->current;
    }
    for (int i = 0; i < ctx->placedCount; i++) {
        const int channel = ctx->placed[i];
        if (ctx->setLow[set][channel] == low[channel] && ctx->setHigh[set][channel] == high[channel]
                && ctx->setFar[set][channel] == far[channel]) {
            continue;
        }
        audioFftBlend(ctx, set, channel, audioFftClamp(low[channel], ctx->ringSize),
                      audioFftClamp(high[channel], ctx->ringSize), far[channel]);
        ctx->setLow[set][channel] = low[channel];
        ctx->setHigh[set][channel] = high[channel];
        ctx->setFar[set][channel] = far[channel];
    }
    ctx->built = 1;
}

// Multiplies every channel by its filters, sums the ears, and brings both ears
// back through one inverse transform: the left ear's spectrum rides as the
// real part and the right ear's as the imaginary part.
static void audioFftEars(AudioFft* ctx, const float* spectra, int frames, float* outLeft,
                         float* outRight) {
    float* earLeft = ctx->earLeft;
    float* earRight = ctx->earRight;
    memset(earLeft, 0, SPECTRUM * sizeof(float));
    memset(earRight, 0, SPECTRUM * sizeof(float));
    for (int i = 0; i < ctx->placedCount; i++) {
        const int channel = ctx->placed[i];
        const float* restrict signal = ctx->channelSpectra + (long) channel * SPECTRUM;
        const float* restrict left = spectra + (long) (2 * channel) * SPECTRUM;
        const float* restrict right = spectra + (long) (2 * channel + 1) * SPECTRUM;
        for (int bin = 0; bin < BINS; bin++) {
            const float signalRe = signal[2 * bin];
            const float signalIm = signal[2 * bin + 1];
            earLeft[2 * bin] += signalRe * left[2 * bin] - signalIm * left[2 * bin + 1];
            earLeft[2 * bin + 1] += signalRe * left[2 * bin + 1] + signalIm * left[2 * bin];
            earRight[2 * bin] += signalRe * right[2 * bin] - signalIm * right[2 * bin + 1];
            earRight[2 * bin + 1] += signalRe * right[2 * bin + 1] + signalIm * right[2 * bin];
        }
    }
    // Each ear is real, so the bins above N/2 are its mirror, and the pair
    // packs into one full spectrum
    float* re = ctx->workRe;
    float* im = ctx->workIm;
    for (int bin = 0; bin < BINS; bin++) {
        const float leftRe = earLeft[2 * bin];
        const float leftIm = earLeft[2 * bin + 1];
        const float rightRe = earRight[2 * bin];
        const float rightIm = earRight[2 * bin + 1];
        re[bin] = leftRe - rightIm;
        im[bin] = leftIm + rightRe;
        if (bin > 0 && bin < AUDIO_FFT_N / 2) {
            re[AUDIO_FFT_N - bin] = leftRe + rightIm;
            im[AUDIO_FFT_N - bin] = rightRe - leftIm;
        }
    }
    audioFftTransform(ctx, re, im, 1);
    // Overlap save: the first taps - 1 outputs have wrapped around and are
    // dropped, the rest are the block's output
    const float scale = 1.0f / AUDIO_FFT_N;
    const int from = ctx->taps - 1;
    for (int frame = 0; frame < frames; frame++) {
        outLeft[frame] = scale * re[from + frame];
        outRight[frame] = scale * im[from + frame];
    }
}

AudioFft* audioFftCreate(int taps, int channelCount, int lfeChannel, int ringSize,
                         const float* ringLeft, const float* ringRight) {
    if (taps < 1 || taps >= AUDIO_FFT_N / 2 || channelCount < 1 || ringSize < 1
            || ringLeft == NULL || ringRight == NULL) {
        return NULL;
    }
    AudioFft* ctx = (AudioFft*) calloc(1, sizeof(AudioFft));
    if (ctx == NULL) {
        return NULL;
    }
    ctx->taps = taps;
    ctx->channelCount = channelCount;
    ctx->lfeChannel = lfeChannel;
    ctx->ringSize = ringSize;
    ctx->pieceMax = AUDIO_FFT_N - taps + 1;
    ctx->placed = (int*) malloc((size_t) channelCount * sizeof(int));
    ctx->twiddleRe = audioFftFloats(AUDIO_FFT_N);
    ctx->twiddleIm = audioFftFloats(AUDIO_FFT_N);
    ctx->twiddleImInverse = audioFftFloats(AUDIO_FFT_N);
    ctx->reversal = (int*) malloc(AUDIO_FFT_N * sizeof(int));
    ctx->ringSpectra = audioFftFloats(2 * ringSize * SPECTRUM);
    ctx->workRe = audioFftFloats(AUDIO_FFT_N);
    ctx->workIm = audioFftFloats(AUDIO_FFT_N);
    ctx->channelSpectra = audioFftFloats(channelCount * SPECTRUM);
    ctx->spare = audioFftFloats(SPECTRUM);
    ctx->earLeft = audioFftFloats(SPECTRUM);
    ctx->earRight = audioFftFloats(SPECTRUM);
    int ok = ctx->placed != NULL && ctx->twiddleRe != NULL && ctx->twiddleIm != NULL
            && ctx->twiddleImInverse != NULL && ctx->reversal != NULL && ctx->ringSpectra != NULL
            && ctx->workRe != NULL && ctx->workIm != NULL && ctx->channelSpectra != NULL
            && ctx->spare != NULL && ctx->earLeft != NULL && ctx->earRight != NULL;
    for (int set = 0; set < 2; set++) {
        ctx->setSpectra[set] = audioFftFloats(2 * channelCount * SPECTRUM);
        ctx->setLow[set] = (int*) malloc((size_t) channelCount * sizeof(int));
        ctx->setHigh[set] = (int*) malloc((size_t) channelCount * sizeof(int));
        ctx->setFar[set] = audioFftFloats(channelCount);
        ok = ok && ctx->setSpectra[set] != NULL && ctx->setLow[set] != NULL
                && ctx->setHigh[set] != NULL && ctx->setFar[set] != NULL;
    }
    if (!ok) {
        audioFftDestroy(ctx);
        return NULL;
    }

    ctx->placedCount = 0;
    for (int channel = 0; channel < channelCount; channel++) {
        if (channel != lfeChannel) {
            ctx->placed[ctx->placedCount++] = channel;
        }
    }
    int at = 0;
    for (int length = 2; length <= AUDIO_FFT_N; length <<= 1) {
        const int half = length >> 1;
        for (int k = 0; k < half; k++) {
            const double angle = -2.0 * M_PI * k / length;
            ctx->twiddleRe[at + k] = (float) cos(angle);
            ctx->twiddleIm[at + k] = (float) sin(angle);
            ctx->twiddleImInverse[at + k] = (float) -sin(angle);
        }
        at += half;
    }
    int bits = 0;
    while ((1 << bits) < AUDIO_FFT_N) {
        bits++;
    }
    for (int i = 0; i < AUDIO_FFT_N; i++) {
        int reversed = 0;
        for (int bit = 0; bit < bits; bit++) {
            if (i & (1 << bit)) {
                reversed |= 1 << (bits - 1 - bit);
            }
        }
        ctx->reversal[i] = reversed;
    }
    for (int entry = 0; entry < ringSize; entry++) {
        audioFftPair(ctx, ringLeft + (long) entry * taps, ringRight + (long) entry * taps, taps, 1,
                     ctx->ringSpectra + (long) (2 * entry) * SPECTRUM,
                     ctx->ringSpectra + (long) (2 * entry + 1) * SPECTRUM);
    }
    audioFftReset(ctx);
    return ctx;
}

void audioFftDestroy(AudioFft* ctx) {
    if (ctx == NULL) {
        return;
    }
    free(ctx->placed);
    free(ctx->twiddleRe);
    free(ctx->twiddleIm);
    free(ctx->twiddleImInverse);
    free(ctx->reversal);
    free(ctx->ringSpectra);
    free(ctx->workRe);
    free(ctx->workIm);
    free(ctx->channelSpectra);
    free(ctx->spare);
    free(ctx->earLeft);
    free(ctx->earRight);
    for (int set = 0; set < 2; set++) {
        free(ctx->setSpectra[set]);
        free(ctx->setLow[set]);
        free(ctx->setHigh[set]);
        free(ctx->setFar[set]);
    }
    free(ctx);
}

void audioFftReset(AudioFft* ctx) {
    if (ctx == NULL) {
        return;
    }
    ctx->current = 0;
    ctx->built = 0;
    ctx->haveOld = 0;
    for (int set = 0; set < 2; set++) {
        for (int channel = 0; channel < ctx->channelCount; channel++) {
            ctx->setLow[set][channel] = -1;
            ctx->setHigh[set][channel] = -1;
            ctx->setFar[set][channel] = 0.0f;
        }
    }
}

void audioFftConvolve(AudioFft* ctx, const float* history, int historyStride, const int* low,
                      const int* high, const float* far, const float* gains, int frames,
                      int withOld, float* outLeft, float* outRight, float* oldLeft,
                      float* oldRight) {
    if (ctx == NULL || frames < 1) {
        return;
    }
    audioFftFilters(ctx, low, high, far);
    const float* current = ctx->setSpectra[ctx->current];
    const float* old = ctx->haveOld ? ctx->setSpectra[1 - ctx->current] : current;
    const int window = ctx->taps - 1;
    // A block longer than one transform can hold is done in pieces, each
    // reading its own window of history, so nothing waits for a later call
    for (int offset = 0; offset < frames; offset += ctx->pieceMax) {
        const int piece = frames - offset < ctx->pieceMax ? frames - offset : ctx->pieceMax;
        // Channels go through two to a transform, with an odd one out alone
        for (int i = 0; i < ctx->placedCount; i += 2) {
            const int first = ctx->placed[i];
            const int second = i + 1 < ctx->placedCount ? ctx->placed[i + 1] : -1;
            const float* firstIn = history + (long) first * historyStride + offset;
            const float* secondIn = second < 0
                    ? NULL : history + (long) second * historyStride + offset;
            float* firstOut = ctx->channelSpectra + (long) first * SPECTRUM;
            float* secondOut = second < 0
                    ? ctx->spare : ctx->channelSpectra + (long) second * SPECTRUM;
            audioFftPair(ctx, firstIn, secondIn, window + piece, 0, firstOut, secondOut);
            // Applying the gain to the signal's spectrum is one multiply a bin
            // rather than one per ear
            for (int bin = 0; bin < SPECTRUM; bin++) {
                firstOut[bin] *= gains[first];
            }
            if (second >= 0) {
                for (int bin = 0; bin < SPECTRUM; bin++) {
                    secondOut[bin] *= gains[second];
                }
            }
        }
        audioFftEars(ctx, current, piece, outLeft + offset, outRight + offset);
        if (withOld) {
            audioFftEars(ctx, old, piece, oldLeft + offset, oldRight + offset);
        }
    }
    if (ctx->lfeChannel >= 0 && ctx->lfeChannel < ctx->channelCount) {
        const float* samples = history + (long) ctx->lfeChannel * historyStride + window;
        const float gain = gains[ctx->lfeChannel];
        for (int frame = 0; frame < frames; frame++) {
            const float sample = gain * samples[frame];
            outLeft[frame] += sample;
            outRight[frame] += sample;
        }
        if (withOld) {
            for (int frame = 0; frame < frames; frame++) {
                const float sample = gain * samples[frame];
                oldLeft[frame] += sample;
                oldRight[frame] += sample;
            }
        }
    }
}

#ifdef __ANDROID__

#include <jni.h>
#include <stdint.h>

JNIEXPORT void JNICALL
Java_com_limelight_binding_audio_NativeConvolver_convolve(
        JNIEnv* env, jclass clazz, jobject history, jint historyStride, jobject filtersLeft,
        jobject filtersRight, jint taps, jobject gains, jint channelCount, jint lfeChannel,
        jint frames, jobject outLeft, jobject outRight) {
    (void) clazz;
    const float* historyAt = (*env)->GetDirectBufferAddress(env, history);
    const float* leftAt = (*env)->GetDirectBufferAddress(env, filtersLeft);
    const float* rightAt = (*env)->GetDirectBufferAddress(env, filtersRight);
    const float* gainsAt = (*env)->GetDirectBufferAddress(env, gains);
    float* outLeftAt = (*env)->GetDirectBufferAddress(env, outLeft);
    float* outRightAt = (*env)->GetDirectBufferAddress(env, outRight);
    if (historyAt == NULL || leftAt == NULL || rightAt == NULL || gainsAt == NULL
            || outLeftAt == NULL || outRightAt == NULL) {
        return;
    }
    audioConvolve(historyAt, historyStride, leftAt, rightAt, taps, gainsAt, channelCount,
                  lfeChannel, frames, outLeftAt, outRightAt);
}

JNIEXPORT jlong JNICALL
Java_com_limelight_binding_audio_NativeConvolver_createFft(
        JNIEnv* env, jclass clazz, jint taps, jint channelCount, jint lfeChannel, jint ringSize,
        jobject ringLeft, jobject ringRight) {
    (void) clazz;
    const float* leftAt = (*env)->GetDirectBufferAddress(env, ringLeft);
    const float* rightAt = (*env)->GetDirectBufferAddress(env, ringRight);
    if (leftAt == NULL || rightAt == NULL) {
        return 0;
    }
    AudioFft* ctx = audioFftCreate(taps, channelCount, lfeChannel, ringSize, leftAt, rightAt);
    return (jlong) (intptr_t) ctx;
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_audio_NativeConvolver_destroyFft(
        JNIEnv* env, jclass clazz, jlong handle) {
    (void) env;
    (void) clazz;
    audioFftDestroy((AudioFft*) (intptr_t) handle);
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_audio_NativeConvolver_resetFft(
        JNIEnv* env, jclass clazz, jlong handle) {
    (void) env;
    (void) clazz;
    audioFftReset((AudioFft*) (intptr_t) handle);
}

JNIEXPORT void JNICALL
Java_com_limelight_binding_audio_NativeConvolver_convolveFft(
        JNIEnv* env, jclass clazz, jlong handle, jobject history, jint historyStride, jobject low,
        jobject high, jobject far, jobject gains, jint frames, jboolean withOld, jobject outLeft,
        jobject outRight, jobject oldLeft, jobject oldRight) {
    (void) clazz;
    AudioFft* ctx = (AudioFft*) (intptr_t) handle;
    const float* historyAt = (*env)->GetDirectBufferAddress(env, history);
    const int* lowAt = (*env)->GetDirectBufferAddress(env, low);
    const int* highAt = (*env)->GetDirectBufferAddress(env, high);
    const float* farAt = (*env)->GetDirectBufferAddress(env, far);
    const float* gainsAt = (*env)->GetDirectBufferAddress(env, gains);
    float* outLeftAt = (*env)->GetDirectBufferAddress(env, outLeft);
    float* outRightAt = (*env)->GetDirectBufferAddress(env, outRight);
    float* oldLeftAt = (*env)->GetDirectBufferAddress(env, oldLeft);
    float* oldRightAt = (*env)->GetDirectBufferAddress(env, oldRight);
    if (ctx == NULL || historyAt == NULL || lowAt == NULL || highAt == NULL || farAt == NULL
            || gainsAt == NULL || outLeftAt == NULL || outRightAt == NULL || oldLeftAt == NULL
            || oldRightAt == NULL) {
        return;
    }
    audioFftConvolve(ctx, historyAt, historyStride, lowAt, highAt, farAt, gainsAt, frames,
                     withOld ? 1 : 0, outLeftAt, outRightAt, oldLeftAt, oldRightAt);
}

#endif

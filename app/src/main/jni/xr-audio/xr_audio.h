// The convolution behind the virtual surround, which is where nearly all of
// its time goes. The rest of it (the speaker layout, the head yaw, the rear
// treatment and the filters themselves) lives in Java.
#ifndef XR_AUDIO_H
#define XR_AUDIO_H

/**
 * Convolves each channel with its own pair of filters and sums them into a
 * left and a right ear.
 *
 * history holds every channel back to back, historyStride floats apiece: the
 * last taps - 1 samples of the previous block, then this block's. The filters
 * are channelCount rows of taps floats, stored reversed so the filter and the
 * samples are both read forwards. The LFE channel is never filtered and goes
 * to both ears at its gain. Both outputs are overwritten over frames.
 */
void audioConvolve(const float* history, int historyStride, const float* filtersLeft,
                   const float* filtersRight, int taps, const float* gains, int channelCount,
                   int lfeChannel, int frames, float* outLeft, float* outRight);

/**
 * How many floats history has to hold for either convolution to read frames
 * of every channel: a stride for each channel but the last, then the last
 * one's taps - 1 samples from the block before and its frames. -1 for
 * arguments no call could be made with.
 */
long audioHistoryFloats(int channelCount, int historyStride, int taps, int frames);

/**
 * Transform size of the overlap save convolution. Sized for the stream's 5 ms
 * packets: 240 frames plus a 175 tap filter's 174 frames of history fit in
 * one transform, and a 10 ms packet goes through in two. Filters must be
 * shorter than half of it.
 */
#define AUDIO_FFT_N 512

/** What the FFT convolution keeps from one block to the next. */
typedef struct AudioFft AudioFft;

/**
 * Sets the FFT convolution up for one layout and takes the spectrum of every
 * filter in the ring, so blocks only ever mix spectra that already exist.
 *
 * ringLeft and ringRight are ringSize filters of taps floats each, reversed as
 * audioConvolve takes them. Returns NULL when the filters are too long for the
 * transform or an allocation fails, in which case audioConvolve does the work.
 */
AudioFft* audioFftCreate(int taps, int channelCount, int lfeChannel, int ringSize,
                         const float* ringLeft, const float* ringRight);

void audioFftDestroy(AudioFft* ctx);

/** Drops the filters built so far, so the next block starts clean. */
void audioFftReset(AudioFft* ctx);

/**
 * audioConvolve's sum, worked in the frequency domain.
 *
 * history is laid out as for audioConvolve. Each channel's filter is ring
 * entry low mixed with ring entry high by far, as the azimuth lookup reports
 * them, and the LFE channel's entries are not read. With withOld set the block
 * also goes through the previous block's filters, into oldLeft and oldRight,
 * for a crossfade; until there is a previous block those match outLeft and
 * outRight. All frames given come back in the same call.
 */
void audioFftConvolve(AudioFft* ctx, const float* history, int historyStride, const int* low,
                      const int* high, const float* far, const float* gains, int frames,
                      int withOld, float* outLeft, float* outRight, float* oldLeft,
                      float* oldRight);

#endif

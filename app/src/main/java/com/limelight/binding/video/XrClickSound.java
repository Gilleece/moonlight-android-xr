package com.limelight.binding.video;

import android.annotation.TargetApi;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.os.Build;

import com.limelight.LimeLog;

/**
 * The tick a press on the headset's panels makes. A pinch or a look has
 * nothing under a finger to say it landed, so the sound does. Made here rather
 * than shipped, on a small track of its own on the system sounds, so the
 * stream's audio and its latency are never touched.
 *
 * The track is built once and kept playing. A streaming track only starts
 * once its buffer has been filled, so it is filled with silence to begin
 * with, and filled again should it ever stall. After that the frame loop keeps
 * a little silence queued ahead of it, so it never runs dry and stops, and a
 * press puts the tick in behind that. Every write from the frame loop is a non
 * blocking one into the track's own buffer, so a busy audio system can never
 * hold up a frame. Only ever made on M and later, where the non blocking
 * write arrived, which the caller checks; every headset is well past it.
 */
@TargetApi(Build.VERSION_CODES.M)
final class XrClickSound {
    private static final int RATE = 48000;
    // Silence kept queued: a couple of frames' worth and a mixer cycle, which
    // is also about as late as the tick can start after a press. More while
    // the frame loop runs slow, as it does while the session loads: the gap
    // since the last feed and a mixer cycle on top.
    private static final int QUEUE_FRAMES = RATE * 30 / 1000;
    private static final int MIXER_FRAMES = RATE * 20 / 1000;
    // Short and quiet: a press, not an alarm
    private static final int TICK_MS = 18;
    private static final double TICK_HZ = 2200.0;
    private static final double TICK_BODY_HZ = 600.0;
    private static final double DECAY_SEC = 0.0035;
    private static final double PEAK = 0.28;
    // Frames the head can sit still for, fed all the while, before the track
    // is taken to have stopped and its buffer is filled to start it again:
    // about a third of a second of frames at 72 Hz or more
    private static final int STALL_FEEDS = 30;

    private final AudioTrack track;
    private final short[] tick;
    private final short[] silence;
    private final int bufferFrames;
    // How much silence is kept queued: QUEUE_FRAMES, or less where the
    // track's buffer would not then have room for a whole tick behind it
    private final int queueFrames;
    private long writtenFrames;
    private int clicks;
    private int lastHead;
    private int stillFeeds;
    private int restarts;
    private long lastFeedNs;

    private XrClickSound(AudioTrack track, short[] tick, int bufferFrames, int queueFrames) {
        this.track = track;
        this.tick = tick;
        this.bufferFrames = bufferFrames;
        this.queueFrames = queueFrames;
        this.silence = new short[bufferFrames];
    }

    /**
     * Builds the track and starts it on a queue of silence, or null where it
     * cannot be made. Talks to the audio system, so never on the frame loop.
     */
    static XrClickSound start() {
        short[] tick = tickSamples(RATE);
        int min = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        int bytes = Math.max(min, 4 * (QUEUE_FRAMES + tick.length));
        AudioTrack track;
        try {
            track = new AudioTrack.Builder()
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_ASSISTANCE_SONIFICATION)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                            .build())
                    .setAudioFormat(new AudioFormat.Builder()
                            .setSampleRate(RATE)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .build())
                    .setBufferSizeInBytes(bytes)
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .build();
        } catch (IllegalArgumentException | UnsupportedOperationException e) {
            LimeLog.warning("Click sound unavailable: " + e);
            return null;
        }
        if (track.getState() != AudioTrack.STATE_INITIALIZED) {
            track.release();
            LimeLog.warning("Click sound unavailable: track not initialised");
            return null;
        }
        // The buffer in use can start smaller than the one asked for, and a
        // tick written into too little room is cut short, so it is opened up
        // to all there is and the silence kept within what is left
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            track.setBufferSizeInFrames(track.getBufferCapacityInFrames());
        }
        int frames = track.getBufferSizeInFrames();
        int queue = queueFor(frames, tick.length);
        XrClickSound sound = new XrClickSound(track, tick, frames, queue);
        sound.write(sound.silence, frames);
        track.play();
        LimeLog.info("Click sound ready: " + tick.length + " frame tick, " + queue
                + " frames of silence queued, buffer " + frames + " frames");
        return sound;
    }

    /**
     * Frame loop, every frame: tops the queue of silence back up to where it
     * is kept. Never blocks.
     */
    void feed() {
        int head = track.getPlaybackHeadPosition();
        stillFeeds = head == lastHead ? stillFeeds + 1 : 0;
        lastHead = head;
        if (stillFeeds == STALL_FEEDS) {
            // Stalled, after a long stop of the frame loop or the audio
            // system's own: filled up so it starts again
            restarts++;
            write(silence, framesToQueue(writtenFrames, head, bufferFrames));
            LimeLog.info("Click sound: track stalled, filled to start it again (" + restarts + ")");
            return;
        }
        long now = System.nanoTime();
        int gap = lastFeedNs == 0 ? 0 : (int)Math.min(bufferFrames, (now - lastFeedNs) * RATE / 1000000000L);
        lastFeedNs = now;
        int target = queueTarget(queueFrames, gap + MIXER_FRAMES, bufferFrames, tick.length);
        int want = framesToQueue(writtenFrames, head, target);
        if (want > 0) {
            write(silence, want);
        }
    }

    /** Frame loop: ticks once, behind the silence already queued. Never blocks. */
    void click() {
        int queued = queueFrames - framesToQueue(writtenFrames, track.getPlaybackHeadPosition(),
                queueFrames);
        int wrote = write(tick, tick.length);
        clicks++;
        LimeLog.info("Click sound: press " + clicks + ", " + wrote + " of " + tick.length
                + " frames written, " + (queued * 1000 / RATE) + " ms queued ahead"
                + (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N
                        ? ", underruns " + track.getUnderrunCount() : ""));
    }

    void release() {
        try {
            track.stop();
        } catch (IllegalStateException ignored) {
        }
        track.release();
    }

    private int write(short[] samples, int frames) {
        int wrote = track.write(samples, 0, frames, AudioTrack.WRITE_NON_BLOCKING);
        if (wrote > 0) {
            writtenFrames += wrote;
        }
        return wrote;
    }

    /**
     * How much silence to keep queued in a buffer of that many frames, so a
     * tick always fits behind it with a little to spare: QUEUE_FRAMES where
     * there is room, never under a tenth of it.
     */
    static int queueFor(int bufferFrames, int tickFrames) {
        int room = bufferFrames - tickFrames - tickFrames / 4;
        return Math.max(QUEUE_FRAMES / 10, Math.min(QUEUE_FRAMES, room));
    }

    /**
     * The silence to keep queued this frame: the usual amount, or enough to
     * last until the next feed when the frame loop is running slow, but never
     * so much that a tick no longer fits behind it.
     */
    static int queueTarget(int usual, int needed, int bufferFrames, int tickFrames) {
        int most = Math.max(usual, bufferFrames - tickFrames - tickFrames / 4);
        return Math.min(most, Math.max(usual, needed));
    }

    /**
     * How many frames to write to bring what is queued back up to the target.
     * The track counts what it has played in an int that wraps, so what has
     * been written is taken the same way before the two are compared.
     */
    static int framesToQueue(long written, int playedHead, int target) {
        long played = playedHead & 0xFFFFFFFFL;
        long queued = (written & 0xFFFFFFFFL) - played;
        if (queued < 0) {
            queued += 1L << 32;
        }
        long want = target - queued;
        return want > 0 ? (int)want : 0;
    }

    /**
     * The tick: a bright partial over a lower body, both dying away within a
     * few milliseconds, with a short ramp on the front so the speaker is not
     * asked to jump. Ends on silence.
     */
    static short[] tickSamples(int rate) {
        int frames = rate * TICK_MS / 1000;
        short[] out = new short[frames];
        int attack = Math.max(1, rate / 2000);
        for (int i = 0; i < frames; i++) {
            double t = (double)i / rate;
            double envelope = Math.exp(-t / DECAY_SEC);
            if (i < attack) {
                envelope *= (double)i / attack;
            }
            // Down to nothing over the last fifth, whatever the decay left
            int tail = frames - i;
            if (tail < frames / 5) {
                envelope *= (double)tail / (frames / 5);
            }
            double value = 0.7 * Math.sin(2.0 * Math.PI * TICK_HZ * t)
                    + 0.3 * Math.sin(2.0 * Math.PI * TICK_BODY_HZ * t);
            out[i] = (short)Math.round(PEAK * envelope * value * Short.MAX_VALUE);
        }
        return out;
    }
}

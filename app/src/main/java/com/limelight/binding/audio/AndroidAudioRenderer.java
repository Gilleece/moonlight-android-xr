package com.limelight.binding.audio;

import android.content.Context;
import android.content.Intent;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.audiofx.AudioEffect;
import android.os.Build;
import android.os.Process;
import android.os.SystemClock;

import com.limelight.FileLog;
import com.limelight.LimeLog;
import com.limelight.nvstream.av.audio.AudioRenderer;
import com.limelight.nvstream.jni.MoonBridge;

import java.io.IOException;
import java.util.Locale;

public class AndroidAudioRenderer implements AudioRenderer {

    private final Context context;
    private final boolean enableAudioFx;
    private final boolean virtualSurround;
    private final HeadYaw headYaw;

    private AudioTrack track;
    // Set only while a 5.1 or 7.1 stream is being rendered to stereo
    private VirtualSurround surround;
    // The track's underruns for the stats, null where the platform cannot
    // say (before Android 7)
    private volatile UnderrunCounter underruns;
    // Set on the first block, which is when the audio thread is known
    private boolean priorityRaised;

    public AndroidAudioRenderer(Context context, boolean enableAudioFx, boolean virtualSurround,
                                HeadYaw headYaw) {
        this.context = context;
        this.enableAudioFx = enableAudioFx;
        this.virtualSurround = virtualSurround;
        this.headYaw = headYaw;
    }

    /** Reads the KEMAR set out of the assets. */
    static Hrtf.Loader assetLoader(Context context) {
        final Context app = context.getApplicationContext();
        return name -> app.getAssets().open(VirtualSurround.ASSET_DIR + name);
    }

    // Loads the filters and builds the renderer. A failure leaves the stream
    // on the multichannel track it would have had with the setting off.
    private VirtualSurround buildSurround(int channelCount, int sampleRate, int samplesPerFrame) {
        long start = SystemClock.elapsedRealtime();
        VirtualSurround built = null;
        try {
            float rearMs = DebugProps.getFloat(DebugProps.SURROUND_REAR_MS,
                    BinauralRenderer.REAR_DELAY_MS);
            built = VirtualSurround.create(channelCount, sampleRate, samplesPerFrame,
                    assetLoader(context), headYaw, rearMs);
            // The Java loop is only there for the host tests. On a headset
            // it could not keep up with the stream.
            if ("Java".equals(built.kernel())) {
                throw new IOException("native convolution unavailable");
            }
            LimeLog.info(String.format(Locale.US,
                    "Virtual surround: %d channels to stereo, %s kernel, %d taps, rear delay %.1f ms, built in %d ms",
                    channelCount, built.kernel(), built.taps(), rearMs,
                    SystemClock.elapsedRealtime() - start));
            return built;
        } catch (IOException | RuntimeException | LinkageError e) {
            if (built != null) {
                built.release();
            }
            LimeLog.warning("Virtual surround unavailable, playing "+channelCount+" channels as they come: "+e);
            return null;
        }
    }

    private AudioTrack createAudioTrack(int channelConfig, int sampleRate, int bufferSize, boolean lowLatency) {
        AudioAttributes.Builder attributesBuilder = new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_GAME);
        AudioFormat format = new AudioFormat.Builder()
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setSampleRate(sampleRate)
                .setChannelMask(channelConfig)
                .build();

        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            // Use FLAG_LOW_LATENCY on L through N
            if (lowLatency) {
                attributesBuilder.setFlags(AudioAttributes.FLAG_LOW_LATENCY);
            }
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            AudioTrack.Builder trackBuilder = new AudioTrack.Builder()
                    .setAudioFormat(format)
                    .setAudioAttributes(attributesBuilder.build())
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .setBufferSizeInBytes(bufferSize);

            // Use PERFORMANCE_MODE_LOW_LATENCY on O and later
            if (lowLatency) {
                trackBuilder.setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY);
            }

            return trackBuilder.build();
        }
        else {
            return new AudioTrack(attributesBuilder.build(),
                    format,
                    bufferSize,
                    AudioTrack.MODE_STREAM,
                    AudioManager.AUDIO_SESSION_ID_GENERATE);
        }
    }

    @Override
    public int setup(MoonBridge.AudioConfiguration audioConfiguration, int sampleRate, int samplesPerFrame) {
        int channelConfig;
        int bytesPerFrame;

        switch (audioConfiguration.channelCount)
        {
            case 2:
                channelConfig = AudioFormat.CHANNEL_OUT_STEREO;
                break;
            case 4:
                channelConfig = AudioFormat.CHANNEL_OUT_QUAD;
                break;
            case 6:
                channelConfig = AudioFormat.CHANNEL_OUT_5POINT1;
                break;
            case 8:
                // AudioFormat.CHANNEL_OUT_7POINT1_SURROUND isn't available until Android 6.0,
                // yet the CHANNEL_OUT_SIDE_LEFT and CHANNEL_OUT_SIDE_RIGHT constants were added
                // in 5.0, so just hardcode the constant so we can work on Lollipop.
                channelConfig = 0x000018fc; // AudioFormat.CHANNEL_OUT_7POINT1_SURROUND
                break;
            default:
                LimeLog.severe("Decoder returned unhandled channel count");
                return -1;
        }

        // Virtual surround renders 5.1 and 7.1 to two ears, so the track is
        // stereo. With the setting off or a stereo stream nothing is built.
        int trackChannels = audioConfiguration.channelCount;
        if (VirtualSurround.appliesTo(virtualSurround, audioConfiguration.channelCount)) {
            surround = buildSurround(audioConfiguration.channelCount, sampleRate, samplesPerFrame);
            if (surround != null) {
                channelConfig = AudioFormat.CHANNEL_OUT_STEREO;
                trackChannels = 2;
            }
        }

        LimeLog.info("Audio channel config: "+String.format("0x%X", channelConfig));

        bytesPerFrame = trackChannels * samplesPerFrame * 2;

        // We're not supposed to request less than the minimum
        // buffer size for our buffer, but it appears that we can
        // do this on many devices and it lowers audio latency.
        // We'll try the small buffer size first and if it fails,
        // use the recommended larger buffer size.

        for (int i = 0; i < 4; i++) {
            boolean lowLatency;
            int bufferSize;

            // We will try:
            // 1) Small buffer, low latency mode
            // 2) Large buffer, low latency mode
            // 3) Small buffer, standard mode
            // 4) Large buffer, standard mode

            switch (i) {
                case 0:
                case 1:
                    lowLatency = true;
                    break;
                case 2:
                case 3:
                    lowLatency = false;
                    break;
                default:
                    // Unreachable
                    throw new IllegalStateException();
            }

            switch (i) {
                case 0:
                case 2:
                    bufferSize = bytesPerFrame * 2;
                    break;

                case 1:
                case 3:
                    // Try the larger buffer size
                    bufferSize = Math.max(AudioTrack.getMinBufferSize(sampleRate,
                            channelConfig,
                            AudioFormat.ENCODING_PCM_16BIT),
                            bytesPerFrame * 2);

                    // Round to next frame
                    bufferSize = (((bufferSize + (bytesPerFrame - 1)) / bytesPerFrame) * bytesPerFrame);
                    break;
                default:
                    // Unreachable
                    throw new IllegalStateException();
            }

            // Skip low latency options if hardware sample rate doesn't match the content
            if (AudioTrack.getNativeOutputSampleRate(AudioManager.STREAM_MUSIC) != sampleRate && lowLatency) {
                continue;
            }

            // Skip low latency options when using audio effects, since low latency mode
            // precludes the use of the audio effect pipeline (as of Android 13).
            if (enableAudioFx && lowLatency) {
                continue;
            }

            try {
                track = createAudioTrack(channelConfig, sampleRate, bufferSize, lowLatency);
                track.play();

                // Successfully created working AudioTrack. We're done here.
                LimeLog.info("Audio track configuration: "+bufferSize+" "+lowLatency);
                underruns = underrunCounter(track);
                break;
            } catch (Exception e) {
                // Try to release the AudioTrack if we got far enough
                e.printStackTrace();
                try {
                    if (track != null) {
                        track.release();
                        track = null;
                    }
                } catch (Exception ignored) {}
            }
        }

        if (track == null) {
            // Couldn't create any audio track for playback
            if (surround != null) {
                surround.release();
                surround = null;
            }
            return -2;
        }

        return 0;
    }

    // Null before Android 7, where a track has no count to read
    private static UnderrunCounter underrunCounter(final AudioTrack track) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.N) {
            return null;
        }
        return new UnderrunCounter(new UnderrunCounter.Track() {
            @Override
            public int underrunCount() {
                return Build.VERSION.SDK_INT >= Build.VERSION_CODES.N
                        ? track.getUnderrunCount() : -1;
            }
        });
    }

    // The thread this is called on is the connection's own, started with no
    // priority asked for, and the virtual surround's convolution runs on it
    // as well, so it could sit behind the frame loop and the depth threads.
    // Raised to the audio priority on the first block, said once in the log.
    private static void raiseThreadPriority() {
        int tid = Process.myTid();
        int was = Process.getThreadPriority(tid);
        try {
            Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);
        } catch (IllegalArgumentException | SecurityException e) {
            LimeLog.warning("Audio thread priority left at "+was+": "+e);
            return;
        }
        LimeLog.info("Audio thread priority: "+was+" raised to "+Process.getThreadPriority(tid)
                +" (tid "+tid+")");
    }

    /** The track's underruns as last read, once a second; -1 where it cannot say. Any thread. */
    public int getUnderrunCount() {
        UnderrunCounter counter = underruns;
        return counter != null ? counter.count() : -1;
    }

    @Override
    public void playDecodedAudio(short[] audioData) {
        if (!priorityRaised) {
            priorityRaised = true;
            raiseThreadPriority();
        }

        // Only queue up to 40 ms of pending audio data in addition to what AudioTrack is buffering for us.
        if (MoonBridge.getPendingAudioDuration() < 40) {
            // This will block until the write is completed. That can cause a backlog
            // of pending audio data, so we do the above check to be able to bound
            // latency at 40 ms in that situation.
            if (surround != null) {
                short[] stereo = surround.render(audioData);
                track.write(stereo, 0, stereo.length);
            }
            else {
                track.write(audioData, 0, audioData.length);
            }
        }
        else {
            LimeLog.info("Too much pending audio data: " + MoonBridge.getPendingAudioDuration() +" ms");
        }

        UnderrunCounter counter = underruns;
        if (counter != null) {
            counter.poll(SystemClock.uptimeMillis());
        }
    }

    @Override
    public void start() {
        if (enableAudioFx) {
            // Open an audio effect control session to allow equalizers to apply audio effects
            Intent i = new Intent(AudioEffect.ACTION_OPEN_AUDIO_EFFECT_CONTROL_SESSION);
            i.putExtra(AudioEffect.EXTRA_AUDIO_SESSION, track.getAudioSessionId());
            i.putExtra(AudioEffect.EXTRA_PACKAGE_NAME, context.getPackageName());
            i.putExtra(AudioEffect.EXTRA_CONTENT_TYPE, AudioEffect.CONTENT_TYPE_GAME);
            context.sendBroadcast(i);
        }
    }

    @Override
    public void stop() {
        if (enableAudioFx) {
            // Close our audio effect control session when we're stopping
            Intent i = new Intent(AudioEffect.ACTION_CLOSE_AUDIO_EFFECT_CONTROL_SESSION);
            i.putExtra(AudioEffect.EXTRA_AUDIO_SESSION, track.getAudioSessionId());
            i.putExtra(AudioEffect.EXTRA_PACKAGE_NAME, context.getPackageName());
            context.sendBroadcast(i);
        }
    }

    @Override
    public void cleanup() {
        UnderrunCounter counter = underruns;
        if (counter != null) {
            FileLog.event("audio: " + counter.readNow() + " underruns over the stream");
        }

        // Immediately drop all pending data
        track.pause();
        track.flush();

        track.release();

        if (surround != null) {
            surround.release();
            surround = null;
        }
    }
}

package com.limelight.binding.video;

import android.content.Context;
import android.content.res.AssetFileDescriptor;

import com.limelight.LimeLog;

import org.tensorflow.lite.Interpreter;
import org.tensorflow.lite.gpu.CompatibilityList;
import org.tensorflow.lite.gpu.GpuDelegate;
import org.tensorflow.lite.gpu.GpuDelegateFactory;

import java.io.FileInputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;

/**
 * MiDaS v2.1 small at 256x256 on LiteRT. Our own fp16 conversion of the
 * MIT licensed ONNX export, see tools/convert_midas.py. The ImageNet
 * normalization is inside the graph, so the input is plain RGB in 0..1.
 *
 * Must be created and used on the thread holding the GL context, since the
 * GPU delegate binds to it.
 */
public class MidasDepthSource implements DepthSource {

    private static final String MODEL_ASSET = "midas_v21_small_256_fp16.tflite";

    private static final int WARMUP_RUNS = 3;
    private static final int BENCHMARK_RUNS = 10;
    // CPU is slow enough that a long reference run is a waste of startup time
    private static final int CPU_BENCHMARK_RUNS = 3;

    // Times a CPU interpreter alongside the GPU one at startup. The point is
    // to prove the delegate is really running on the GPU rather than having
    // silently fallen back, which a bare timing number cannot show. Costs
    // about a second at stream start.
    private static final boolean BENCHMARK_CPU = true;

    private Interpreter interpreter;
    private GpuDelegate gpuDelegate;
    private ByteBuffer input;
    private ByteBuffer output;
    private boolean gpuAccelerated;
    private long lastInferenceNs;

    @Override
    public boolean initialize(Context context, ByteBuffer inputBuffer, ByteBuffer outputBuffer) {
        input = inputBuffer.order(ByteOrder.nativeOrder());
        output = outputBuffer.order(ByteOrder.nativeOrder());

        MappedByteBuffer model;
        try {
            model = loadModel(context);
        } catch (IOException e) {
            LimeLog.severe("Depth model asset unreadable: "+e.getMessage());
            return false;
        }

        Interpreter.Options options = new Interpreter.Options();

        // The compatibility list is a shipped allowlist of known device and
        // driver strings, not a capability check, and headsets are not on it.
        // Log what it thinks but ignore it: the only real test is creating
        // the delegate and seeing whether the model loads.
        CompatibilityList compatibility = new CompatibilityList();
        LimeLog.info("Depth model: GPU allowlist says "
                +compatibility.isDelegateSupportedOnThisDevice()+", trying the delegate anyway");

        try {
            GpuDelegateFactory.Options gpuOptions = new GpuDelegateFactory.Options();
            // fp16 math, which is what the model already carries
            gpuOptions.setPrecisionLossAllowed(true);
            gpuOptions.setInferencePreference(
                    GpuDelegateFactory.Options.INFERENCE_PREFERENCE_SUSTAINED_SPEED);
            gpuDelegate = new GpuDelegate(gpuOptions);
            options.addDelegate(gpuDelegate);
            gpuAccelerated = true;
        } catch (Exception e) {
            LimeLog.warning("GPU delegate creation failed, using CPU: "+e.getMessage());
        }
        if (!gpuAccelerated) {
            options.setNumThreads(2);
        }

        try {
            interpreter = new Interpreter(model, options);
        } catch (Exception e) {
            LimeLog.warning("Depth model failed to load with the GPU delegate: "+e.getMessage());
            releaseDelegate();
            gpuAccelerated = false;
            try {
                Interpreter.Options cpuOptions = new Interpreter.Options();
                cpuOptions.setNumThreads(2);
                interpreter = new Interpreter(model, cpuOptions);
            } catch (Exception e2) {
                LimeLog.severe("Depth model failed to load: "+e2.getMessage());
                return false;
            }
        }

        int[] inputShape = interpreter.getInputTensor(0).shape();
        int[] outputShape = interpreter.getOutputTensor(0).shape();
        LimeLog.info("Depth model loaded, running on "+(gpuAccelerated ? "GPU" : "CPU")
                +", input "+shapeToString(inputShape)+" output "+shapeToString(outputShape));

        if (inputShape.length != 4 || inputShape[1] != DEPTH_SIZE || inputShape[2] != DEPTH_SIZE
                || inputShape[3] != 3) {
            LimeLog.severe("Unexpected depth model input shape");
            release();
            return false;
        }

        for (int i = 0; i < WARMUP_RUNS; i++) {
            if (!estimate()) {
                release();
                return false;
            }
        }
        LimeLog.info("Depth model warmup done, "+(gpuAccelerated ? "GPU" : "CPU")+" inference "
                +String.format("%.1f", benchmark(interpreter, BENCHMARK_RUNS))
                +" ms avg over "+BENCHMARK_RUNS+" runs");

        if (gpuAccelerated && BENCHMARK_CPU) {
            benchmarkCpuForComparison(model);
        }
        return true;
    }

    private void benchmarkCpuForComparison(MappedByteBuffer model) {
        Interpreter cpu = null;
        try {
            Interpreter.Options options = new Interpreter.Options();
            options.setNumThreads(2);
            cpu = new Interpreter(model, options);
            for (int i = 0; i < WARMUP_RUNS; i++) {
                input.rewind();
                output.rewind();
                cpu.run(input, output);
            }
            LimeLog.info("Depth model CPU reference: "
                    +String.format("%.1f", benchmark(cpu, CPU_BENCHMARK_RUNS))
                    +" ms avg over "+CPU_BENCHMARK_RUNS+" runs");
        } catch (Exception e) {
            LimeLog.warning("CPU reference benchmark failed: "+e.getMessage());
        } finally {
            if (cpu != null) {
                cpu.close();
            }
        }
    }

    private float benchmark(Interpreter target, int runs) {
        long total = 0;
        for (int i = 0; i < runs; i++) {
            long start = System.nanoTime();
            input.rewind();
            output.rewind();
            target.run(input, output);
            total += System.nanoTime() - start;
        }
        return total / (float)runs / 1000000.0f;
    }

    private static String shapeToString(int[] shape) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < shape.length; i++) {
            sb.append(i == 0 ? "" : "x").append(shape[i]);
        }
        return sb.toString();
    }

    private MappedByteBuffer loadModel(Context context) throws IOException {
        AssetFileDescriptor fd = context.getAssets().openFd(MODEL_ASSET);
        try {
            FileInputStream stream = new FileInputStream(fd.getFileDescriptor());
            try {
                return stream.getChannel().map(FileChannel.MapMode.READ_ONLY,
                        fd.getStartOffset(), fd.getDeclaredLength());
            } finally {
                stream.close();
            }
        } finally {
            fd.close();
        }
    }

    @Override
    public boolean estimate() {
        if (interpreter == null) {
            return false;
        }
        long start = System.nanoTime();
        try {
            input.rewind();
            output.rewind();
            interpreter.run(input, output);
        } catch (Exception e) {
            LimeLog.severe("Depth inference failed: "+e.getMessage());
            return false;
        }
        lastInferenceNs = System.nanoTime() - start;
        return true;
    }

    @Override
    public float getLastInferenceMs() {
        return lastInferenceNs / 1000000.0f;
    }

    @Override
    public boolean isGpuAccelerated() {
        return gpuAccelerated;
    }

    private void releaseDelegate() {
        if (gpuDelegate != null) {
            gpuDelegate.close();
            gpuDelegate = null;
        }
    }

    @Override
    public void release() {
        if (interpreter != null) {
            interpreter.close();
            interpreter = null;
        }
        releaseDelegate();
    }
}

package com.limelight.binding.video;

import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.os.SystemClock;

import com.limelight.LimeLog;
import com.limelight.preferences.PreferenceConfiguration;

import org.tensorflow.lite.Interpreter;
import org.tensorflow.lite.gpu.CompatibilityList;
import org.tensorflow.lite.gpu.GpuDelegate;
import org.tensorflow.lite.gpu.GpuDelegateFactory;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;
import java.util.ArrayList;
import java.util.List;

/**
 * A relative depth model on LiteRT: ZipDepth, or MiDaS v2.1 small on an XR2
 * Gen 2 headset. Which asset, at which size and on which runtime depends on
 * the headset, see Spec and Route. All the assets are our own conversions
 * (tools/convert_zipdepth.py, tools/quantize_depth.py, tools/convert_midas.py).
 * Both models normalize their input inside the graph, so the input is plain
 * RGB in 0..1, and both give relative inverse depth, larger is nearer.
 *
 * Must be created and used on the thread holding the GL context, since the
 * GPU delegate binds to it.
 */
public class MidasDepthSource implements DepthSource {

    /** How one headset generation runs a model. */
    public static final class Route {
        public final String asset;
        // The size the asset was exported at, which the session's map takes
        public final DepthSize size;
        public final boolean gpu;
        // What a CPU interpreter gets on this route
        public final int threads;

        Route(String asset, DepthSize size, boolean gpu, int threads) {
            this.asset = asset;
            this.size = size;
            this.gpu = gpu;
            this.threads = threads;
        }

        /** The asset and the runtime, as the log words them. */
        public String label() {
            return asset+" on "+(gpu ? "gpu" : "cpu ("+threads+" threads)");
        }
    }

    /**
     * One model: the list_vr_depth_source value that picks it and the route
     * each headset generation takes. The two generations do not run the same
     * export, so the size belongs to the route.
     */
    public static final class Spec {
        public final String key;
        public final String name;
        private final Route gen2Route;
        private final Route gen1Route;

        Spec(String key, String name, Route gen2Route, Route gen1Route) {
            this.key = key;
            this.name = name;
            this.gen2Route = gen2Route;
            this.gen1Route = gen1Route;
        }

        public Route route(boolean gen1) {
            return gen1 ? gen1Route : gen2Route;
        }
    }

    // What a CPU route runs on, the big cores these headsets have
    private static final int CPU_THREADS = 4;
    // What a delegate that will not load falls back to
    private static final int FALLBACK_CPU_THREADS = 2;

    // 16:9, so a 16:9 frame is not squashed on the way in, with the pixels of
    // the 384 square ZipDepth is trained and published at
    public static final Spec ZIPDEPTH = new Spec(PreferenceConfiguration.VR_DEPTH_SOURCE_ZIPDEPTH,
            "ZipDepth",
            new Route("zipdepth_512x288_fp16.tflite", new DepthSize(512, 288), true, CPU_THREADS),
            // A Gen 1 GPU will not take this graph, so those headsets run an
            // int8 dynamic range copy on the CPU instead
            new Route("zipdepth_256_int8dr.tflite", DepthSize.square(256), false, CPU_THREADS));

    // The same on both generations, though a Gen 1 headset is never offered it
    public static final Spec MIDAS = new Spec(PreferenceConfiguration.VR_DEPTH_SOURCE_MIDAS,
            "MiDaS",
            new Route("midas_v21_small_256_fp16.tflite", DepthSize.square(256), true, CPU_THREADS),
            new Route("midas_v21_small_256_fp16.tflite", DepthSize.square(256), true, CPU_THREADS));

    // Where the GPU delegate keeps its compiled kernels, under the app's
    // cache. Compiling them takes seconds at the first stream start; read back
    // from here it is a fraction of that, and a cache the system clears only
    // costs the compile once more.
    private static final String KERNEL_CACHE_DIR = "depth-kernels";

    private static final int WARMUP_RUNS = 3;
    private static final int BENCHMARK_RUNS = 10;
    // CPU is slow enough that a long reference run is a waste of startup time
    private static final int CPU_BENCHMARK_RUNS = 3;

    // Times a CPU interpreter alongside the GPU one at startup. The point is
    // to prove the delegate is really running on the GPU rather than having
    // silently fallen back, which a bare timing number cannot show. Costs
    // about a second at stream start, so it stays off now that the comparison
    // has been made (183 to 265 ms CPU against 13.5 ms GPU).
    private static final boolean BENCHMARK_CPU = false;

    private final Route route;
    // The interpreter reads the model for as long as it lives
    private MappedByteBuffer model;
    private Interpreter interpreter;
    private GpuDelegate gpuDelegate;
    private ByteBuffer input;
    private ByteBuffer output;
    private boolean gpuAccelerated;
    private boolean kernelCache;
    private int cpuThreads;
    private long loadMs;
    private long lastInferenceNs;

    public MidasDepthSource(Route route) {
        this.route = route;
    }

    /** The model a list_vr_depth_source value names, ZipDepth for anything but MiDaS. */
    public static Spec specFor(String value) {
        return MIDAS.key.equals(value) ? MIDAS : ZIPDEPTH;
    }

    /** The models a headset offers, in the order the settings list them. */
    public static List<Spec> offeredSpecs(boolean gen1) {
        List<Spec> offered = new ArrayList<>();
        for (Spec spec : new Spec[] { ZIPDEPTH, MIDAS }) {
            if (PreferenceConfiguration.isDepthSourceOffered(spec.key, gen1)) {
                offered.add(spec);
            }
        }
        return offered;
    }

    /**
     * The size a session's depth map runs at: the input size of the export
     * this headset loads for that value. The renderer allocates its staging
     * at this before the depth thread starts.
     */
    public static DepthSize sessionSize(String value, boolean gen1) {
        return specFor(value).route(gen1).size;
    }

    @Override
    public boolean initialize(Context context, ByteBuffer inputBuffer, ByteBuffer outputBuffer) {
        long start = SystemClock.elapsedRealtime();
        input = inputBuffer.order(ByteOrder.nativeOrder());
        output = outputBuffer.order(ByteOrder.nativeOrder());
        if (!stagingFits(input.capacity(), output.capacity(), route.size)) {
            LimeLog.severe("Depth staging is "+input.capacity()+" and "+output.capacity()
                    +" bytes, not a "+route.size+" map's");
            return false;
        }

        try {
            model = loadModel(context, route.asset);
        } catch (IOException e) {
            LimeLog.severe("Depth model asset unreadable: "+route.asset+": "+e.getMessage());
            return false;
        }
        if (!buildInterpreter(context)) {
            return false;
        }
        loadMs = SystemClock.elapsedRealtime() - start;

        int[] inputShape = interpreter.getInputTensor(0).shape();
        int[] outputShape = interpreter.getOutputTensor(0).shape();
        LimeLog.info("Depth model loaded in "+loadMs+" ms, "+route.asset+" on "+runtimeLabel()
                +", kernel cache "+(kernelCache ? "on" : "off")+", input "
                +shapeToString(inputShape)+" output "+shapeToString(outputShape));

        if (!inputFits(inputShape, route.size) || !outputFits(outputShape, route.size)) {
            LimeLog.severe("Depth model shape mismatch: input "+shapeToString(inputShape)
                    +" and output "+shapeToString(outputShape)+" for a "+route.size+" map");
            release();
            return false;
        }

        for (int i = 0; i < WARMUP_RUNS; i++) {
            if (!estimate()) {
                release();
                return false;
            }
        }
        LimeLog.info("Depth model warmup done, "+runtimeLabel()+" inference "
                +String.format("%.1f", benchmark(interpreter, BENCHMARK_RUNS))
                +" ms avg over "+BENCHMARK_RUNS+" runs");

        if (gpuAccelerated && BENCHMARK_CPU) {
            benchmarkCpuForComparison();
        }
        return true;
    }

    /**
     * Builds the interpreter on the route's runtime, dropping to the CPU if
     * the GPU delegate will not take the model.
     */
    private boolean buildInterpreter(Context context) {
        Interpreter.Options options = new Interpreter.Options();
        if (route.gpu) {
            useGpu(context, options);
        }
        else {
            useCpu(options, route.threads);
        }

        try {
            interpreter = new Interpreter(model, options);
        } catch (Exception e) {
            LimeLog.warning("Depth model failed to load on "+runtimeLabel()+": "+e.getMessage());
            releaseDelegate();
            try {
                Interpreter.Options cpuOptions = new Interpreter.Options();
                useCpu(cpuOptions, FALLBACK_CPU_THREADS);
                interpreter = new Interpreter(model, cpuOptions);
            } catch (Exception e2) {
                LimeLog.severe("Depth model failed to load: "+e2.getMessage());
                return false;
            }
        }
        return true;
    }

    private void useGpu(Context context, Interpreter.Options options) {
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
            String cacheDir = kernelCacheDir(context);
            if (cacheDir != null) {
                gpuOptions.setSerializationParams(cacheDir,
                        kernelToken(route.asset, model.capacity()));
                kernelCache = true;
            }
            gpuDelegate = new GpuDelegate(gpuOptions);
            options.addDelegate(gpuDelegate);
            gpuAccelerated = true;
        } catch (Exception e) {
            LimeLog.warning("GPU delegate creation failed, using CPU: "+e.getMessage());
            useCpu(options, FALLBACK_CPU_THREADS);
        }
    }

    private void useCpu(Interpreter.Options options, int threads) {
        gpuAccelerated = false;
        kernelCache = false;
        cpuThreads = threads;
        options.setNumThreads(threads);
        // Already the default for a float model, asked for so it stays that
        // way. XNNPACK is also what has kernels for int8 dynamic range weights.
        options.setUseXNNPACK(true);
    }

    /**
     * What the delegate checks its kernel cache against: the asset's name and
     * its size in bytes, so a changed asset never reads back the kernels of
     * the one before it.
     */
    static String kernelToken(String asset, int bytes) {
        return asset.substring(asset.lastIndexOf('/') + 1)+"-"+bytes;
    }

    // Null if the folder cannot be made, which only costs the compile again
    private static String kernelCacheDir(Context context) {
        File dir = new File(context.getCacheDir(), KERNEL_CACHE_DIR);
        if (!dir.isDirectory() && !dir.mkdirs()) {
            LimeLog.warning("No depth kernel cache at "+dir);
            return null;
        }
        return dir.getAbsolutePath();
    }

    /** Whether a model input is one RGB image of this size, rows then columns. */
    static boolean inputFits(int[] shape, DepthSize size) {
        return shape != null && shape.length == 4 && shape[0] == 1 && shape[1] == size.height
                && shape[2] == size.width && shape[3] == 3;
    }

    /**
     * Whether a model output is one value per pixel of this size, rows then
     * columns. Axes of 1 are ignored, so 1xHxW, 1xHxWx1 and 1x1xHxW all fit;
     * each lays the values out the same way.
     */
    static boolean outputFits(int[] shape, DepthSize size) {
        if (shape == null) {
            return false;
        }
        int[] kept = new int[2];
        int count = 0;
        for (int dim : shape) {
            if (dim == 1) {
                continue;
            }
            if (count == 2) {
                return false;
            }
            kept[count++] = dim;
        }
        return count == 2 && kept[0] == size.height && kept[1] == size.width;
    }

    /** Whether the staging the renderer handed over holds a map of this size. */
    static boolean stagingFits(int inputBytes, int outputBytes, DepthSize size) {
        return inputBytes == size.pixels() * 3 * 4 && outputBytes == size.pixels() * 4;
    }

    private void benchmarkCpuForComparison() {
        Interpreter cpu = null;
        try {
            Interpreter.Options options = new Interpreter.Options();
            options.setNumThreads(FALLBACK_CPU_THREADS);
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

    static String shapeToString(int[] shape) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < shape.length; i++) {
            sb.append(i == 0 ? "" : "x").append(shape[i]);
        }
        return sb.toString();
    }

    private static MappedByteBuffer loadModel(Context context, String asset) throws IOException {
        AssetFileDescriptor fd = context.getAssets().openFd(asset);
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

    /** gpu, or cpu with its thread count, whichever the model really runs on. */
    public String runtimeLabel() {
        return gpuAccelerated ? "gpu" : "cpu ("+cpuThreads+" threads)";
    }

    /** How long the model took to map and build, kernel compile or cache read included. */
    public long getLoadMs() {
        return loadMs;
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

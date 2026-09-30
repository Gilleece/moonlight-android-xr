package com.limelight.binding.video;

import org.junit.Test;

import java.util.Arrays;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;

/** The two depth models and the route each headset generation takes. */
public class DepthModelSpecTest {

    @Test
    public void eachValuePicksItsModel() {
        assertSame(MidasDepthSource.ZIPDEPTH, MidasDepthSource.specFor("zipdepth"));
        assertSame(MidasDepthSource.MIDAS, MidasDepthSource.specFor("model"));
    }

    // The test patterns run at the default model's size
    @Test
    public void anythingElseIsZipDepth() {
        assertSame(MidasDepthSource.ZIPDEPTH, MidasDepthSource.specFor("off"));
        assertSame(MidasDepthSource.ZIPDEPTH, MidasDepthSource.specFor("blob"));
        assertSame(MidasDepthSource.ZIPDEPTH, MidasDepthSource.specFor(""));
        assertSame(MidasDepthSource.ZIPDEPTH, MidasDepthSource.specFor(null));
    }

    @Test
    public void zipDepthIs512x288OnTheGen2Gpu() {
        MidasDepthSource.Route route = MidasDepthSource.ZIPDEPTH.route(false);
        assertEquals("zipdepth_512x288_fp16.tflite", route.asset);
        assertEquals(new DepthSize(512, 288), route.size);
        assertTrue(route.gpu);
        assertEquals("zipdepth_512x288_fp16.tflite on gpu", route.label());
    }

    @Test
    public void zipDepthIs256Int8OnTheGen1Cpu() {
        MidasDepthSource.Route route = MidasDepthSource.ZIPDEPTH.route(true);
        assertEquals("zipdepth_256_int8dr.tflite", route.asset);
        assertEquals(DepthSize.square(256), route.size);
        assertFalse(route.gpu);
        assertEquals(1, route.threads);
        assertEquals("zipdepth_256_int8dr.tflite on cpu (1 thread)", route.label());
    }

    @Test
    public void theThreadCountReadsRight() {
        assertEquals("1 thread", MidasDepthSource.threadCount(1));
        assertEquals("2 threads", MidasDepthSource.threadCount(2));
    }

    @Test
    public void midasIs256OnTheGpu() {
        MidasDepthSource.Route route = MidasDepthSource.MIDAS.route(false);
        assertEquals("midas_v21_small_256_fp16.tflite", route.asset);
        assertEquals(DepthSize.square(256), route.size);
        assertTrue(route.gpu);
    }

    // 16:9 with both sides multiples of 32, at the pixels of a 384 square
    @Test
    public void theGen2MapIs16by9() {
        DepthSize size = MidasDepthSource.ZIPDEPTH.route(false).size;
        assertEquals(0, size.width % 32);
        assertEquals(0, size.height % 32);
        assertEquals(16.0f / 9.0f, size.width / (float)size.height, 1e-6f);
        assertEquals(384 * 384, size.pixels());
    }

    @Test
    public void theSessionSizeFollowsModelAndHeadset() {
        assertEquals(new DepthSize(512, 288), MidasDepthSource.sessionSize("zipdepth", false));
        assertEquals(DepthSize.square(256), MidasDepthSource.sessionSize("zipdepth", true));
        assertEquals(DepthSize.square(256), MidasDepthSource.sessionSize("model", false));
        assertEquals(new DepthSize(512, 288), MidasDepthSource.sessionSize("flat", false));
    }

    @Test
    public void aGen1HeadsetOnlyOffersZipDepth() {
        assertEquals(Arrays.asList(MidasDepthSource.ZIPDEPTH),
                MidasDepthSource.offeredSpecs(true));
        assertEquals(Arrays.asList(MidasDepthSource.ZIPDEPTH, MidasDepthSource.MIDAS),
                MidasDepthSource.offeredSpecs(false));
    }

    // A changed asset never reads back the kernels compiled for the one before
    @Test
    public void theKernelTokenIsTheNameAndTheSize() {
        assertEquals("zipdepth_512x288_fp16.tflite-4096",
                MidasDepthSource.kernelToken("zipdepth_512x288_fp16.tflite", 4096));
        assertEquals("model.tflite-10", MidasDepthSource.kernelToken("/some/dir/model.tflite", 10));
    }

    @Test
    public void theInputHasToBeOneRgbImageAtTheMapSize() {
        DepthSize size = new DepthSize(512, 288);
        assertTrue(MidasDepthSource.inputFits(new int[] { 1, 288, 512, 3 }, size));
        // Transposed, or channels first, or square
        assertFalse(MidasDepthSource.inputFits(new int[] { 1, 512, 288, 3 }, size));
        assertFalse(MidasDepthSource.inputFits(new int[] { 1, 3, 288, 512 }, size));
        assertFalse(MidasDepthSource.inputFits(new int[] { 1, 256, 256, 3 }, size));
        assertFalse(MidasDepthSource.inputFits(new int[] { 288, 512, 3 }, size));
        assertFalse(MidasDepthSource.inputFits(null, size));
    }

    @Test
    public void theOutputHasToBeOneValuePerPixel() {
        DepthSize size = new DepthSize(512, 288);
        assertTrue(MidasDepthSource.outputFits(new int[] { 1, 288, 512 }, size));
        assertTrue(MidasDepthSource.outputFits(new int[] { 1, 288, 512, 1 }, size));
        assertTrue(MidasDepthSource.outputFits(new int[] { 1, 1, 288, 512 }, size));
        assertFalse(MidasDepthSource.outputFits(new int[] { 1, 512, 288 }, size));
        assertFalse(MidasDepthSource.outputFits(new int[] { 1, 144, 256 }, size));
        assertFalse(MidasDepthSource.outputFits(new int[] { 1, 288, 512, 2 }, size));
        assertFalse(MidasDepthSource.outputFits(new int[] { 147456 }, size));
        assertFalse(MidasDepthSource.outputFits(null, size));
    }

    @Test
    public void theStagingHasToBeTheMapSize() {
        DepthSize size = new DepthSize(512, 288);
        assertTrue(MidasDepthSource.stagingFits(512 * 288 * 12, 512 * 288 * 4, size));
        assertFalse(MidasDepthSource.stagingFits(256 * 256 * 12, 256 * 256 * 4, size));
        assertFalse(MidasDepthSource.stagingFits(512 * 288 * 12, 256 * 256 * 4, size));
    }
}

package com.limelight.binding.video;

import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertTrue;

public class DepthSizeTest {

    @Test
    public void readsAsWidthByHeight() {
        assertEquals("512x288", new DepthSize(512, 288).toString());
        assertEquals("256x256", DepthSize.square(256).toString());
    }

    @Test
    public void equalByWidthAndHeight() {
        assertEquals(new DepthSize(512, 288), new DepthSize(512, 288));
        assertEquals(new DepthSize(512, 288).hashCode(), new DepthSize(512, 288).hashCode());
        assertNotEquals(new DepthSize(512, 288), new DepthSize(288, 512));
        assertNotEquals(new DepthSize(512, 288), null);
        assertNotEquals(new DepthSize(512, 288), "512x288");
    }

    @Test
    public void pixelsAndSquareness() {
        assertEquals(147456, new DepthSize(512, 288).pixels());
        assertTrue(DepthSize.square(256).isSquare());
        assertFalse(new DepthSize(512, 288).isSquare());
    }
}

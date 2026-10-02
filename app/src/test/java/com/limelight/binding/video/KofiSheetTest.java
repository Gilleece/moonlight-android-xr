package com.limelight.binding.video;

import org.junit.Test;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.Inflater;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

/**
 * The Ko-fi sheet in the session: the QR code it shows is the one the script
 * writes for the page the sheet names, laid out where the sheet expects it,
 * and its words are there in every language.
 */
public class KofiSheetTest {

    private static final String[] LANGUAGES =
            { "values", "values-fr", "values-zh-rCN", "values-zh-rTW" };
    // What tools/make_qr.py asks for: version 3, 8 px a module, a four
    // module quiet zone
    private static final int MODULES = 29;
    private static final int MODULE_PX = 8;
    private static final int QUIET = 4;

    private static File file(String path) {
        File f = new File(path);
        return f.exists() ? f : new File("app/" + path);
    }

    private static String string(String dir, String name) throws Exception {
        String strings = new String(Files.readAllBytes(
                file("src/main/res/" + dir + "/strings.xml").toPath()), StandardCharsets.UTF_8);
        Matcher m = Pattern.compile("name=\"" + name + "\">([^<]+)<").matcher(strings);
        return m.find() ? m.group(1) : null;
    }

    /**
     * The grey levels of a greyscale PNG, rows of 0 to 255, for the one or
     * eight bit, non interlaced kind the script writes. The tests run without
     * Android's decoder or a desktop one, so this is the format by hand.
     */
    private static int[][] readGreyPng(File f) throws Exception {
        ByteBuffer in = ByteBuffer.wrap(Files.readAllBytes(f.toPath()));
        byte[] magic = new byte[8];
        in.get(magic);
        assertEquals((byte) 0x89, magic[0]);
        assertEquals('P', magic[1]);
        int width = 0, height = 0, depth = 0;
        ByteArrayOutputStream data = new ByteArrayOutputStream();
        while (in.remaining() >= 12) {
            int length = in.getInt();
            byte[] type = new byte[4];
            in.get(type);
            byte[] body = new byte[length];
            in.get(body);
            in.getInt();
            String name = new String(type, StandardCharsets.US_ASCII);
            if (name.equals("IHDR")) {
                ByteBuffer h = ByteBuffer.wrap(body);
                width = h.getInt();
                height = h.getInt();
                depth = h.get();
                assertEquals("greyscale", 0, h.get());
                h.get();
                h.get();
                assertEquals("not interlaced", 0, h.get());
                assertTrue(depth == 1 || depth == 8);
            }
            else if (name.equals("IDAT")) {
                data.write(body);
            }
        }
        Inflater inflater = new Inflater();
        inflater.setInput(data.toByteArray());
        int stride = (width * depth + 7) / 8;
        byte[] raw = new byte[(stride + 1) * height];
        int got = 0;
        while (got < raw.length && !inflater.finished()) {
            got += inflater.inflate(raw, got, raw.length - got);
        }
        assertEquals(raw.length, got);
        int[][] grey = new int[height][width];
        int[] prev = new int[stride];
        int[] line = new int[stride];
        for (int y = 0; y < height; y++) {
            int filter = raw[y * (stride + 1)];
            for (int i = 0; i < stride; i++) {
                int x = raw[y * (stride + 1) + 1 + i] & 0xFF;
                int a = i > 0 ? line[i - 1] : 0;
                int b = prev[i];
                int c = i > 0 ? prev[i - 1] : 0;
                if (filter == 1) {
                    x += a;
                }
                else if (filter == 2) {
                    x += b;
                }
                else if (filter == 3) {
                    x += (a + b) / 2;
                }
                else if (filter == 4) {
                    int p = a + b - c;
                    int pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
                    x += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                }
                line[i] = x & 0xFF;
            }
            for (int x = 0; x < width; x++) {
                grey[y][x] = depth == 8 ? line[x]
                        : ((line[x / 8] >> (7 - x % 8)) & 1) * 255;
            }
            int[] swap = prev;
            prev = line;
            line = swap;
        }
        return grey;
    }

    private static boolean dark(int[][] image, int moduleX, int moduleY) {
        int px = (QUIET + moduleX) * MODULE_PX;
        int py = (QUIET + moduleY) * MODULE_PX;
        boolean first = image[py][px] == 0;
        // Every pixel of a module the same, so it reads as one block
        for (int y = 0; y < MODULE_PX; y++) {
            for (int x = 0; x < MODULE_PX; x++) {
                assertEquals(first, image[py + y][px + x] == 0);
            }
        }
        return first;
    }

    // A finder pattern: a dark ring seven modules across, a light ring inside
    // it, a dark three by three in the middle
    private static void checkFinder(int[][] image, int ox, int oy) {
        for (int y = 0; y < 7; y++) {
            for (int x = 0; x < 7; x++) {
                int ring = Math.max(Math.abs(x - 3), Math.abs(y - 3));
                assertEquals(ring != 2, dark(image, ox + x, oy + y));
            }
        }
    }

    @Test
    public void theCodeIsTheScriptsForThePageTheSheetNames() throws Exception {
        String script = new String(Files.readAllBytes(file("../tools/make_qr.py").toPath()),
                StandardCharsets.UTF_8);
        Matcher url = Pattern.compile("(?m)^URL = \"([^\"]+)\"").matcher(script);
        assertTrue(url.find());
        assertEquals(XrRenderer.SUPPORT_URL, url.group(1));
        assertEquals("https://ko-fi.com/moonlightxr", XrRenderer.SUPPORT_URL);
    }

    @Test
    public void theCodeIsSharpBlackOnWhite() throws Exception {
        int[][] image = readGreyPng(file("src/main/assets/images/kofi_qr.png"));
        assertEquals(XrShared.KOFI_QR_PX, image.length);
        assertEquals(XrShared.KOFI_QR_PX, image[0].length);
        assertEquals((MODULES + 2 * QUIET) * MODULE_PX, XrShared.KOFI_QR_PX);
        // Black and white and nothing between, the quiet zone all white
        int end = (QUIET + MODULES) * MODULE_PX;
        for (int y = 0; y < image.length; y++) {
            for (int x = 0; x < image[y].length; x++) {
                int grey = image[y][x];
                assertTrue(grey == 0 || grey == 255);
                boolean quiet = x < QUIET * MODULE_PX || y < QUIET * MODULE_PX
                        || x >= end || y >= end;
                if (quiet) {
                    assertEquals(255, grey);
                }
            }
        }
        // The three finder patterns where a reader looks for them, and the
        // timing row between two of them alternating
        checkFinder(image, 0, 0);
        checkFinder(image, MODULES - 7, 0);
        checkFinder(image, 0, MODULES - 7);
        for (int x = 8; x < MODULES - 8; x++) {
            assertEquals(x % 2 == 0, dark(image, x, 6));
        }
    }

    @Test
    public void theSheetHasRoomForTheCodeAndTheButton() {
        float qrLeft = Math.round(XrShared.KOFI_QR_L * XrShared.KOFI_TEX_W);
        float qrTop = Math.round(XrShared.KOFI_QR_T * XrShared.KOFI_TEX_H);
        // Inside the sheet, under the title, clear of its rounded corners
        assertTrue(qrLeft >= 32.0f && qrTop >= 0.18f * XrShared.KOFI_TEX_H);
        assertTrue(qrTop + XrShared.KOFI_QR_PX <= XrShared.KOFI_TEX_H - 32.0f);
        // The words beside it have most of the rest of the width
        float column = qrLeft + XrShared.KOFI_QR_PX + 48.0f;
        assertTrue(XrShared.KOFI_CLOSE_R * XrShared.KOFI_TEX_W - column > 500.0f);
        // The button bottom right, clear of the code, inside the sheet
        assertTrue(XrShared.KOFI_CLOSE_L * XrShared.KOFI_TEX_W > qrLeft + XrShared.KOFI_QR_PX);
        assertTrue(XrShared.KOFI_CLOSE_L < XrShared.KOFI_CLOSE_R && XrShared.KOFI_CLOSE_R < 1.0f);
        assertTrue(XrShared.KOFI_BTN_T < XrShared.KOFI_BTN_B && XrShared.KOFI_BTN_B < 1.0f);
        assertTrue(XrShared.KOFI_BTN_T > 0.5f);
    }

    @Test
    public void itIsWordedInEveryLanguage() throws Exception {
        for (String dir : LANGUAGES) {
            for (String name : new String[] { "vr_kofi_title", "vr_kofi_scan", "vr_kofi_free",
                    "vr_kofi_close" }) {
                String said = string(dir, name);
                assertNotNull(dir + " " + name, said);
                assertFalse(dir + " " + name, said.trim().isEmpty());
            }
            assertTrue(dir, string(dir, "vr_kofi_title").contains("Ko-fi"));
            assertTrue(dir, string(dir, "vr_kofi_free").contains("Moonlight XR"));
        }
        assertEquals("Support Moonlight XR on Ko-fi", string("values", "vr_kofi_title"));
        assertEquals("Scan it with your phone.", string("values", "vr_kofi_scan"));
        assertEquals("Optional: Moonlight XR is free and stays free.",
                string("values", "vr_kofi_free"));
        assertEquals("Close", string("values", "vr_kofi_close"));
    }
}

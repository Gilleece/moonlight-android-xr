package com.limelight.binding.video;

import org.junit.BeforeClass;
import org.junit.Test;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.zip.Inflater;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

/**
 * The splash draws the logo from numbers rather than from the logo file, so
 * these hold the numbers to the file: the wedges cover what is white in it
 * and nothing else, the letters' boxes are where its letters are, and the
 * wedges open clockwise from the one beside the letters.
 */
public class SplashLogoTest {

    private static int width;
    private static int height;
    // 0 to 1 from the disc's slate to the white of the wedges and letters,
    // 0 wherever the file is clear
    private static float[][] light;
    private static double cx;
    private static double cy;
    private static double radius;

    private static File logo() {
        File f = new File("../moonlight-xr-logo-transparent.png");
        return f.exists() ? f : new File("moonlight-xr-logo-transparent.png");
    }

    /**
     * The file's pixels, for the eight bit RGBA, non interlaced kind it is.
     * The tests run without Android's decoder or a desktop one, so this is
     * the format by hand.
     */
    @BeforeClass
    public static void readLogo() throws Exception {
        ByteBuffer in = ByteBuffer.wrap(Files.readAllBytes(logo().toPath()));
        in.position(8);
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
                assertEquals("eight bit", 8, h.get());
                assertEquals("RGBA", 6, h.get());
                h.get();
                h.get();
                assertEquals("not interlaced", 0, h.get());
            }
            else if (name.equals("IDAT")) {
                data.write(body);
            }
        }
        int stride = width * 4;
        byte[] raw = new byte[height * (stride + 1)];
        Inflater inflater = new Inflater();
        inflater.setInput(data.toByteArray());
        int got = 0;
        while (got < raw.length && !inflater.finished()) {
            got += inflater.inflate(raw, got, raw.length - got);
        }
        inflater.end();
        assertEquals(raw.length, got);

        int[] prev = new int[stride];
        int[] line = new int[stride];
        light = new float[height][width];
        double area = 0.0, sx = 0.0, sy = 0.0;
        for (int y = 0; y < height; y++) {
            int filter = raw[y * (stride + 1)];
            for (int i = 0; i < stride; i++) {
                int v = raw[y * (stride + 1) + 1 + i] & 255;
                int a = i >= 4 ? line[i - 4] : 0;
                int b = prev[i];
                int c = i >= 4 ? prev[i - 4] : 0;
                int pred;
                switch (filter) {
                    case 1: pred = a; break;
                    case 2: pred = b; break;
                    case 3: pred = (a + b) >> 1; break;
                    case 4: {
                        int p = a + b - c;
                        int pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
                        pred = pa <= pb && pa <= pc ? a : (pb <= pc ? b : c);
                        break;
                    }
                    default: pred = 0;
                }
                line[i] = (v + pred) & 255;
            }
            for (int x = 0; x < width; x++) {
                double alpha = line[x * 4 + 3] / 255.0;
                area += alpha;
                sx += alpha * (x + 0.5);
                sy += alpha * (y + 0.5);
                double lum = (line[x * 4] + line[x * 4 + 1] + line[x * 4 + 2]) / 765.0;
                light[y][x] = alpha < 0.5 ? 0.0f
                        : (float) Math.max(0.0, Math.min(1.0, (lum - 0.36) / 0.64));
            }
            int[] t = prev;
            prev = line;
            line = t;
        }
        cx = sx / area;
        cy = sy / area;
        radius = Math.sqrt(area / Math.PI);
    }

    private static boolean inAnyWedge(double x, double y) {
        for (int k = 0; k < XrPanels.Logo.WEDGES; k++) {
            if (XrPanels.Logo.inWedge(k, x, y)) {
                return true;
            }
        }
        return false;
    }

    @Test
    public void theLogoIsTheShapeTheSplashExpects() {
        assertEquals(960, width);
        assertEquals(1024, height);
        // A disc 820 px across, which the measured fractions are of
        assertEquals(409.5, radius, 1.0);
        assertEquals(XrShared.SPLASH_WEDGES, XrPanels.Logo.WEDGES);
    }

    @Test
    public void theWedgesCoverWhatIsWhiteOutsideTheLetters() {
        // Every pixel inside the disc's edge but outside the letters' quarter,
        // where only the wedges are white. They can only disagree along an
        // edge, by the antialiasing: 0.39 percent as measured, where a wedge
        // 1 percent too long reads 1.3 and a spoke a pixel too wide 1.2.
        int counted = 0, wrong = 0;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                double dx = (x + 0.5 - cx) / radius, dy = (y + 0.5 - cy) / radius;
                if (Math.hypot(dx, dy) > 0.95 || (dx > 0.0 && dy > 0.0)) {
                    continue;
                }
                counted++;
                if (inAnyWedge(dx, dy) != light[y][x] >= 0.5f) {
                    wrong++;
                }
            }
        }
        double share = wrong / (double) counted;
        assertTrue("wedges disagree with the logo on " + wrong + " of " + counted + " px",
                share < 0.005);
    }

    @Test
    public void eachWedgeIsWhiteWellInsideAndSlateJustOutside() {
        for (int k = 0; k < XrPanels.Logo.WEDGES; k++) {
            double mid = Math.toRadians(XrPanels.Logo.FIRST_DEG
                    + XrPanels.Logo.WEDGE_DEG * (k + 0.5));
            for (double r = 0.2; r <= 0.7; r += 0.05) {
                assertTrue("wedge " + k + " at " + r,
                        sample(r * Math.cos(mid), r * Math.sin(mid)) > 0.9f);
            }
            // Just past its reach, and on the spoke along its first edge
            double past = XrPanels.Logo.WEDGE_R + 0.01;
            assertTrue(sample(past * Math.cos(mid), past * Math.sin(mid)) < 0.1f);
            double edge = Math.toRadians(XrPanels.Logo.FIRST_DEG + XrPanels.Logo.WEDGE_DEG * k);
            for (double r = 0.15; r <= 0.7; r += 0.05) {
                assertTrue("spoke " + k + " at " + r,
                        sample(r * Math.cos(edge), r * Math.sin(edge)) < 0.1f);
            }
        }
    }

    @Test
    public void theLettersFillTheirBoxes() {
        for (float[] box : new float[][] { XrPanels.Logo.X_BOX, XrPanels.Logo.R_BOX }) {
            int x0 = (int) Math.floor(cx + box[0] * radius) - 2;
            int x1 = (int) Math.ceil(cx + box[2] * radius) + 2;
            int y0 = (int) Math.floor(cy + box[1] * radius) - 2;
            int y1 = (int) Math.ceil(cy + box[3] * radius) + 2;
            int left = Integer.MAX_VALUE, right = -1, top = Integer.MAX_VALUE, bottom = -1;
            for (int y = y0; y <= y1; y++) {
                for (int x = x0; x <= x1; x++) {
                    if (light[y][x] >= 0.5f) {
                        left = Math.min(left, x);
                        right = Math.max(right, x + 1);
                        top = Math.min(top, y);
                        bottom = Math.max(bottom, y + 1);
                    }
                }
            }
            assertEquals(cx + box[0] * radius, left, 1.5);
            assertEquals(cx + box[2] * radius, right, 1.5);
            assertEquals(cy + box[1] * radius, top, 1.5);
            assertEquals(cy + box[3] * radius, bottom, 1.5);
        }
    }

    @Test
    public void theWedgesOpenClockwiseFromBesideTheLetters() {
        double last = -1.0;
        for (int k = 0; k < XrPanels.Logo.WEDGES; k++) {
            float[] w = XrPanels.Logo.wedge(k);
            // The arc stays inside its 45 degrees, and each starts further
            // round clockwise than the one before
            assertTrue(w[2] > XrPanels.Logo.FIRST_DEG + XrPanels.Logo.WEDGE_DEG * k);
            assertTrue(w[2] + w[3] < XrPanels.Logo.FIRST_DEG + XrPanels.Logo.WEDGE_DEG * (k + 1));
            assertTrue(w[2] > last);
            last = w[2];
            // The tip is where the two spokes' edges meet, inside the wedge
            double tipAngle = Math.toDegrees(Math.atan2(w[1], w[0]));
            if (tipAngle < 0.0) {
                tipAngle += 360.0;
            }
            assertEquals(XrPanels.Logo.FIRST_DEG + XrPanels.Logo.WEDGE_DEG * (k + 0.5),
                    tipAngle, 1e-3);
            assertTrue(XrPanels.Logo.inWedge(k, w[0] * 1.5, w[1] * 1.5));
            assertTrue(!XrPanels.Logo.inWedge(k, w[0] * 0.9, w[1] * 0.9));
        }
        // The first sits just clockwise of straight down, beside the letters
        float[] first = XrPanels.Logo.wedge(0);
        assertTrue(first[0] < 0.0f && first[1] > 0.0f);
    }

    // Bilinear lightness at a point given as fractions of the radius
    private static float sample(double dx, double dy) {
        double x = cx + dx * radius - 0.5, y = cy + dy * radius - 0.5;
        int x0 = (int) Math.floor(x), y0 = (int) Math.floor(y);
        double fx = x - x0, fy = y - y0;
        return (float) (light[y0][x0] * (1 - fx) * (1 - fy) + light[y0][x0 + 1] * fx * (1 - fy)
                + light[y0 + 1][x0] * (1 - fx) * fy + light[y0 + 1][x0 + 1] * fx * fy);
    }
}

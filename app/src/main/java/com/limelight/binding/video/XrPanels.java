package com.limelight.binding.video;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.RectF;
import android.graphics.Shader;

import com.limelight.LimeLog;

import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;

/**
 * The flat panels reachable from inside the session: the environment picker,
 * the settings sheets, the keyboard and the exit prompt, and the buttons that
 * open them. Java is the only place Android will lay out text, so their art
 * is drawn to bitmaps here and handed back as pixels for the frame loop to
 * upload, since that thread owns the GL context. Nothing in here touches the
 * session, so it can run on whichever thread has the time.
 */
final class XrPanels {

    // Environment picker, a grid of thumbnails reachable from inside the
    // session. One band per category: a header strip carrying its name, then a
    // row of cells under it. The rooms are the first band, the photos from the
    // assets folder in name order are the second. A cell is a place in the
    // grid and nothing more, what gets saved is the stable id it maps to. Must
    // match the PICKER_ constants in xr_renderer.h.
    static final String ENVIRONMENT_DIR = "environments";
    private static final String IMAGE_DIR = "images";
    private static final int PICKER_COLS = 4;
    private static final int PICKER_ROWS = 2;
    private static final int PICKER_CELLS = PICKER_COLS * PICKER_ROWS;
    private static final int PICKER_TEX_W = 1024;
    private static final int PICKER_HEADER_PX = 40;
    private static final int PICKER_CELL_PX = 256;
    private static final int PICKER_BAND_PX = PICKER_HEADER_PX + PICKER_CELL_PX;
    private static final int PICKER_TEX_H = PICKER_BAND_PX * PICKER_ROWS;
    private static final int PICKER_CELL_W = PICKER_TEX_W / PICKER_COLS;
    // One per band, drawn in the strip above its cells
    private static final String[] PICKER_HEADERS = { "Rooms", "360 Images" };
    private static final int ENV_BUTTON_TEX = 128;
    // The padlock that locks the hands out. Must match LOCK_TEX in xr_renderer.h.
    private static final int LOCK_TEX = 384;
    static final int CELL_PASSTHROUGH = 0;
    static final int CELL_VOID = 1;
    // Shared with the native side, which needs them to pick a room style. Must
    // match ENV_CELL_MINIMAL_ROOM and ENV_CELL_PSX_CINEMA in xr_renderer.h.
    static final int CELL_MINIMAL_ROOM = 2;
    static final int CELL_PSX_CINEMA = 3;
    static final int CELL_FIRST_PHOTO = 4;
    // The photos take whatever the rooms leave, so how many fit is a question
    // for the layout rather than a count kept here
    static final int MAX_PHOTOS = PICKER_CELLS - CELL_FIRST_PHOTO;

    // The settings panel behind the cog button. Drawn here, placed and dragged
    // natively, so the layout has to be agreed between the two: these must
    // match the COG_ constants in xr_renderer.h.
    private static final int COG_TEX_W = 768;
    private static final int COG_TEX_H = 640;
    private static final float COG_TRACK_L = 0.42f;
    private static final float COG_TRACK_R = 0.93f;
    private static final float COG_TAB_BAR_B = 0.16f;
    // Six rows on the screen tab, so they start a little higher and sit closer
    // together than they did at five
    private static final float COG_ROW_V0 = 0.25f;
    private static final float COG_ROW_STEP = 0.11f;
    private static final float COG_CELL_HALF = 0.045f;
    private static final float COG_RESET_L = 0.35f;
    private static final float COG_RESET_R = 0.65f;
    // Clear of the last row, which reaches 0.80 plus the half band
    private static final float COG_RESET_T = 0.87f;
    private static final float COG_RESET_B = 0.97f;
    // Three tabs, a texture each, all uploaded once so switching is free, and
    // a fourth sheet handed over after them: the screen tab as it reads while
    // a 3d room hangs the picture, which the native side picks for itself.
    private static final int COG_TAB_SCREEN = 0;
    private static final int COG_TAB_DISPLAY = 1;
    private static final int COG_TAB_3D = 2;
    private static final String[] COG_TABS = { "Screen", "Display", "3D" };
    private static final String[] COG_SLIDER_ROWS =
            { "Distance", "Height", "Tilt", "Rotate", "Curve", "Size" };
    private static final int COG_ROW_TILT = 2;
    private static final int COG_ROW_ROTATE = 3;
    private static final int COG_ROW_CURVE = 4;
    // What stands in for those rows in a room, where the wall decides both the
    // placement and the size
    private static final String COG_ROOM_NOTICE =
            "Screen size cannot be changed in a 3D environment. "
                    + "Please choose a different environment to customise the screen size.";
    // Display tab: a label and a row of cells, one of which is in force, and
    // the glow level track under them. Head locked sits with the picture rows
    // so the two light rows and the level track they belong with stay together
    // at the bottom. Screen light is the wash the picture throws over a 3d
    // room, which only shows in one, and head lock is ignored in one, but both
    // stay live here like the rest: the picker can put a room up at any moment.
    private static final String[] COG_OPTION_ROWS =
            { "Sharpen", "Stats", "Head locked", "Glow", "Screen light" };
    private static final String[][] COG_OPTION_CELLS = {
            { "Off", "Normal", "Quality" },
            { "Off", "On" },
            { "Off", "On" },
            { "Off", "On" },
            { "Off", "On" }
    };
    // Must match COG_DISPLAY_SLIDER_ROW in xr_renderer.h
    private static final int COG_DISPLAY_SLIDER_ROW = 5;
    // 3D tab: two sliders, drawn the same way the screen tab's are. Only
    // values that take effect the moment they move belong on the panel, which
    // is why the depth source itself stays in the 2d settings.
    private static final String[] COG_SLIDER3D_ROWS = { "Depth", "Convergence" };
    // Where the measured comfort cap, which is also the shipped default, falls
    // along the separation track. Must match COG_SEP_MAX and COG_SEP_STEPS in
    // xr_renderer.h.
    private static final float COG_SEP_CAP_T = 5.0f / 15.0f;

    // The in world keyboard. Three sheets of the same layout, one per state,
    // handed over in state order, along with the geometry that goes with them:
    // the native side is given key rectangles and codes and knows nothing else
    // about it. KB_TEX_W, KB_TEX_H and the code values must match the KB_
    // constants in xr_renderer.h.
    private static final int KB_TEX_W = 1120;
    private static final int KB_TEX_H = 448;
    private static final int KB_CODE_SHIFT = -2;
    private static final int KB_CODE_SYMBOLS = -3;
    private static final int KB_CODE_HIDE = -4;
    // Key widths per row, in units where a plain key is 1, and where each row
    // starts. One table for all three states, so every state has to lay its
    // keys out the same way.
    private static final float[][] KB_ROW_WIDTHS = {
            { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
            { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
            { 1, 1, 1, 1, 1, 1, 1, 1, 1 },
            { 1.5f, 1, 1, 1, 1, 1, 1, 1, 1.5f },
            { 1.5f, 1, 4, 1, 1.5f, 1 }
    };
    // Only the home row is inset, the way it is on a real keyboard
    private static final float[] KB_ROW_INDENT = { 0.0f, 0.0f, 0.5f, 0.0f, 0.0f };
    private static final float KB_ROW_UNITS = 10.0f;
    // Margins and the gap between two keys, all as fractions of the panel
    private static final float KB_PAD_U = 0.012f;
    private static final float KB_PAD_V = 0.030f;
    private static final float KB_GAP_U = 0.005f;
    private static final float KB_GAP_V = 0.014f;

    private static final String[][] KB_LABELS_LOWER = {
            { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
            { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p" },
            { "a", "s", "d", "f", "g", "h", "j", "k", "l" },
            { "Shift", "z", "x", "c", "v", "b", "n", "m", "Del" },
            { "?123", ",", "space", ".", "Enter", "Hide" }
    };
    private static final int[][] KB_CODES_LOWER = {
            { '1', '2', '3', '4', '5', '6', '7', '8', '9', '0' },
            { 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p' },
            { 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l' },
            { KB_CODE_SHIFT, 'z', 'x', 'c', 'v', 'b', 'n', 'm', 8 },
            { KB_CODE_SYMBOLS, ',', 32, '.', 13, KB_CODE_HIDE }
    };
    private static final String[][] KB_LABELS_UPPER = {
            { "!", "@", "#", "$", "%", "^", "&", "*", "(", ")" },
            { "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P" },
            { "A", "S", "D", "F", "G", "H", "J", "K", "L" },
            { "Shift", "Z", "X", "C", "V", "B", "N", "M", "Del" },
            { "?123", ",", "space", ".", "Enter", "Hide" }
    };
    private static final int[][] KB_CODES_UPPER = {
            { '!', '@', '#', '$', '%', '^', '&', '*', '(', ')' },
            { 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P' },
            { 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L' },
            { KB_CODE_SHIFT, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', 8 },
            { KB_CODE_SYMBOLS, ',', 32, '.', 13, KB_CODE_HIDE }
    };
    // The brackets row has one slot fewer than a letter row, since the
    // geometry is shared, so tab takes the place shift had
    private static final String[][] KB_LABELS_SYMBOLS = {
            { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
            { "@", "#", "$", "%", "&", "*", "-", "+", "(", ")" },
            { "!", "\"", "'", ":", ";", "/", "?", "_", "=" },
            { "Tab", "<", ">", "[", "]", "{", "}", "\\", "Del" },
            { "ABC", ",", "space", ".", "Enter", "Hide" }
    };
    private static final int[][] KB_CODES_SYMBOLS = {
            { '1', '2', '3', '4', '5', '6', '7', '8', '9', '0' },
            { '@', '#', '$', '%', '&', '*', '-', '+', '(', ')' },
            { '!', '"', '\'', ':', ';', '/', '?', '_', '=' },
            { 9, '<', '>', '[', ']', '{', '}', '\\', 8 },
            { KB_CODE_SYMBOLS, ',', 32, '.', 13, KB_CODE_HIDE }
    };

    // The button that ends the stream and the prompt it opens. One sheet per
    // lit button, in zone order, so which one shows is a swapchain handle on
    // the native side rather than an upload. These must match the EXIT_
    // constants in xr_renderer.h.
    private static final int EXIT_TEX_W = 512;
    private static final int EXIT_TEX_H = 256;
    private static final int EXIT_ZONE_NONE = 0;
    private static final int EXIT_ZONE_EXIT = 1;
    private static final int EXIT_ZONE_CANCEL = 2;
    private static final float EXIT_BTN_T = 0.56f;
    private static final float EXIT_BTN_B = 0.86f;
    private static final float EXIT_EXIT_L = 0.08f;
    private static final float EXIT_EXIT_R = 0.46f;
    private static final float EXIT_CANCEL_L = 0.54f;
    private static final float EXIT_CANCEL_R = 0.92f;
    private static final String EXIT_QUESTION = "Exit the stream?";

    private final Context context;
    // The photos in the assets folder, in the order the picker shows them
    private final String[] environmentFiles;

    XrPanels(Context context, String[] environmentFiles) {
        this.context = context;
        this.environmentFiles = environmentFiles;
    }

    // Everything the keyboard hands over: the three sheets in state order, the
    // button that opens it, and the geometry they were all drawn from
    static final class Keyboard {
        final ByteBuffer lower;
        final ByteBuffer upper;
        final ByteBuffer symbols;
        final ByteBuffer button;
        final float[] keyRects;
        final int[] codesLower;
        final int[] codesUpper;
        final int[] codesSymbols;

        Keyboard(ByteBuffer lower, ByteBuffer upper, ByteBuffer symbols, ByteBuffer button,
                 float[] keyRects, int[] codesLower, int[] codesUpper, int[] codesSymbols) {
            this.lower = lower;
            this.upper = upper;
            this.symbols = symbols;
            this.button = button;
            this.keyRects = keyRects;
            this.codesLower = codesLower;
            this.codesUpper = codesUpper;
            this.codesSymbols = codesSymbols;
        }
    }

    // Where a cell sits in the picker texture: along to its column, then down
    // past the headers of its own band and the ones above it. The native side
    // ends up at the same place from the PICKER_ constants.
    private static RectF pickerTile(int cell, float pad) {
        float left = (cell % PICKER_COLS) * PICKER_CELL_W;
        float top = (cell / PICKER_COLS) * PICKER_BAND_PX + PICKER_HEADER_PX;
        return new RectF(left + pad, top + pad,
                left + PICKER_CELL_W - pad, top + PICKER_CELL_PX - pad);
    }

    /**
     * Draws the grid. Java is the only place Android will lay out text, so the
     * labels have to be baked into the texture here rather than drawn in the
     * shader.
     */
    ByteBuffer buildPickerGrid() {
        final float pad = 7.0f;
        // Matches the radius of the hover ring drawn over it, which is a
        // fraction of the cell rather than a pixel count
        final float radius = PICKER_CELL_W * 0.125f;

        Bitmap grid = Bitmap.createBitmap(PICKER_TEX_W, PICKER_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(grid);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        paint.setColor(0xE0141416);
        canvas.drawRoundRect(new RectF(1.0f, 1.0f, PICKER_TEX_W - 1.0f, PICKER_TEX_H - 1.0f),
                radius * 0.6f, radius * 0.6f, paint);

        Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
        label.setColor(Color.WHITE);
        label.setTextSize(21.0f);
        label.setTextAlign(Paint.Align.CENTER);

        // The category names, quieter than the tile labels so they read as
        // headings rather than as another row of things to press
        Paint header = new Paint(Paint.ANTI_ALIAS_FLAG);
        header.setColor(0xB0FFFFFF);
        header.setTextSize(22.0f);
        header.setTextAlign(Paint.Align.CENTER);
        Paint.FontMetrics metrics = header.getFontMetrics();
        float baseline = (PICKER_HEADER_PX - (metrics.descent - metrics.ascent)) * 0.5f
                - metrics.ascent;
        for (int band = 0; band < PICKER_ROWS && band < PICKER_HEADERS.length; band++) {
            canvas.drawText(PICKER_HEADERS[band], PICKER_TEX_W * 0.5f,
                    band * PICKER_BAND_PX + baseline, header);
        }

        for (int cell = 0; cell < PICKER_CELLS; cell++) {
            RectF tile = pickerTile(cell, pad);

            String name;
            Bitmap thumb = null;
            if (cell == CELL_PASSTHROUGH) {
                name = "Passthrough";
                paint.setColor(0xFF2A3540);
            }
            else if (cell == CELL_VOID) {
                name = "Black void";
                paint.setColor(0xFF090909);
            }
            else if (cell == CELL_MINIMAL_ROOM) {
                // No photo to preview, so the room is sketched on the tile
                // below once the base colour is down
                name = "Minimal room";
                paint.setColor(0xFF0B0B0E);
            }
            else if (cell == CELL_PSX_CINEMA) {
                name = "PSX Cinema";
                paint.setColor(0xFF120A0C);
            }
            else if (cell - CELL_FIRST_PHOTO < environmentFiles.length) {
                name = labelFor(environmentFiles[cell - CELL_FIRST_PHOTO]);
                thumb = decodeThumb(environmentFiles[cell - CELL_FIRST_PHOTO], (int)tile.height());
                paint.setColor(0xFF1E1E20);
            }
            else {
                continue;
            }

            if (thumb != null) {
                // Scaled to cover and centred, so the middle of the panorama
                // becomes the preview rather than a squashed whole sphere
                BitmapShader shader = new BitmapShader(thumb, Shader.TileMode.CLAMP,
                                                       Shader.TileMode.CLAMP);
                float scale = Math.max(tile.width() / thumb.getWidth(),
                                       tile.height() / thumb.getHeight());
                Matrix m = new Matrix();
                m.setScale(scale, scale);
                m.postTranslate(tile.centerX() - thumb.getWidth() * scale * 0.5f,
                                tile.centerY() - thumb.getHeight() * scale * 0.5f);
                shader.setLocalMatrix(m);
                paint.setShader(shader);
            }
            paint.setStyle(Paint.Style.FILL);
            canvas.drawRoundRect(tile, radius, radius, paint);
            paint.setShader(null);
            if (thumb != null) {
                thumb.recycle();
            }
            if (cell == CELL_MINIMAL_ROOM) {
                drawRoomTile(canvas, paint, tile, radius);
            }
            else if (cell == CELL_PSX_CINEMA) {
                drawCinemaTile(canvas, paint, tile, radius);
            }

            // Dark band under the label, clipped to the bottom of the tile so
            // it keeps the rounded corners it sits in
            canvas.save();
            canvas.clipRect(tile.left, tile.bottom - 44.0f, tile.right, tile.bottom);
            paint.setColor(0xC0000000);
            canvas.drawRoundRect(tile, radius, radius, paint);
            canvas.restore();

            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(2.0f);
            paint.setColor(0x50FFFFFF);
            canvas.drawRoundRect(tile, radius, radius, paint);
            paint.setStyle(Paint.Style.FILL);

            canvas.drawText(name, tile.centerX(), tile.bottom - 15.0f, label);
        }

        ByteBuffer pixels = toBuffer(grid);
        grid.recycle();
        return pixels;
    }

    /**
     * The thumbnail for a room cell, drawn rather than photographed: a lit
     * screen on the wall of a bare dark room, with a faint line low down where
     * the floor meets it.
     */
    private void drawRoomTile(Canvas canvas, Paint paint, RectF tile, float radius) {
        canvas.save();
        // Clipped to the tile so nothing leaks past the rounded corners
        Path clip = new Path();
        clip.addRoundRect(tile, radius, radius, Path.Direction.CW);
        canvas.clipPath(clip);

        final float w = tile.width();
        final float h = tile.height();

        // A 16:9 screen sitting in the upper middle, with a wider soft rect
        // behind it standing in for the light it throws on the wall
        float screenW = w * 0.62f;
        float screenH = screenW * 9.0f / 16.0f;
        float screenTop = tile.top + h * 0.24f;
        RectF screen = new RectF(tile.centerX() - screenW * 0.5f, screenTop,
                tile.centerX() + screenW * 0.5f, screenTop + screenH);

        RectF halo = new RectF(screen);
        halo.inset(-w * 0.07f, -h * 0.07f);
        paint.setColor(0x38A6C4F0);
        canvas.drawRoundRect(halo, radius * 0.7f, radius * 0.7f, paint);
        paint.setColor(0xFFDCE6F4);
        canvas.drawRect(screen, paint);

        // Where the floor meets the wall, faint enough to read as a room
        // rather than as a line across the tile
        paint.setColor(0x28FFFFFF);
        float floorY = tile.top + h * 0.78f;
        canvas.drawRect(new RectF(tile.left, floorY, tile.right, floorY + 1.5f), paint);

        canvas.restore();
    }

    /**
     * The thumbnail for the cinema cell: a lit screen between the deep red side
     * curtains, which is about all of that room that reads at this size.
     */
    private void drawCinemaTile(Canvas canvas, Paint paint, RectF tile, float radius) {
        canvas.save();
        Path clip = new Path();
        clip.addRoundRect(tile, radius, radius, Path.Direction.CW);
        canvas.clipPath(clip);

        final float w = tile.width();
        final float h = tile.height();

        // The picture, narrower than the bare room's since the curtains take
        // the sides of the tile
        float screenW = w * 0.50f;
        float screenH = screenW * 9.0f / 16.0f;
        float screenTop = tile.top + h * 0.27f;
        RectF screen = new RectF(tile.centerX() - screenW * 0.5f, screenTop,
                tile.centerX() + screenW * 0.5f, screenTop + screenH);

        RectF halo = new RectF(screen);
        halo.inset(-w * 0.07f, -h * 0.07f);
        paint.setColor(0x34C4D6F0);
        canvas.drawRoundRect(halo, radius * 0.7f, radius * 0.7f, paint);
        paint.setColor(0xFFE2E9F6);
        canvas.drawRect(screen, paint);

        // Curtains over the ends of that halo, so the light reads as coming
        // from behind them
        final float curtainW = w * 0.21f;
        paint.setColor(0xFF7C1319);
        canvas.drawRect(new RectF(tile.left, tile.top, tile.left + curtainW, tile.bottom), paint);
        canvas.drawRect(new RectF(tile.right - curtainW, tile.top, tile.right, tile.bottom), paint);

        // Three pleats apiece, which is what says curtain rather than red panel
        final float pleatW = w * 0.013f;
        paint.setColor(0xFF4A0B10);
        for (int i = 1; i < 4; i++) {
            float along = curtainW * (i / 4.0f);
            float left = tile.left + along;
            canvas.drawRect(new RectF(left, tile.top, left + pleatW, tile.bottom), paint);
            float right = tile.right - curtainW + along;
            canvas.drawRect(new RectF(right, tile.top, right + pleatW, tile.bottom), paint);
        }

        // The front of the stage, faint enough to read as the dark of the room
        // rather than as a line across the tile
        paint.setColor(0x20FFFFFF);
        float stageY = tile.top + h * 0.76f;
        canvas.drawRect(new RectF(tile.left + curtainW, stageY,
                tile.right - curtainW, stageY + 1.5f), paint);

        canvas.restore();
    }

    // The padlocks and the cog ship as PNGs. Colour carries the state, so there
    // is nothing to tint or dim here, just a decode and a downscale to whatever
    // the swapchain it is headed for wants.
    private Bitmap loadIcon(String fileName, int size) {
        InputStream in = null;
        try {
            in = context.getAssets().open(IMAGE_DIR + "/" + fileName);
            Bitmap full = BitmapFactory.decodeStream(in);
            if (full == null) {
                LimeLog.warning("Icon " + fileName + " did not decode");
                return null;
            }
            if (full.getWidth() == size && full.getHeight() == size) {
                return full;
            }
            Bitmap scaled = Bitmap.createScaledBitmap(full, size, size, true);
            if (scaled != full) {
                full.recycle();
            }
            return scaled;
        } catch (IOException | OutOfMemoryError e) {
            LimeLog.warning("Icon " + fileName + " failed: " + e);
            return null;
        } finally {
            closeQuietly(in);
        }
    }

    // A framed landscape, which is about as much as reads at this size
    ByteBuffer buildEnvButton() {
        Bitmap button = Bitmap.createBitmap(ENV_BUTTON_TEX, ENV_BUTTON_TEX,
                                            Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(button);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);

        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(6.0f);
        canvas.drawRoundRect(new RectF(14.0f, 14.0f, 114.0f, 114.0f), 22.0f, 22.0f, paint);

        paint.setStyle(Paint.Style.FILL);
        canvas.drawCircle(46.0f, 46.0f, 9.0f, paint);

        Path hills = new Path();
        hills.moveTo(26.0f, 100.0f);
        hills.lineTo(54.0f, 58.0f);
        hills.lineTo(73.0f, 84.0f);
        hills.lineTo(84.0f, 70.0f);
        hills.lineTo(102.0f, 100.0f);
        hills.close();
        canvas.drawPath(hills, paint);

        return toBuffer(button);
    }

    // The padlock shut, then open, or nothing at all
    ByteBuffer[] buildLockIcons() {
        Bitmap shut = loadIcon("handtracking_locked.png", LOCK_TEX);
        Bitmap open = loadIcon("handtracking_unlocked.png", LOCK_TEX);
        // Both or neither, since one on its own would leave the button blank
        // in half its states
        ByteBuffer[] icons = null;
        if (shut != null && open != null) {
            icons = new ByteBuffer[] { toBuffer(shut), toBuffer(open) };
        }
        if (shut != null) {
            shut.recycle();
        }
        if (open != null) {
            open.recycle();
        }
        return icons;
    }

    /**
     * The settings panel. A texture per tab and one more for the screen tab in
     * a room, all drawn once here, so changing tab in the session picks
     * another swapchain rather than redrawing anything. Only the labels, tracks
     * and cells live in the texture: thumbs and selection rings are quads of
     * their own, so using the panel costs no upload.
     */
    ByteBuffer[] buildCogTabs(boolean curveOk, boolean stereoOk) {
        Bitmap screenTab = buildCogTab(COG_TAB_SCREEN, curveOk, stereoOk);
        ByteBuffer screen = toBuffer(screenTab);
        screenTab.recycle();

        Bitmap displayTab = buildCogTab(COG_TAB_DISPLAY, curveOk, stereoOk);
        ByteBuffer display = toBuffer(displayTab);
        displayTab.recycle();

        Bitmap tab3d = buildCogTab(COG_TAB_3D, curveOk, stereoOk);
        ByteBuffer sheet3d = toBuffer(tab3d);
        tab3d.recycle();

        Bitmap roomTab = buildCogRoomTab();
        ByteBuffer room = toBuffer(roomTab);
        roomTab.recycle();

        return new ByteBuffer[] { screen, display, sheet3d, room };
    }

    // The cog that opens the settings panel
    ByteBuffer buildCogButton() {
        // Never blank: the drawn gear stands in if the art does not decode
        Bitmap button = loadIcon("settings_icon.png", ENV_BUTTON_TEX);
        if (button == null) {
            button = buildCogFallback();
        }
        ByteBuffer pixels = toBuffer(button);
        button.recycle();
        return pixels;
    }

    // The screen tab as it reads inside a 3d room: the same chrome, and a note
    // where the rows would be, since the room hangs and sizes the picture
    // itself. The native side shows this sheet in place of the screen tab
    // while a room is on.
    private Bitmap buildCogRoomTab() {
        Bitmap bitmap = Bitmap.createBitmap(COG_TEX_W, COG_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        drawCogChrome(canvas, COG_TAB_SCREEN);
        drawCogRoomNotice(canvas);
        return bitmap;
    }

    private Bitmap buildCogTab(int tab, boolean curveOk, boolean stereoOk) {
        Bitmap bitmap = Bitmap.createBitmap(COG_TEX_W, COG_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        drawCogChrome(canvas, tab);
        if (tab == COG_TAB_SCREEN) {
            drawCogSliderRows(canvas, curveOk);
        }
        else if (tab == COG_TAB_3D) {
            drawCog3dRows(canvas, stereoOk);
        }
        else {
            drawCogOptionRows(canvas);
        }
        return bitmap;
    }

    // Background and tab bar, the part both tabs have in common. The tab this
    // texture belongs to is the one drawn as current.
    private void drawCogChrome(Canvas canvas, int tab) {
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        paint.setColor(0xF0141416);
        canvas.drawRoundRect(new RectF(1.0f, 1.0f, COG_TEX_W - 1.0f, COG_TEX_H - 1.0f),
                32.0f, 32.0f, paint);

        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextSize(25.0f);
        text.setTextAlign(Paint.Align.CENTER);

        final float barB = COG_TAB_BAR_B * COG_TEX_H;
        final float slotW = COG_TEX_W / (float)COG_TABS.length;
        for (int i = 0; i < COG_TABS.length; i++) {
            boolean current = i == tab;
            RectF slot = new RectF(i * slotW + 12.0f, 12.0f, (i + 1) * slotW - 12.0f, barB - 8.0f);

            if (current) {
                paint.setColor(0x28FFFFFF);
                canvas.drawRoundRect(slot, 14.0f, 14.0f, paint);
            }

            text.setColor(current ? Color.WHITE : 0x60FFFFFF);
            canvas.drawText(COG_TABS[i], slot.centerX(),
                    slot.centerY() - (text.ascent() + text.descent()) * 0.5f, text);

            if (current) {
                // The underline is what carries at a glance, the fill alone is
                // too subtle at this size
                paint.setColor(0xEEFFFFFF);
                canvas.drawRect(slot.left + 24.0f, slot.bottom - 4.0f,
                        slot.right - 24.0f, slot.bottom, paint);
            }
        }

        paint.setColor(0x30FFFFFF);
        canvas.drawRect(20.0f, barB, COG_TEX_W - 20.0f, barB + 2.0f, paint);
    }

    // Screen tab: a label and a track per row, and the reset button under them
    private void drawCogSliderRows(Canvas canvas, boolean curveOk) {
        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextSize(22.0f);
        text.setTextAlign(Paint.Align.LEFT);

        Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
        track.setStyle(Paint.Style.STROKE);
        track.setStrokeWidth(6.0f);
        track.setStrokeCap(Paint.Cap.ROUND);

        Paint tick = new Paint(Paint.ANTI_ALIAS_FLAG);
        tick.setColor(0xCCFFFFFF);

        for (int row = 0; row < COG_SLIDER_ROWS.length; row++) {
            boolean live = row != COG_ROW_CURVE || curveOk;
            float y = (COG_ROW_V0 + row * COG_ROW_STEP) * COG_TEX_H;

            text.setColor(live ? Color.WHITE : 0x30FFFFFF);
            // Centred on the row rather than sitting on it, so the label lines
            // up with the track beside it
            canvas.drawText(COG_SLIDER_ROWS[row], 0.06f * COG_TEX_W,
                    y - (text.ascent() + text.descent()) * 0.5f, text);

            track.setColor(live ? 0x66FFFFFF : 0x30FFFFFF);
            canvas.drawLine(COG_TRACK_L * COG_TEX_W, y, COG_TRACK_R * COG_TEX_W, y, track);

            if (row == COG_ROW_TILT || row == COG_ROW_ROTATE) {
                // Marks level, which is where the middle of these two tracks
                // snaps to. The rows that do not snap stay unmarked.
                float midX = (COG_TRACK_L + COG_TRACK_R) * 0.5f * COG_TEX_W;
                float tickHalf = COG_CELL_HALF * COG_TEX_H;
                canvas.drawRect(midX - 2.0f, y - tickHalf, midX + 2.0f, y + tickHalf, tick);
            }
        }

        // A way back for a screen dragged somewhere unrecoverable
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(4.0f);
        RectF reset = new RectF(COG_RESET_L * COG_TEX_W, COG_RESET_T * COG_TEX_H,
                COG_RESET_R * COG_TEX_W, COG_RESET_B * COG_TEX_H);
        canvas.drawRoundRect(reset, 14.0f, 14.0f, paint);

        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        canvas.drawText("Reset", reset.centerX(),
                reset.centerY() - (text.ascent() + text.descent()) * 0.5f, text);
    }

    // What the screen tab carries in a room instead of its rows: the reason
    // there are none, centred in the body under the tab bar
    private void drawCogRoomNotice(Canvas canvas) {
        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextSize(22.0f);
        text.setTextAlign(Paint.Align.CENTER);
        text.setColor(0xC0FFFFFF);

        String[] lines = wrapText(COG_ROOM_NOTICE, text, 0.80f * COG_TEX_W);
        float step = (text.descent() - text.ascent()) * 1.4f;
        float middle = (COG_TAB_BAR_B * COG_TEX_H + COG_TEX_H) * 0.5f;
        float y = middle - (lines.length - 1) * step * 0.5f
                - (text.ascent() + text.descent()) * 0.5f;
        for (String line : lines) {
            canvas.drawText(line, COG_TEX_W * 0.5f, y, text);
            y += step;
        }
    }

    // Greedy word wrap, which is all one fixed sentence on a fixed panel needs
    private static String[] wrapText(String message, Paint paint, float width) {
        ArrayList<String> lines = new ArrayList<>();
        StringBuilder line = new StringBuilder();
        for (String word : message.split(" ")) {
            if (line.length() > 0 && paint.measureText(line + " " + word) > width) {
                lines.add(line.toString());
                line.setLength(0);
            }
            if (line.length() > 0) {
                line.append(' ');
            }
            line.append(word);
        }
        if (line.length() > 0) {
            lines.add(line.toString());
        }
        return lines.toArray(new String[0]);
    }

    // 3D tab: the two values worth reaching mid stream. Depth runs past the
    // comfortable range on purpose, with the far end marked, since where that
    // range ends is a matter of eyes rather than of hardware.
    private void drawCog3dRows(Canvas canvas, boolean stereoOk) {
        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextSize(22.0f);
        text.setTextAlign(Paint.Align.LEFT);
        text.setColor(stereoOk ? Color.WHITE : 0x30FFFFFF);

        Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
        track.setStyle(Paint.Style.STROKE);
        track.setStrokeWidth(6.0f);
        track.setStrokeCap(Paint.Cap.ROUND);

        Paint tick = new Paint(Paint.ANTI_ALIAS_FLAG);
        tick.setColor(stereoOk ? 0xCCFFFFFF : 0x30FFFFFF);

        final float trackL = COG_TRACK_L * COG_TEX_W;
        final float trackR = COG_TRACK_R * COG_TEX_W;
        final float tickHalf = COG_CELL_HALF * COG_TEX_H;

        for (int row = 0; row < COG_SLIDER3D_ROWS.length; row++) {
            float y = (COG_ROW_V0 + row * COG_ROW_STEP) * COG_TEX_H;
            canvas.drawText(COG_SLIDER3D_ROWS[row], 0.06f * COG_TEX_W,
                    y - (text.ascent() + text.descent()) * 0.5f, text);

            // The default sits a third along the depth track and halfway along
            // convergence, and a tick says so on both
            float markT = row == 0 ? COG_SEP_CAP_T : 0.5f;
            float markX = trackL + markT * (trackR - trackL);

            if (row == 0) {
                // Measured on device: past 0.5 percent the depth stops growing
                // and only the strain does, so the rest of the track is drawn
                // as a place you can go rather than one you should
                track.setColor(stereoOk ? 0x66FFFFFF : 0x30FFFFFF);
                canvas.drawLine(trackL, y, markX, y, track);
                track.setColor(stereoOk ? 0x66FFB74D : 0x30FFB74D);
                canvas.drawLine(markX, y, trackR, y, track);

                Paint caption = new Paint(Paint.ANTI_ALIAS_FLAG);
                caption.setTextSize(15.0f);
                caption.setTextAlign(Paint.Align.CENTER);
                caption.setColor(stereoOk ? 0xA0FFB74D : 0x30FFB74D);
                // Just above the next row's hit band, which starts 0.055 down
                // now the rows sit closer together
                canvas.drawText("harder on the eyes", (markX + trackR) * 0.5f,
                        y + 0.04f * COG_TEX_H, caption);
            }
            else {
                track.setColor(stereoOk ? 0x66FFFFFF : 0x30FFFFFF);
                canvas.drawLine(trackL, y, trackR, y, track);
            }

            canvas.drawRect(markX - 2.0f, y - tickHalf, markX + 2.0f, y + tickHalf, tick);
        }

        // A way back from a pair of values that turned out to be unwatchable
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(4.0f);
        RectF reset = new RectF(COG_RESET_L * COG_TEX_W, COG_RESET_T * COG_TEX_H,
                COG_RESET_R * COG_TEX_W, COG_RESET_B * COG_TEX_H);
        canvas.drawRoundRect(reset, 14.0f, 14.0f, paint);

        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        canvas.drawText("Reset", reset.centerX(),
                reset.centerY() - (text.ascent() + text.descent()) * 0.5f, text);

        if (!stereoOk) {
            // Otherwise two dead sliders with no explanation
            Paint hint = new Paint(Paint.ANTI_ALIAS_FLAG);
            hint.setTextSize(17.0f);
            hint.setTextAlign(Paint.Align.CENTER);
            hint.setColor(0x50FFFFFF);
            canvas.drawText("3D is off in settings", COG_TEX_W * 0.5f,
                    0.62f * COG_TEX_H, hint);
        }
    }

    // Display tab: a label and a row of cells, one press wide each. Which cell
    // is in force and which is under the ray are rings the native side puts
    // over them, so nothing here has to be redrawn when one is chosen.
    private void drawCogOptionRows(Canvas canvas) {
        Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
        label.setTextSize(22.0f);
        label.setTextAlign(Paint.Align.LEFT);
        label.setColor(Color.WHITE);

        Paint cellText = new Paint(Paint.ANTI_ALIAS_FLAG);
        cellText.setTextSize(19.0f);
        cellText.setTextAlign(Paint.Align.CENTER);
        cellText.setColor(Color.WHITE);

        Paint cell = new Paint(Paint.ANTI_ALIAS_FLAG);

        final float trackL = COG_TRACK_L * COG_TEX_W;
        final float trackR = COG_TRACK_R * COG_TEX_W;
        final float cellHalf = COG_CELL_HALF * COG_TEX_H;

        for (int row = 0; row < COG_OPTION_ROWS.length; row++) {
            float y = (COG_ROW_V0 + row * COG_ROW_STEP) * COG_TEX_H;
            canvas.drawText(COG_OPTION_ROWS[row], 0.06f * COG_TEX_W,
                    y - (label.ascent() + label.descent()) * 0.5f, label);

            String[] names = COG_OPTION_CELLS[row];
            float span = (trackR - trackL) / names.length;
            for (int i = 0; i < names.length; i++) {
                // Inset so neighbours read as separate buttons rather than one
                // long strip
                RectF box = new RectF(trackL + i * span + 3.0f, y - cellHalf,
                        trackL + (i + 1) * span - 3.0f, y + cellHalf);

                cell.setStyle(Paint.Style.FILL);
                cell.setColor(0x28FFFFFF);
                canvas.drawRoundRect(box, 10.0f, 10.0f, cell);
                cell.setStyle(Paint.Style.STROKE);
                cell.setStrokeWidth(2.0f);
                cell.setColor(0x50FFFFFF);
                canvas.drawRoundRect(box, 10.0f, 10.0f, cell);

                canvas.drawText(names[i], box.centerX(),
                        box.centerY() - (cellText.ascent() + cellText.descent()) * 0.5f,
                        cellText);
            }
        }

        // How strong the glow is, a track under the cells and the only row on
        // this tab that is dragged rather than pressed
        float y = (COG_ROW_V0 + COG_DISPLAY_SLIDER_ROW * COG_ROW_STEP) * COG_TEX_H;
        canvas.drawText("Glow level", 0.06f * COG_TEX_W,
                y - (label.ascent() + label.descent()) * 0.5f, label);

        Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
        track.setStyle(Paint.Style.STROKE);
        track.setStrokeWidth(6.0f);
        track.setStrokeCap(Paint.Cap.ROUND);
        track.setColor(0x66FFFFFF);
        canvas.drawLine(trackL, y, trackR, y, track);

        // Marks the default, halfway, the same way the 3D tab marks its two
        Paint tick = new Paint(Paint.ANTI_ALIAS_FLAG);
        tick.setColor(0xCCFFFFFF);
        float midX = (trackL + trackR) * 0.5f;
        canvas.drawRect(midX - 2.0f, y - cellHalf, midX + 2.0f, y + cellHalf, tick);
    }

    // The fallback cog, drawn only when the icon asset is missing. About as
    // much of a gear as reads at this size.
    private Bitmap buildCogFallback() {
        Bitmap button = Bitmap.createBitmap(ENV_BUTTON_TEX, ENV_BUTTON_TEX,
                                            Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(button);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);

        final float mid = ENV_BUTTON_TEX * 0.5f;
        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(10.0f);
        canvas.drawCircle(mid, mid, 34.0f, paint);

        paint.setStrokeWidth(12.0f);
        paint.setStrokeCap(Paint.Cap.ROUND);
        for (int tooth = 0; tooth < 8; tooth++) {
            double angle = tooth * Math.PI / 4.0;
            float dx = (float)Math.cos(angle);
            float dy = (float)Math.sin(angle);
            canvas.drawLine(mid + dx * 34.0f, mid + dy * 34.0f,
                            mid + dx * 48.0f, mid + dy * 48.0f, paint);
        }

        // A ring rather than a filled dot, which reads as a hole through the
        // middle of the gear the way a real one does
        paint.setStrokeCap(Paint.Cap.BUTT);
        paint.setStrokeWidth(8.0f);
        canvas.drawCircle(mid, mid, 14.0f, paint);

        return button;
    }

    /**
     * The in world keyboard: one sheet of art per state, the button that opens
     * it, and the layout the native side hit tests against. All three sheets
     * share one set of key rectangles, so the art and the hit test are built
     * from the same numbers and cannot drift apart.
     */
    Keyboard buildKeyboard() {
        float[] keyRects = buildKeyRects();
        int[] codesLower = flatten(KB_CODES_LOWER);
        int[] codesUpper = flatten(KB_CODES_UPPER);
        int[] codesSymbols = flatten(KB_CODES_SYMBOLS);

        Bitmap lower = buildKeyboardSheet(KB_LABELS_LOWER, keyRects);
        ByteBuffer lowerPixels = toBuffer(lower);
        lower.recycle();

        Bitmap upper = buildKeyboardSheet(KB_LABELS_UPPER, keyRects);
        ByteBuffer upperPixels = toBuffer(upper);
        upper.recycle();

        Bitmap symbols = buildKeyboardSheet(KB_LABELS_SYMBOLS, keyRects);
        ByteBuffer symbolsPixels = toBuffer(symbols);
        symbols.recycle();

        Bitmap button = buildKeyboardButton();
        ByteBuffer buttonPixels = toBuffer(button);
        button.recycle();

        return new Keyboard(lowerPixels, upperPixels, symbolsPixels, buttonPixels,
                keyRects, codesLower, codesUpper, codesSymbols);
    }

    // Left, top, right and bottom of every key as fractions of the panel, rows
    // top down, keys left to right. The row widths are in key units, so this is
    // where they turn into a place on the texture.
    private static float[] buildKeyRects() {
        int keys = 0;
        for (float[] row : KB_ROW_WIDTHS) {
            keys += row.length;
        }

        float[] rects = new float[keys * 4];
        float unit = (1.0f - 2.0f * KB_PAD_U) / KB_ROW_UNITS;
        float rowHeight = (1.0f - 2.0f * KB_PAD_V) / KB_ROW_WIDTHS.length;
        int at = 0;
        for (int row = 0; row < KB_ROW_WIDTHS.length; row++) {
            float x = KB_ROW_INDENT[row];
            for (int key = 0; key < KB_ROW_WIDTHS[row].length; key++) {
                float w = KB_ROW_WIDTHS[row][key];
                rects[at++] = KB_PAD_U + x * unit + KB_GAP_U * 0.5f;
                rects[at++] = KB_PAD_V + row * rowHeight + KB_GAP_V * 0.5f;
                rects[at++] = KB_PAD_U + (x + w) * unit - KB_GAP_U * 0.5f;
                rects[at++] = KB_PAD_V + (row + 1) * rowHeight - KB_GAP_V * 0.5f;
                x += w;
            }
        }
        return rects;
    }

    private static int[] flatten(int[][] rows) {
        int keys = 0;
        for (int[] row : rows) {
            keys += row.length;
        }

        int[] flat = new int[keys];
        int at = 0;
        for (int[] row : rows) {
            for (int code : row) {
                flat[at++] = code;
            }
        }
        return flat;
    }

    // One state's worth of keys, drawn as caps on the same dark rounded panel
    // the settings use
    private Bitmap buildKeyboardSheet(String[][] labels, float[] rects) {
        Bitmap bitmap = Bitmap.createBitmap(KB_TEX_W, KB_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);

        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        paint.setColor(0xF0141416);
        canvas.drawRoundRect(new RectF(1.0f, 1.0f, KB_TEX_W - 1.0f, KB_TEX_H - 1.0f),
                32.0f, 32.0f, paint);

        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextAlign(Paint.Align.CENTER);
        text.setColor(Color.WHITE);

        int at = 0;
        for (String[] row : labels) {
            for (String label : row) {
                RectF box = new RectF(rects[at] * KB_TEX_W, rects[at + 1] * KB_TEX_H,
                        rects[at + 2] * KB_TEX_W, rects[at + 3] * KB_TEX_H);
                at += 4;

                paint.setStyle(Paint.Style.FILL);
                paint.setColor(0x28FFFFFF);
                canvas.drawRoundRect(box, 10.0f, 10.0f, paint);
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(2.0f);
                paint.setColor(0x50FFFFFF);
                canvas.drawRoundRect(box, 10.0f, 10.0f, paint);

                // A single character is what the key types, so it gets the
                // room. The named keys are wordier and have to fit.
                text.setTextSize(label.length() == 1 ? 34.0f : 22.0f);
                canvas.drawText(label, box.centerX(),
                        box.centerY() - (text.ascent() + text.descent()) * 0.5f, text);
            }
        }

        return bitmap;
    }

    // A keyboard outline with a few keys in it, which is about as much as reads
    // at this size
    private Bitmap buildKeyboardButton() {
        Bitmap button = Bitmap.createBitmap(ENV_BUTTON_TEX, ENV_BUTTON_TEX,
                                            Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(button);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);

        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(6.0f);
        canvas.drawRoundRect(new RectF(12.0f, 28.0f, 116.0f, 100.0f), 14.0f, 14.0f, paint);

        paint.setStyle(Paint.Style.FILL);
        for (int row = 0; row < 2; row++) {
            float y = 42.0f + row * 16.0f;
            for (int key = 0; key < 4; key++) {
                float x = 26.0f + key * 20.0f;
                canvas.drawRoundRect(new RectF(x, y, x + 14.0f, y + 12.0f), 3.0f, 3.0f, paint);
            }
        }
        canvas.drawRoundRect(new RectF(44.0f, 74.0f, 84.0f, 86.0f), 3.0f, 3.0f, paint);

        return button;
    }

    /**
     * The button that ends the stream and the prompt it opens. The prompt is
     * drawn three times, once plain and once with each of its buttons lit, so
     * hovering one in the session picks another sheet rather than costing an
     * upload.
     */
    ByteBuffer[] buildExitArt() {
        Bitmap button = buildExitButton();
        ByteBuffer buttonPixels = toBuffer(button);
        button.recycle();

        Bitmap plain = buildExitPrompt(EXIT_ZONE_NONE);
        ByteBuffer plainPixels = toBuffer(plain);
        plain.recycle();

        Bitmap exitHot = buildExitPrompt(EXIT_ZONE_EXIT);
        ByteBuffer exitHotPixels = toBuffer(exitHot);
        exitHot.recycle();

        Bitmap cancelHot = buildExitPrompt(EXIT_ZONE_CANCEL);
        ByteBuffer cancelHotPixels = toBuffer(cancelHot);
        cancelHot.recycle();

        return new ByteBuffer[] { buttonPixels, plainPixels, exitHotPixels, cancelHotPixels };
    }

    // A power symbol, in the same weight and colour as the buttons either side
    // of it: a ring open at the top with a bar standing in the gap
    private Bitmap buildExitButton() {
        Bitmap button = Bitmap.createBitmap(ENV_BUTTON_TEX, ENV_BUTTON_TEX,
                                            Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(button);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);

        final float mid = ENV_BUTTON_TEX * 0.5f;
        final float radius = 36.0f;
        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(9.0f);
        paint.setStrokeCap(Paint.Cap.ROUND);

        // Starts a little past the top on one side and comes back round to the
        // same place on the other, which leaves the gap centred
        RectF ring = new RectF(mid - radius, mid - radius, mid + radius, mid + radius);
        canvas.drawArc(ring, -60.0f, 300.0f, false, paint);

        canvas.drawLine(mid, mid - radius - 10.0f, mid, mid - 2.0f, paint);

        return button;
    }

    // The prompt sheet: the question, and the two buttons under it. The zone
    // passed in is the one drawn lit, or none of them.
    private Bitmap buildExitPrompt(int hot) {
        Bitmap bitmap = Bitmap.createBitmap(EXIT_TEX_W, EXIT_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);

        // The same dark sheet the settings panel and the keyboard sit on
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        paint.setColor(0xF0141416);
        canvas.drawRoundRect(new RectF(1.0f, 1.0f, EXIT_TEX_W - 1.0f, EXIT_TEX_H - 1.0f),
                32.0f, 32.0f, paint);

        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextAlign(Paint.Align.CENTER);
        text.setColor(Color.WHITE);
        text.setTextSize(34.0f);
        float questionY = EXIT_TEX_H * 0.30f;
        canvas.drawText(EXIT_QUESTION, EXIT_TEX_W * 0.5f,
                questionY - (text.ascent() + text.descent()) * 0.5f, text);

        // Leaving is the destructive half, so it is the one that reads red.
        // Both are the same shape, so neither is the easier target.
        drawExitChoice(canvas, paint, text, EXIT_EXIT_L, EXIT_EXIT_R, "Exit",
                0xFFE05A5A, hot == EXIT_ZONE_EXIT);
        drawExitChoice(canvas, paint, text, EXIT_CANCEL_L, EXIT_CANCEL_R, "Cancel",
                0xEEFFFFFF, hot == EXIT_ZONE_CANCEL);

        return bitmap;
    }

    // One of the prompt's buttons. Hovering fills it, which is what says which
    // of the two a press would land on.
    private void drawExitChoice(Canvas canvas, Paint paint, Paint text, float left, float right,
                                String label, int colour, boolean hot) {
        RectF box = new RectF(left * EXIT_TEX_W, EXIT_BTN_T * EXIT_TEX_H,
                right * EXIT_TEX_W, EXIT_BTN_B * EXIT_TEX_H);

        if (hot) {
            paint.setStyle(Paint.Style.FILL);
            // The button's own colour, kept faint enough to read as a wash
            // behind the label rather than as a filled block
            paint.setColor((colour & 0x00FFFFFF) | 0x38000000);
            canvas.drawRoundRect(box, 16.0f, 16.0f, paint);
        }

        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(hot ? 5.0f : 3.0f);
        paint.setColor(colour);
        canvas.drawRoundRect(box, 16.0f, 16.0f, paint);
        paint.setStyle(Paint.Style.FILL);

        text.setColor(colour);
        text.setTextSize(30.0f);
        canvas.drawText(label, box.centerX(),
                box.centerY() - (text.ascent() + text.descent()) * 0.5f, text);
    }

    static ByteBuffer toBuffer(Bitmap bitmap) {
        ByteBuffer pixels = ByteBuffer.allocateDirect(
                bitmap.getWidth() * bitmap.getHeight() * 4);
        bitmap.copyPixelsToBuffer(pixels);
        pixels.rewind();
        return pixels;
    }

    // Sampled down on the way out of the JPEG, since a full 4096x2048 decode
    // for a 240 pixel tile would cost 32 MB apiece
    private Bitmap decodeThumb(String fileName, int wanted) {
        InputStream in = null;
        try {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            in = context.getAssets().open(ENVIRONMENT_DIR + "/" + fileName);
            BitmapFactory.decodeStream(in, null, bounds);
            closeQuietly(in);

            BitmapFactory.Options opts = new BitmapFactory.Options();
            opts.inSampleSize = 1;
            while (bounds.outHeight / (opts.inSampleSize * 2) >= wanted) {
                opts.inSampleSize *= 2;
            }

            in = context.getAssets().open(ENVIRONMENT_DIR + "/" + fileName);
            Bitmap thumb = BitmapFactory.decodeStream(in, null, opts);
            // A square panorama is top/bottom stereo: thumb from the top half,
            // or the crop lands on the seam between the two eyes
            if (thumb != null && thumb.getWidth() == thumb.getHeight()) {
                Bitmap top = Bitmap.createBitmap(thumb, 0, 0,
                        thumb.getWidth(), thumb.getHeight() / 2);
                thumb.recycle();
                return top;
            }
            return thumb;
        } catch (IOException | OutOfMemoryError e) {
            LimeLog.warning("Thumbnail " + fileName + " failed: " + e);
            return null;
        } finally {
            closeQuietly(in);
        }
    }

    // spaichingen_hill.jpg becomes Spaichingen Hill
    static String labelFor(String fileName) {
        int dot = fileName.lastIndexOf('.');
        String base = dot > 0 ? fileName.substring(0, dot) : fileName;
        StringBuilder out = new StringBuilder(base.length());
        boolean wordStart = true;
        for (int i = 0; i < base.length(); i++) {
            char c = base.charAt(i) == '_' ? ' ' : base.charAt(i);
            out.append(wordStart ? Character.toUpperCase(c) : c);
            wordStart = c == ' ';
        }
        return out.toString();
    }

    static void closeQuietly(InputStream in) {
        if (in != null) {
            try {
                in.close();
            } catch (IOException ignored) {
            }
        }
    }
}

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
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;

import com.limelight.BuildConfig;
import com.limelight.FileLog;
import com.limelight.LimeLog;
import com.limelight.R;
import com.limelight.utils.BugReport;

import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

import static com.limelight.binding.video.XrShared.*;

/**
 * The flat panels reachable from inside the session: the environment picker,
 * the settings sheets, the keyboard and the exit prompt, and the buttons along
 * the bar that open them or switch the 3D, with the splash the session opens
 * on, the hand lock hint and the Ko-fi sheet. Java is the only place Android
 * will lay out text, so their art is drawn to bitmaps here and handed back as
 * pixels for the frame loop to upload, since that thread owns the GL context.
 * Nothing in here touches the session, so it can run on whichever thread has
 * the time.
 */
final class XrPanels {

    // Environment picker, a grid of thumbnails reachable from inside the
    // session: a header strip carrying its name, then a row of cells under it.
    // A cell is a place in the grid and nothing more, what gets saved is the
    // stable id it maps to. The grid and its cells are the PICKER_ and
    // ENV_CELL_ values in XrShared, which is what the native side hit tests
    // against.
    private static final String IMAGE_DIR = "images";
    // The Ko-fi sheet's QR code, as tools/make_qr.py writes it
    private static final String KOFI_QR = "kofi_qr.png";
    // A baked room shows a picture of itself on its tile, at the cell's own size
    private static final String THEATER_THUMB = "rooms/thumbs/home_theater.jpg";
    private static final String GRAND_CINEMA_THUMB = "rooms/thumbs/grand_cinema.jpg";
    private static final String SYNTHWAVE_THUMB = "rooms/thumbs/synthwave.jpg";
    private static final int PICKER_CELL_W = PICKER_TEX_W / PICKER_COLS;
    // One per band, drawn in the strip above its cells
    private static final int[] PICKER_HEADERS = { R.string.vr_panel_rooms };
    // Each cell's name on its tile, in ENV_CELL_ order, as the 2d settings
    // name the same choices
    private static final int[] PICKER_NAMES = { R.string.vr_environment_passthrough,
            R.string.vr_environment_void, R.string.vr_environment_home_theater,
            R.string.vr_environment_grand_cinema, R.string.vr_environment_synthwave };

    // The settings panel behind the cog button. Drawn here, placed and dragged
    // natively, so the layout is agreed between the two through the COG_
    // values in XrShared. Five tabs, a texture each, all uploaded once so
    // switching is free, and the sheets a 3d room shows handed over after
    // them, which the native side picks for itself: the Room tab in the first
    // tab's place, and the other four again with its name over that slot.
    private static final int[] COG_TABS = { R.string.vr_panel_tab_screen,
            R.string.vr_panel_tab_display, R.string.vr_panel_3d, R.string.vr_panel_tab_picture,
            R.string.vr_panel_tab_about };
    private static final int COG_ROOM_TAB = R.string.vr_panel_tab_room;
    // Screen tab: the six that place the screen, then head aim's two, which
    // only act with the screen locked to the head and are greyed otherwise
    private static final int[] COG_SLIDER_ROWS =
            { R.string.vr_panel_distance, R.string.vr_panel_height, R.string.vr_panel_tilt,
              R.string.vr_panel_rotate, R.string.vr_panel_curve, R.string.vr_panel_size,
              R.string.vr_panel_aim_sensitivity, R.string.vr_panel_aim_dead_zone };
    // In place of a row's track or cells where head aim cannot act: the
    // screen not locked to the head, or a room up, which hangs it on a wall
    private static final int COG_NEEDS_HEAD_LOCK = R.string.vr_panel_needs_head_lock;
    private static final int COG_NOT_IN_ROOM = R.string.vr_panel_not_in_room;
    // Room tab: a track, two rows of cells and two more tracks, in the
    // COG_ROOM_ROW_ order. The screen light is one switch for every room and
    // the rest are the room's own.
    private static final int[] COG_ROOM_ROWS =
            { R.string.vr_panel_brightness, R.string.vr_panel_glow, R.string.vr_panel_screen_light,
              R.string.vr_panel_light_level, R.string.vr_panel_size };
    private static final int[] COG_ROOM_SWITCH = { R.string.vr_panel_off, R.string.vr_panel_on };
    // Under the size row where the room keeps its picture whole
    private static final int COG_ROOM_FIXED_HINT = R.string.vr_panel_fixed_size;
    // Display tab: a label and a row of cells, one of which is in force, and
    // the glow level track under them. Head locked, head aim, the controllers
    // as pointer or gamepad, pointer sleep, the ray, the controller model and
    // the click sit with the picture rows so the two light rows and the level
    // track they belong with stay together at the bottom. Screen light is the
    // wash the picture throws over a 3d room, which only shows in one, and
    // head lock is ignored in one, but both stay live here like the rest: the
    // picker can put a room up at any moment. The head aim, controllers and
    // ray rows are the bar's buttons, for the session only, and head aim's is
    // greyed where it cannot act.
    private static final int[] COG_OPTION_ROWS = { R.string.vr_panel_sharpen,
            R.string.vr_panel_supersample, R.string.vr_panel_stats, R.string.vr_panel_head_locked,
            R.string.vr_panel_head_aim, R.string.vr_panel_controllers,
            R.string.vr_panel_pointer_sleep, R.string.vr_panel_ray,
            R.string.vr_panel_controller_model, R.string.vr_panel_click_sound,
            R.string.vr_panel_glow, R.string.vr_panel_screen_light };
    private static final int[][] COG_OPTION_CELLS = {
            { R.string.vr_panel_off, R.string.vr_panel_normal, R.string.vr_panel_quality },
            { R.string.vr_panel_off, R.string.vr_panel_normal, R.string.vr_panel_quality },
            { R.string.vr_panel_off, R.string.vr_panel_on },
            { R.string.vr_panel_off, R.string.vr_panel_on },
            { R.string.vr_panel_off, R.string.vr_panel_on },
            { R.string.vr_panel_pointer, R.string.vr_panel_gamepad },
            { R.string.vr_panel_off, R.string.vr_panel_on },
            { R.string.vr_panel_off, R.string.vr_panel_on },
            { R.string.vr_panel_off, R.string.vr_panel_on },
            { R.string.vr_panel_off, R.string.vr_panel_on },
            { R.string.vr_panel_off, R.string.vr_panel_on },
            { R.string.vr_panel_off, R.string.vr_panel_on }
    };
    // 3D tab: a row of presets over two sliders, then the switch the bar's 3D
    // button works too, drawn the same way the display tab's cells and the
    // screen tab's tracks are, in the COG_ROW3D_ order. Only values that take
    // effect the moment they move belong on the panel, which is why the depth
    // source itself stays in the 2d settings.
    private static final int COG_PRESET_ROW = R.string.vr_panel_preset;
    // In cell order, as DepthPresets numbers them
    private static final int[] COG_PRESETS =
            { R.string.vr_panel_comfort, R.string.vr_panel_balanced, R.string.vr_panel_strong };
    private static final int[] COG_SLIDER3D_ROWS =
            { R.string.vr_panel_depth, R.string.vr_panel_convergence };
    private static final int COG_STEREO_ROW = R.string.vr_panel_3d;
    // Picture tab: a track for each of the grade's values, in the PICTURE_
    // order, each ticked where it leaves the picture as streamed
    private static final int[] COG_PICTURE_ROWS =
            { R.string.vr_panel_brightness, R.string.vr_panel_contrast, R.string.vr_panel_gamma,
              R.string.vr_panel_saturation };
    // About tab: the app, its version and its log, over the report button
    private static final String COG_ABOUT_NAME = "Moonlight XR";

    // The in world keyboard. Four sheets of the same layout, one per state,
    // handed over in state order, along with the geometry that goes with them:
    // the native side is given key rectangles and codes and knows nothing else
    // about it. The sheet size and the code values are the KB_ values in
    // XrShared.
    // Key widths per row, in units where a plain key is 1, and where each row
    // starts. One table for all the states, so every state has to lay its
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

    // A key's label is what it types, or a string id where it is named by a
    // word, which goes in the language the app is in
    private static final Object[][] KB_LABELS_LOWER = {
            { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
            { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p" },
            { "a", "s", "d", "f", "g", "h", "j", "k", "l" },
            { R.string.vr_key_shift, "z", "x", "c", "v", "b", "n", "m", "Del" },
            { "?123", ",", R.string.vr_key_space, ".", R.string.vr_key_enter,
              R.string.vr_key_hide }
    };
    private static final int[][] KB_CODES_LOWER = {
            { '1', '2', '3', '4', '5', '6', '7', '8', '9', '0' },
            { 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p' },
            { 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l' },
            { KB_CODE_SHIFT, 'z', 'x', 'c', 'v', 'b', 'n', 'm', 8 },
            { KB_CODE_SYMBOLS, ',', 32, '.', 13, KB_CODE_HIDE }
    };
    private static final Object[][] KB_LABELS_UPPER = {
            { "!", "@", "#", "$", "%", "^", "&", "*", "(", ")" },
            { "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P" },
            { "A", "S", "D", "F", "G", "H", "J", "K", "L" },
            { R.string.vr_key_shift, "Z", "X", "C", "V", "B", "N", "M", "Del" },
            { "?123", ",", R.string.vr_key_space, ".", R.string.vr_key_enter,
              R.string.vr_key_hide }
    };
    private static final int[][] KB_CODES_UPPER = {
            { '!', '@', '#', '$', '%', '^', '&', '*', '(', ')' },
            { 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P' },
            { 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L' },
            { KB_CODE_SHIFT, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', 8 },
            { KB_CODE_SYMBOLS, ',', 32, '.', 13, KB_CODE_HIDE }
    };
    // The brackets row has one slot fewer than a letter row, since the
    // geometry is shared, so the Fn sheet's key takes the place shift had.
    // Tab is on that sheet.
    private static final Object[][] KB_LABELS_SYMBOLS = {
            { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
            { "@", "#", "$", "%", "&", "*", "-", "+", "(", ")" },
            { "!", "\"", "'", ":", ";", "/", "?", "_", "=" },
            { "Fn", "<", ">", "[", "]", "{", "}", "\\", "Del" },
            { "ABC", ",", R.string.vr_key_space, ".", R.string.vr_key_enter,
              R.string.vr_key_hide }
    };
    private static final int[][] KB_CODES_SYMBOLS = {
            { '1', '2', '3', '4', '5', '6', '7', '8', '9', '0' },
            { '@', '#', '$', '%', '&', '*', '-', '+', '(', ')' },
            { '!', '"', '\'', ':', ';', '/', '?', '_', '=' },
            { KB_CODE_FN, '<', '>', '[', ']', '{', '}', '\\', 8 },
            { KB_CODE_SYMBOLS, ',', 32, '.', 13, KB_CODE_HIDE }
    };
    // The keys that type nothing. Esc and Tab lead two rows of F keys with
    // the editing keys after them as a real keyboard has them, the arrows sit
    // as an inverted T with up over down, and Ctrl, Win and Alt stay lit until
    // the next key goes with them. ?123 sits where Fn does on the symbols, so
    // the one place flips between the two. A null label is a blank with no
    // key, code 0. Backspace says so here, since Del is the delete key.
    private static final Object[][] KB_LABELS_FN = {
            { "Esc", "F1", "F2", "F3", "F4", "F5", "F6", "Ins", "Home", "PgUp" },
            { "Tab", "F7", "F8", "F9", "F10", "F11", "F12", "Del", "End", "PgDn" },
            { null, null, null, null, null, null, "\u2191", null, null },
            { "?123", R.string.vr_key_ctrl, R.string.vr_key_win, R.string.vr_key_alt, null,
              "\u2190", "\u2193", "\u2192", "Bksp" },
            { "ABC", ",", R.string.vr_key_space, ".", R.string.vr_key_enter,
              R.string.vr_key_hide }
    };
    private static final int[][] KB_CODES_FN = {
            { vk(0x1B), vk(0x70), vk(0x71), vk(0x72), vk(0x73), vk(0x74), vk(0x75),
              vk(0x2D), vk(0x24), vk(0x21) },
            { 9, vk(0x76), vk(0x77), vk(0x78), vk(0x79), vk(0x7A), vk(0x7B),
              vk(0x2E), vk(0x23), vk(0x22) },
            { 0, 0, 0, 0, 0, 0, vk(0x26), 0, 0 },
            { KB_CODE_FN, KB_CODE_CTRL, KB_CODE_WIN, KB_CODE_ALT, 0, vk(0x25), vk(0x28), vk(0x27), 8 },
            { KB_CODE_SYMBOLS, ',', 32, '.', 13, KB_CODE_HIDE }
    };
    // Every sheet's labels and codes in KB_STATE_ order
    private static final Object[][][] KB_LABELS = {
            KB_LABELS_LOWER, KB_LABELS_UPPER, KB_LABELS_SYMBOLS, KB_LABELS_FN
    };
    private static final int[][][] KB_CODES = {
            KB_CODES_LOWER, KB_CODES_UPPER, KB_CODES_SYMBOLS, KB_CODES_FN
    };
    // The names the held modifiers go by, in KB_MOD_ bit order
    private static final int[] KB_MOD_BITS = { KB_MOD_CTRL, KB_MOD_ALT, KB_MOD_WIN };
    private static final int[] KB_MOD_NAMES =
            { R.string.vr_key_ctrl, R.string.vr_key_alt, R.string.vr_key_win };

    // A Windows virtual key code as the keyboard carries it
    private static int vk(int code) {
        return KB_CODE_VK + code;
    }

    // The button that ends the stream and the prompt it opens. One sheet per
    // lit button, in zone order, so which one shows is a swapchain handle on
    // the native side rather than an upload. The sheet and where its buttons
    // sit on it are the EXIT_ values in XrShared.
    private static final int EXIT_QUESTION = R.string.vr_exit_question;

    // What the launch splash says while the session comes up
    private static final String SPLASH_NAME = "Moonlight XR";
    private static final int SPLASH_LOADING = R.string.vr_splash_loading;

    private final Context context;
    // The gamepad mode shortcut chosen, a PAD_SHORTCUT_ value, which the
    // Display tab names under its controllers row
    private final int gamepadShortcut;

    // The words above in the language the app is in, looked up once here,
    // since a panel only ever lives as long as one session
    private final String[] pickerHeaders;
    private final String[] pickerNames;
    private final String[] cogTabs;
    private final String cogRoomTab;
    private final String[] sliderRows;
    private final String needsHeadLock;
    private final String notInRoom;
    private final String[] roomRows;
    private final String[] roomSwitch;
    private final String roomFixedHint;
    private final String[] optionRows;
    private final String[][] optionCells;
    private final String presetRow;
    private final String[] presets;
    private final String[] slider3dRows;
    private final String stereoRow;
    private final String[] pictureRows;
    private final String resetWord;
    private final String[][][] kbLabels;
    private final String[] kbModNames;
    private final String exitQuestion;
    private final String splashLoading;

    XrPanels(Context context, int gamepadShortcut) {
        this.context = context;
        this.gamepadShortcut = gamepadShortcut;
        pickerHeaders = words(PICKER_HEADERS);
        pickerNames = words(PICKER_NAMES);
        cogTabs = words(COG_TABS);
        cogRoomTab = context.getString(COG_ROOM_TAB);
        sliderRows = words(COG_SLIDER_ROWS);
        needsHeadLock = context.getString(COG_NEEDS_HEAD_LOCK);
        notInRoom = context.getString(COG_NOT_IN_ROOM);
        roomRows = words(COG_ROOM_ROWS);
        roomSwitch = words(COG_ROOM_SWITCH);
        roomFixedHint = context.getString(COG_ROOM_FIXED_HINT);
        optionRows = words(COG_OPTION_ROWS);
        optionCells = new String[COG_OPTION_CELLS.length][];
        for (int row = 0; row < COG_OPTION_CELLS.length; row++) {
            optionCells[row] = words(COG_OPTION_CELLS[row]);
        }
        presetRow = context.getString(COG_PRESET_ROW);
        presets = words(COG_PRESETS);
        slider3dRows = words(COG_SLIDER3D_ROWS);
        stereoRow = context.getString(COG_STEREO_ROW);
        pictureRows = words(COG_PICTURE_ROWS);
        resetWord = context.getString(R.string.vr_panel_reset);
        kbLabels = new String[KB_LABELS.length][][];
        for (int state = 0; state < KB_LABELS.length; state++) {
            kbLabels[state] = new String[KB_LABELS[state].length][];
            for (int row = 0; row < KB_LABELS[state].length; row++) {
                Object[] keys = KB_LABELS[state][row];
                kbLabels[state][row] = new String[keys.length];
                for (int key = 0; key < keys.length; key++) {
                    kbLabels[state][row][key] = keys[key] instanceof Integer
                            ? context.getString((Integer) keys[key]) : (String) keys[key];
                }
            }
        }
        kbModNames = words(KB_MOD_NAMES);
        exitQuestion = context.getString(EXIT_QUESTION);
        splashLoading = context.getString(SPLASH_LOADING);
    }

    private String[] words(int[] ids) {
        String[] words = new String[ids.length];
        for (int i = 0; i < ids.length; i++) {
            words[i] = context.getString(ids[i]);
        }
        return words;
    }

    // The toast's way out of gamepad mode, by the shortcut chosen
    static int gamepadBackText(int shortcut) {
        switch (shortcut) {
            case PAD_SHORTCUT_STICKS:
                return R.string.vr_toast_gamepad_back_sticks;
            case PAD_SHORTCUT_TRIGGERS_GRIPS:
                return R.string.vr_toast_gamepad_back_triggers_grips;
            default:
                return R.string.vr_toast_gamepad_back_menu_grip;
        }
    }

    // Under the controllers row on the Display tab: the shortcut that switches
    // the same thing from the controllers
    static int controllersHint(int shortcut) {
        switch (shortcut) {
            case PAD_SHORTCUT_STICKS:
                return R.string.vr_panel_shortcut_sticks;
            case PAD_SHORTCUT_TRIGGERS_GRIPS:
                return R.string.vr_panel_shortcut_triggers_grips;
            default:
                return R.string.vr_panel_shortcut_menu_grip;
        }
    }

    // Everything the keyboard hands over: the sheets and their codes in state
    // order, the button that opens it, and the geometry they were all drawn
    // from
    static final class Keyboard {
        final ByteBuffer[] sheets;
        final ByteBuffer button;
        final float[] keyRects;
        final int[][] codes;

        Keyboard(ByteBuffer[] sheets, ByteBuffer button, float[] keyRects, int[][] codes) {
            this.sheets = sheets;
            this.button = button;
            this.keyRects = keyRects;
            this.codes = codes;
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
        for (int band = 0; band < PICKER_ROWS && band < pickerHeaders.length; band++) {
            canvas.drawText(pickerHeaders[band], PICKER_TEX_W * 0.5f,
                    band * PICKER_BAND_PX + baseline, header);
        }

        for (int cell = 0; cell < PICKER_CELLS && cell < pickerNames.length; cell++) {
            RectF tile = pickerTile(cell, pad);

            String name = pickerNames[cell];
            Bitmap thumb = null;
            if (cell == ENV_CELL_PASSTHROUGH) {
                paint.setColor(0xFF2A3540);
            }
            else if (cell == ENV_CELL_VOID) {
                paint.setColor(0xFF090909);
            }
            else if (cell == ENV_CELL_HOME_THEATER) {
                thumb = decodeRoomThumb(THEATER_THUMB);
                paint.setColor(0xFF14110F);
            }
            else if (cell == ENV_CELL_GRAND_CINEMA) {
                thumb = decodeRoomThumb(GRAND_CINEMA_THUMB);
                paint.setColor(0xFF1A0A0B);
            }
            else if (cell == ENV_CELL_SYNTHWAVE) {
                thumb = decodeRoomThumb(SYNTHWAVE_THUMB);
                paint.setColor(0xFF140B20);
            }
            else {
                continue;
            }

            if (thumb != null) {
                // Scaled to cover and centred, so a picture of another shape
                // fills the tile rather than leaving bars
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

            // A long name is drawn smaller rather than run into the tile's edge
            float size = 21.0f;
            label.setTextSize(size);
            while (label.measureText(name) > tile.width() - 16.0f && size > 15.0f) {
                size -= 1.0f;
                label.setTextSize(size);
            }
            canvas.drawText(name, tile.centerX(), tile.bottom - 15.0f, label);
        }

        ByteBuffer pixels = toBuffer(grid);
        grid.recycle();
        return pixels;
    }

    // The cog ships as a PNG. There is nothing to tint or dim here, just a
    // decode and a downscale to whatever the swapchain it is headed for wants.
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
        Bitmap button = Bitmap.createBitmap(BUTTON_TEX, BUTTON_TEX,
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

    /**
     * The settings panel. A texture per tab, and the ones a room shows in
     * their place, all drawn once here in COG_ART_ order, so changing tab in
     * the session picks another swapchain rather than redrawing anything. Only
     * the labels, tracks and cells live in the texture: thumbs, selection
     * rings and the values beside the Room and Picture tabs' tracks are quads
     * of their own, so using the panel costs no upload of a whole sheet. The
     * 3D tab's ticks mark the running model's own pair, in the preferences'
     * units.
     */
    ByteBuffer[] buildCogTabs(boolean curveOk, boolean stereoOk, int defaultSeparation,
                              int defaultConvergence) {
        ByteBuffer[] sheets = new ByteBuffer[COG_ART_COUNT];
        for (int art = 0; art < COG_ART_COUNT; art++) {
            Bitmap sheet = buildCogSheet(art, curveOk, stereoOk, defaultSeparation,
                    defaultConvergence);
            sheets[art] = toBuffer(sheet);
            sheet.recycle();
        }
        return sheets;
    }

    // The cog that opens the settings panel
    ByteBuffer buildCogButton() {
        // Never blank: the drawn gear stands in if the art does not decode
        Bitmap button = loadIcon("settings_icon.png", BUTTON_TEX);
        if (button == null) {
            button = buildCogFallback();
        }
        ByteBuffer pixels = toBuffer(button);
        button.recycle();
        return pixels;
    }

    // One sheet of the panel. The ones past the tabs are what a room shows:
    // the Room tab with its size row live or greyed, then the display, 3D,
    // Picture and About tabs with the Room tab's name over the first slot.
    // Last come the screen and display tabs for a screen not locked to the
    // head, with head aim's rows greyed, as the room's display tab has them.
    private Bitmap buildCogSheet(int art, boolean curveOk, boolean stereoOk,
                                 int defaultSeparation, int defaultConvergence) {
        Bitmap bitmap = Bitmap.createBitmap(COG_TEX_W, COG_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        boolean inRoom = art >= COG_ART_ROOM && art <= COG_ART_ROOM_ABOUT;
        boolean aimLive = art == COG_TAB_SCREEN || art == COG_TAB_DISPLAY;
        int tab = art == COG_ART_ROOM_DISPLAY || art == COG_ART_DISPLAY_WORLD ? COG_TAB_DISPLAY
                : art == COG_ART_ROOM_3D ? COG_TAB_3D
                : art == COG_ART_ROOM_PICTURE ? COG_TAB_PICTURE
                : art == COG_ART_ROOM_ABOUT ? COG_TAB_ABOUT
                : art == COG_ART_SCREEN_WORLD ? COG_TAB_SCREEN
                : inRoom ? COG_TAB_SCREEN : art;
        drawCogChrome(canvas, tab, inRoom);
        if (art == COG_ART_ROOM || art == COG_ART_ROOM_FIXED) {
            drawCogRoomRows(canvas, art == COG_ART_ROOM);
        }
        else if (tab == COG_TAB_SCREEN) {
            drawCogSliderRows(canvas, curveOk, aimLive);
        }
        else if (tab == COG_TAB_3D) {
            drawCog3dRows(canvas, stereoOk, defaultSeparation, defaultConvergence);
        }
        else if (tab == COG_TAB_PICTURE) {
            drawCogPictureRows(canvas);
        }
        else if (tab == COG_TAB_ABOUT) {
            drawCogAbout(canvas);
        }
        else {
            drawCogOptionRows(canvas, aimLive, inRoom ? notInRoom : needsHeadLock);
        }
        return bitmap;
    }

    // Background and tab bar, the part every tab has in common. The tab this
    // texture belongs to is the one drawn as current, and in a room the first
    // slot is the Room tab.
    private void drawCogChrome(Canvas canvas, int tab, boolean inRoom) {
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        paint.setColor(0xF0141416);
        canvas.drawRoundRect(new RectF(1.0f, 1.0f, COG_TEX_W - 1.0f, COG_TEX_H - 1.0f),
                32.0f, 32.0f, paint);

        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextSize(25.0f);
        text.setTextAlign(Paint.Align.CENTER);

        final float barB = COG_TAB_BAR_B * COG_TEX_H;
        final float slotW = COG_TEX_W / (float)cogTabs.length;
        for (int i = 0; i < cogTabs.length; i++) {
            boolean current = i == tab;
            RectF slot = new RectF(i * slotW + 12.0f, 12.0f, (i + 1) * slotW - 12.0f, barB - 8.0f);

            if (current) {
                paint.setColor(0x28FFFFFF);
                canvas.drawRoundRect(slot, 14.0f, 14.0f, paint);
            }

            text.setColor(current ? Color.WHITE : 0x60FFFFFF);
            String name = inRoom && i == COG_TAB_SCREEN ? cogRoomTab : cogTabs[i];
            canvas.drawText(name, slot.centerX(),
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

    // Screen tab: a label and a track per row, and the reset button under
    // them. Head aim's two rows are greyed with the reason in place of their
    // tracks where the screen is not locked to the head.
    private void drawCogSliderRows(Canvas canvas, boolean curveOk, boolean aimLive) {
        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextSize(22.0f);
        text.setTextAlign(Paint.Align.LEFT);

        Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
        track.setStyle(Paint.Style.STROKE);
        track.setStrokeWidth(6.0f);
        track.setStrokeCap(Paint.Cap.ROUND);

        Paint tick = new Paint(Paint.ANTI_ALIAS_FLAG);
        tick.setColor(0xCCFFFFFF);

        final float cellHalf = COG_SCREEN_CELL_HALF * COG_TEX_H;
        for (int row = 0; row < sliderRows.length; row++) {
            boolean aimRow = row == COG_SLIDER_AIM_SENSITIVITY || row == COG_SLIDER_AIM_DEADZONE;
            boolean live = aimRow ? aimLive : row != COG_SLIDER_CURVE || curveOk;
            float y = cogRowV(COG_TAB_SCREEN, row) * COG_TEX_H;

            text.setColor(live ? Color.WHITE : 0x30FFFFFF);
            // Centred on the row rather than sitting on it, so the label lines
            // up with the track beside it
            canvas.drawText(sliderRows[row], 0.06f * COG_TEX_W,
                    y - (text.ascent() + text.descent()) * 0.5f, text);

            if (aimRow && !live) {
                drawGreyedReason(canvas, y, needsHeadLock);
                continue;
            }

            track.setColor(live ? 0x66FFFFFF : 0x30FFFFFF);
            canvas.drawLine(COG_RUN_L * COG_TEX_W, y, COG_RUN_R * COG_TEX_W, y, track);
            drawCogChevrons(canvas, y, live, cellHalf);

            if (row == COG_SLIDER_TILT || row == COG_SLIDER_ROTATE) {
                // Marks level, which is where the middle of these two tracks
                // snaps to. The placement rows that do not snap stay unmarked.
                float midX = (COG_RUN_L + COG_RUN_R) * 0.5f * COG_TEX_W;
                canvas.drawRect(midX - 2.0f, y - cellHalf, midX + 2.0f, y + cellHalf, tick);
            }
            else if (aimRow) {
                // Head aim's defaults, as the Picture tab marks the picture
                // as streamed
                float t = row == COG_SLIDER_AIM_SENSITIVITY
                        ? (HEAD_AIM_SENSITIVITY_DEFAULT - HEAD_AIM_SENSITIVITY_MIN)
                                / (float)(HEAD_AIM_SENSITIVITY_MAX - HEAD_AIM_SENSITIVITY_MIN)
                        : (HEAD_AIM_DEADZONE_DEFAULT - HEAD_AIM_DEADZONE_MIN)
                                / (float)(HEAD_AIM_DEADZONE_MAX - HEAD_AIM_DEADZONE_MIN);
                float markX = (COG_RUN_L + t * (COG_RUN_R - COG_RUN_L)) * COG_TEX_W;
                canvas.drawRect(markX - 2.0f, y - cellHalf, markX + 2.0f, y + cellHalf, tick);
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
        canvas.drawText(resetWord, reset.centerX(),
                reset.centerY() - (text.ascent() + text.descent()) * 0.5f, text);
    }

    // Room tab: the room's own brightness, its glow, the screen light and the
    // light's level, and the picture's size inside the room's screen. Drawn
    // the way the other tabs draw their tracks and cells. The brightness and
    // size rows start somewhere different in every room, so only the light
    // level, which starts in the same place everywhere, carries a tick. Where
    // the room keeps its picture whole the size row is greyed with the reason
    // under it.
    private void drawCogRoomRows(Canvas canvas, boolean sizeLive) {
        Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
        label.setTextSize(22.0f);
        label.setTextAlign(Paint.Align.LEFT);

        Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
        track.setStyle(Paint.Style.STROKE);
        track.setStrokeWidth(6.0f);
        track.setStrokeCap(Paint.Cap.ROUND);

        Paint tick = new Paint(Paint.ANTI_ALIAS_FLAG);
        tick.setColor(0xCCFFFFFF);

        final float trackL = COG_RUN_L * COG_TEX_W;
        final float trackR = COG_RUN_R * COG_TEX_W;
        final float cellHalf = COG_CELL_HALF * COG_TEX_H;

        for (int row = 0; row < roomRows.length; row++) {
            boolean live = row != COG_ROOM_ROW_SIZE || sizeLive;
            float y = (COG_ROW_V0 + row * COG_ROW_STEP) * COG_TEX_H;
            label.setColor(live ? Color.WHITE : 0x30FFFFFF);
            canvas.drawText(roomRows[row], 0.06f * COG_TEX_W,
                    y - (label.ascent() + label.descent()) * 0.5f, label);

            if (row == COG_ROOM_ROW_GLOW || row == COG_ROOM_ROW_LIGHT) {
                drawCogCells(canvas, roomSwitch, y);
                continue;
            }

            track.setColor(live ? 0x66FFFFFF : 0x30FFFFFF);
            canvas.drawLine(trackL, y, trackR, y, track);
            drawCogChevrons(canvas, y, live, cellHalf);

            if (row == COG_ROOM_ROW_LIGHT_LEVEL) {
                float markX = trackL + (ROOM_LIGHT_DEFAULT - ROOM_LIGHT_MIN)
                        / (float)(ROOM_LIGHT_MAX - ROOM_LIGHT_MIN) * (trackR - trackL);
                canvas.drawRect(markX - 2.0f, y - cellHalf, markX + 2.0f, y + cellHalf, tick);
            }
        }

        if (!sizeLive) {
            // Otherwise a dead track with no explanation, the way the 3D tab
            // says why its rows are greyed
            Paint hint = new Paint(Paint.ANTI_ALIAS_FLAG);
            hint.setTextSize(17.0f);
            hint.setTextAlign(Paint.Align.CENTER);
            hint.setColor(0x50FFFFFF);
            float y = (COG_ROW_V0 + (COG_ROOM_ROW_SIZE + 1) * COG_ROW_STEP) * COG_TEX_H;
            canvas.drawText(roomFixedHint, COG_TEX_W * 0.5f, y, hint);
        }
    }

    // Where a row sits down the panel, as a fraction of its height: the same
    // as cogRowV in xr_layout.c, which hit tests and rings what this draws.
    // The display and screen tabs pack their rows closer than the other tabs.
    static float cogRowV(int tab, int row) {
        if (tab == COG_TAB_DISPLAY) {
            return COG_DISPLAY_ROW_V0 + row * COG_DISPLAY_ROW_STEP;
        }
        if (tab == COG_TAB_SCREEN) {
            return COG_SCREEN_ROW_V0 + row * COG_SCREEN_ROW_STEP;
        }
        return COG_ROW_V0 + row * COG_ROW_STEP;
    }

    // Why a head aim row is greyed, across where its track or cells would be
    private static void drawGreyedReason(Canvas canvas, float y, String reason) {
        Paint hint = new Paint(Paint.ANTI_ALIAS_FLAG);
        hint.setTextSize(19.0f);
        hint.setTextAlign(Paint.Align.CENTER);
        hint.setColor(0x50FFFFFF);
        canvas.drawText(reason, (COG_TRACK_L + COG_TRACK_R) * 0.5f * COG_TEX_W,
                y - (hint.ascent() + hint.descent()) * 0.5f, hint);
    }

    // One row of cells, one press wide each, as the display tab draws them
    private static void drawCogCells(Canvas canvas, String[] names, float y) {
        drawCogCells(canvas, names, y, true);
    }

    // The same, greyed like a dead track where the row can do nothing
    private static void drawCogCells(Canvas canvas, String[] names, float y, boolean live) {
        drawCogCells(canvas, names, y, live, COG_CELL_HALF * COG_TEX_H);
    }

    // The same again at a height of its own, for the display tab's closer rows
    private static void drawCogCells(Canvas canvas, String[] names, float y, boolean live,
                                     float cellHalf) {
        Paint cellText = new Paint(Paint.ANTI_ALIAS_FLAG);
        cellText.setTextSize(19.0f);
        cellText.setTextAlign(Paint.Align.CENTER);
        cellText.setColor(live ? Color.WHITE : 0x30FFFFFF);

        Paint cell = new Paint(Paint.ANTI_ALIAS_FLAG);
        final float trackL = COG_TRACK_L * COG_TEX_W;
        final float trackR = COG_TRACK_R * COG_TEX_W;
        float span = (trackR - trackL) / names.length;
        for (int i = 0; i < names.length; i++) {
            // Inset so neighbours read as separate buttons rather than one
            // long strip
            RectF box = new RectF(trackL + i * span + 3.0f, y - cellHalf,
                    trackL + (i + 1) * span - 3.0f, y + cellHalf);

            cell.setStyle(Paint.Style.FILL);
            cell.setColor(live ? 0x28FFFFFF : 0x10FFFFFF);
            canvas.drawRoundRect(box, 10.0f, 10.0f, cell);
            cell.setStyle(Paint.Style.STROKE);
            cell.setStrokeWidth(2.0f);
            cell.setColor(live ? 0x50FFFFFF : 0x20FFFFFF);
            canvas.drawRoundRect(box, 10.0f, 10.0f, cell);

            canvas.drawText(names[i], box.centerX(),
                    box.centerY() - (cellText.ascent() + cellText.descent()) * 0.5f, cellText);
        }
    }

    // A step button at each end of a track, the size of a narrow cell with a
    // chevron in it pointing the way it steps the row, drawn the way the bar
    // and the corner brackets are: a rounded white stroke. Greyed with the
    // row, where they do nothing.
    private static void drawCogChevrons(Canvas canvas, float y, boolean live, float cellHalf) {
        Paint box = new Paint(Paint.ANTI_ALIAS_FLAG);
        Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(4.0f);
        stroke.setStrokeCap(Paint.Cap.ROUND);
        stroke.setStrokeJoin(Paint.Join.ROUND);
        stroke.setColor(live ? 0xEBFFFFFF : 0x30FFFFFF);
        float width = COG_CHEVRON_W * COG_TEX_W;
        float[] lefts = { COG_TRACK_L * COG_TEX_W, COG_TRACK_R * COG_TEX_W - width };
        for (int side = 0; side < 2; side++) {
            RectF cell = new RectF(lefts[side], y - cellHalf, lefts[side] + width, y + cellHalf);
            box.setStyle(Paint.Style.FILL);
            box.setColor(live ? 0x28FFFFFF : 0x10FFFFFF);
            canvas.drawRoundRect(cell, 8.0f, 8.0f, box);
            box.setStyle(Paint.Style.STROKE);
            box.setStrokeWidth(2.0f);
            box.setColor(live ? 0x50FFFFFF : 0x20FFFFFF);
            canvas.drawRoundRect(cell, 8.0f, 8.0f, box);

            // Points out of the track, left on the left and right on the right
            float dir = side == 0 ? -1.0f : 1.0f;
            float reach = width * 0.16f;
            float rise = cellHalf * 0.42f;
            Path chevron = new Path();
            chevron.moveTo(cell.centerX() - dir * reach, y - rise);
            chevron.lineTo(cell.centerX() + dir * reach, y);
            chevron.lineTo(cell.centerX() - dir * reach, y + rise);
            canvas.drawPath(chevron, stroke);
        }
    }

    /**
     * The strip of values beside the Room, Picture or Screen tab's tracks: the
     * Room tab's percents, the Picture tab's values in their own units, or
     * head aim's two on the Screen tab. Redrawn on the frame loop whenever one
     * of them moves, into the one bitmap and buffer, which the upload has
     * finished with by the time the next draw comes round.
     */
    static final class Readout {
        // The Room tab's rows the values after the first in IN_READOUT sit
        // beside. The Picture tab's are its rows in order.
        private static final int[] ROOM_ROWS =
                { COG_ROOM_ROW_BRIGHTNESS, COG_ROOM_ROW_LIGHT_LEVEL, COG_ROOM_ROW_SIZE };

        private final Bitmap bitmap = Bitmap.createBitmap(COG_READOUT_TEX_W, COG_READOUT_TEX_H,
                Bitmap.Config.ARGB_8888);
        private final Canvas canvas = new Canvas(bitmap);
        private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final ByteBuffer pixels =
                ByteBuffer.allocateDirect(COG_READOUT_TEX_W * COG_READOUT_TEX_H * 4);

        Readout() {
            text.setTextSize(20.0f);
            text.setTextAlign(Paint.Align.RIGHT);
            text.setColor(0xB0FFFFFF);
        }

        // In IN_READOUT order: which tab, then a value a row. Right aligned a
        // little short of the strip's edge, which is where the thumb at the
        // left end of a track starts. A Room tab value under zero leaves its
        // row blank; a Picture tab one is a real value.
        ByteBuffer draw(int[] values) {
            canvas.drawColor(0, PorterDuff.Mode.CLEAR);
            float right = COG_READOUT_TEX_W - 8.0f;
            boolean picture = values.length > 0 && values[0] == READOUT_PICTURE;
            boolean screen = values.length > 0 && values[0] == READOUT_SCREEN;
            int rows = picture ? PICTURE_VALUES : screen ? 2 : ROOM_ROWS.length;
            for (int i = 0; i < rows && i + 1 < values.length; i++) {
                int value = values[i + 1];
                String said;
                int row;
                if (picture) {
                    said = pictureReadout(i, value);
                    row = i;
                }
                else if (screen) {
                    row = COG_SLIDER_AIM_SENSITIVITY + i;
                    said = headAimReadout(row, value);
                }
                else if (values[0] == READOUT_ROOM && value >= 0) {
                    said = value + "%";
                    row = ROOM_ROWS[i];
                }
                else {
                    continue;
                }
                float rowV = screen ? cogRowV(COG_TAB_SCREEN, row)
                        : COG_ROW_V0 + row * COG_ROW_STEP;
                float y = (rowV - COG_READOUT_T) * COG_TEX_H;
                canvas.drawText(said, right, y - (text.ascent() + text.descent()) * 0.5f, text);
            }
            pixels.rewind();
            bitmap.copyPixelsToBuffer(pixels);
            pixels.rewind();
            return pixels;
        }
    }

    /**
     * One of head aim's values as the Screen tab says it, by its row: pixels
     * a degree, or the dead zone in degrees a second.
     */
    static String headAimReadout(int row, int units) {
        return row == COG_SLIDER_AIM_SENSITIVITY ? units + " px/\u00b0" : units + "\u00b0/s";
    }

    /**
     * A picture value as the Picture tab says it, from its whole units:
     * brightness signed, contrast and saturation in percent, gamma to the
     * hundredth.
     */
    static String pictureReadout(int row, int units) {
        switch (row) {
            case PICTURE_CONTRAST:
            case PICTURE_SATURATION:
                return units + "%";
            case PICTURE_GAMMA:
                return String.format(Locale.ROOT, "%.2f", units / 100.0);
            default:
                return units > 0 ? "+" + units : String.valueOf(units);
        }
    }

    /**
     * The marks on the display tab's cells: a ring round the cell in force on
     * each row, all in one strip over the column of cells, so the tab costs
     * one layer for them however many rows it has. Redrawn on the frame loop
     * whenever one moves, into the one bitmap and buffer, which the upload has
     * finished with by the time the next draw comes round. The ring under the
     * ray stays the native side's own, since it moves with every frame.
     */
    static final class Marks {
        private final Bitmap bitmap = Bitmap.createBitmap(COG_MARKS_TEX_W, COG_MARKS_TEX_H,
                Bitmap.Config.ARGB_8888);
        private final Canvas canvas = new Canvas(bitmap);
        private final Paint ring = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final ByteBuffer pixels =
                ByteBuffer.allocateDirect(COG_MARKS_TEX_W * COG_MARKS_TEX_H * 4);

        Marks() {
            ring.setStyle(Paint.Style.STROKE);
            ring.setStrokeWidth(3.0f);
            ring.setColor(Color.WHITE);
        }

        // One cell per row, by its place in the row, in COG_OPTION_ order. A
        // row under zero has nothing in force and goes unmarked.
        ByteBuffer draw(int[] cells) {
            canvas.drawColor(0, PorterDuff.Mode.CLEAR);
            float left = COG_MARKS_L * COG_TEX_W;
            float top = COG_MARKS_T * COG_TEX_H;
            float trackL = COG_TRACK_L * COG_TEX_W;
            float trackR = COG_TRACK_R * COG_TEX_W;
            float half = COG_DISPLAY_CELL_HALF * COG_TEX_H;
            for (int row = 0; row < COG_OPTION_COUNT && row < cells.length; row++) {
                int count = COG_OPTION_CELLS[row].length;
                if (cells[row] < 0 || cells[row] >= count) {
                    continue;
                }
                float span = (trackR - trackL) / count;
                float y = cogRowV(COG_TAB_DISPLAY, row) * COG_TEX_H - top;
                // On the edge of the cell the sheet draws, inset the same way
                float l = trackL + cells[row] * span + 3.0f - left;
                canvas.drawRoundRect(new RectF(l + 1.5f, y - half + 1.5f, l + span - 6.0f - 1.5f,
                        y + half - 1.5f), 10.0f, 10.0f, ring);
            }
            pixels.rewind();
            bitmap.copyPixelsToBuffer(pixels);
            pixels.rewind();
            return pixels;
        }
    }

    /**
     * The clock line over the settings panel: the time and the battery on a
     * small dark strip, drawn on the frame loop when the line changes, into
     * the one bitmap and buffer like the toast's.
     */
    static final class ClockStrip {
        private final Bitmap bitmap = Bitmap.createBitmap(COG_CLOCK_TEX_W, COG_CLOCK_TEX_H,
                Bitmap.Config.ARGB_8888);
        private final Canvas canvas = new Canvas(bitmap);
        private final Paint sheet = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final ByteBuffer pixels =
                ByteBuffer.allocateDirect(COG_CLOCK_TEX_W * COG_CLOCK_TEX_H * 4);

        ClockStrip() {
            sheet.setColor(0xF0141416);
            text.setColor(0xD0FFFFFF);
            text.setTextSize(26.0f);
            text.setTextAlign(Paint.Align.CENTER);
        }

        ByteBuffer draw(String line) {
            canvas.drawColor(0, PorterDuff.Mode.CLEAR);
            canvas.drawRoundRect(new RectF(1.0f, 1.0f, COG_CLOCK_TEX_W - 1.0f,
                    COG_CLOCK_TEX_H - 1.0f), 20.0f, 20.0f, sheet);
            canvas.drawText(line, COG_CLOCK_TEX_W * 0.5f,
                    COG_CLOCK_TEX_H * 0.5f - (text.ascent() + text.descent()) * 0.5f, text);
            pixels.rewind();
            bitmap.copyPixelsToBuffer(pixels);
            pixels.rewind();
            return pixels;
        }
    }

    /**
     * The toast: a line, and a quieter one under it when there is more to
     * say, on the same dark sheet the panels sit on. Redrawn on the frame loop
     * whenever a notice goes up, into the one bitmap and buffer, which the
     * upload has finished with by the time the next one comes round. Either
     * line is cut short with an ellipsis rather than let run off the sheet.
     */
    static final class Toast {
        private final Bitmap bitmap = Bitmap.createBitmap(TOAST_TEX_W, TOAST_TEX_H,
                Bitmap.Config.ARGB_8888);
        private final Canvas canvas = new Canvas(bitmap);
        private final Paint sheet = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint line = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint detail = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final ByteBuffer pixels = ByteBuffer.allocateDirect(TOAST_TEX_W * TOAST_TEX_H * 4);

        private static final float DETAIL_SIZE = 32.0f;
        private static final float DETAIL_SIZE_MIN = 22.0f;

        Toast() {
            sheet.setColor(0xF0141416);
            line.setColor(Color.WHITE);
            line.setTextSize(44.0f);
            line.setTextAlign(Paint.Align.CENTER);
            detail.setColor(0xB0FFFFFF);
            detail.setTextSize(DETAIL_SIZE);
            detail.setTextAlign(Paint.Align.CENTER);
        }

        // A second line that is a path is cut from the middle rather than the
        // end when it will not fit, so the file at its end stays in view
        ByteBuffer draw(String text, String more, boolean path) {
            canvas.drawColor(0, PorterDuff.Mode.CLEAR);
            canvas.drawRoundRect(new RectF(1.0f, 1.0f, TOAST_TEX_W - 1.0f, TOAST_TEX_H - 1.0f),
                    40.0f, 40.0f, sheet);
            float room = TOAST_TEX_W - 80.0f;
            float mid = TOAST_TEX_W * 0.5f;
            if (more == null || more.isEmpty()) {
                canvas.drawText(fit(text, line, room), mid,
                        TOAST_TEX_H * 0.5f - (line.ascent() + line.descent()) * 0.5f, line);
            }
            else {
                canvas.drawText(fit(text, line, room), mid, TOAST_TEX_H * 0.44f, line);
                // A path is what the second line most often carries, and its
                // end is the part that matters, so it gets smaller before it
                // gets cut
                float size = DETAIL_SIZE;
                detail.setTextSize(size);
                while (detail.measureText(more) > room && size > DETAIL_SIZE_MIN) {
                    size -= 1.0f;
                    detail.setTextSize(size);
                }
                canvas.drawText(path ? fitMiddle(more, detail, room) : fit(more, detail, room), mid,
                        TOAST_TEX_H * 0.78f, detail);
            }
            pixels.rewind();
            bitmap.copyPixelsToBuffer(pixels);
            pixels.rewind();
            return pixels;
        }

        // Trimmed from the middle until it fits round its ellipsis, a third
        // of what is kept from the start and the rest from the end
        static String fitMiddle(String text, Paint paint, float width) {
            if (paint.measureText(text) <= width) {
                return text;
            }
            for (int keep = text.length() - 1; keep > 1; keep--) {
                int head = keep / 3;
                String cut = text.substring(0, head) + "…"
                        + text.substring(text.length() - (keep - head));
                if (paint.measureText(cut) <= width) {
                    return cut;
                }
            }
            return "…";
        }

        // Trimmed a character at a time until it fits with its ellipsis
        static String fit(String text, Paint paint, float width) {
            if (paint.measureText(text) <= width) {
                return text;
            }
            int end = text.length();
            while (end > 0 && paint.measureText(text, 0, end) + paint.measureText("…") > width) {
                end--;
            }
            return text.substring(0, end).trim() + "…";
        }
    }

    // 3D tab: three presets, then the two values worth reaching mid stream,
    // then the switch. Depth runs past the comfortable range on purpose, with
    // the far end marked, since where that range ends is a matter of eyes
    // rather than of hardware. The ticks and the start of the marked end are
    // the running model's own pair, which is also where Balanced sits. Which
    // preset is in force, and which way the switch is set, are rings the
    // native side puts over their cells.
    private void drawCog3dRows(Canvas canvas, boolean stereoOk, int defaultSeparation,
                               int defaultConvergence) {
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

        final float trackL = COG_RUN_L * COG_TEX_W;
        final float trackR = COG_RUN_R * COG_TEX_W;
        final float tickHalf = COG_CELL_HALF * COG_TEX_H;

        float presetY = (COG_ROW_V0 + COG_ROW3D_PRESET * COG_ROW_STEP) * COG_TEX_H;
        canvas.drawText(presetRow, 0.06f * COG_TEX_W,
                presetY - (text.ascent() + text.descent()) * 0.5f, text);
        drawCogCells(canvas, presets, presetY, stereoOk);

        for (int i = 0; i < slider3dRows.length; i++) {
            int row = COG_ROW3D_SEPARATION + i;
            float y = (COG_ROW_V0 + row * COG_ROW_STEP) * COG_TEX_H;
            canvas.drawText(slider3dRows[i], 0.06f * COG_TEX_W,
                    y - (text.ascent() + text.descent()) * 0.5f, text);

            // A tick at the model's default on both tracks
            float markT = row == COG_ROW3D_SEPARATION ? defaultSeparation / (float)COG_SEP_STEPS
                    : defaultConvergence / 100.0f;
            float markX = trackL + markT * (trackR - trackL);

            if (row == COG_ROW3D_SEPARATION) {
                // Measured on device: past the model's default the depth
                // stops growing and only the strain does, so the rest of the
                // track is drawn as a place you can go rather than one you
                // should
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
                canvas.drawText(context.getString(R.string.vr_panel_harder_on_eyes),
                        (markX + trackR) * 0.5f, y + 0.04f * COG_TEX_H, caption);
            }
            else {
                track.setColor(stereoOk ? 0x66FFFFFF : 0x30FFFFFF);
                canvas.drawLine(trackL, y, trackR, y, track);
            }

            canvas.drawRect(markX - 2.0f, y - tickHalf, markX + 2.0f, y + tickHalf, tick);
            drawCogChevrons(canvas, y, stereoOk, tickHalf);
        }

        // The switch, which only lasts the session, the same one the bar's
        // button works, so the two never disagree
        float switchY = (COG_ROW_V0 + COG_ROW3D_SWITCH * COG_ROW_STEP) * COG_TEX_H;
        canvas.drawText(stereoRow, 0.06f * COG_TEX_W,
                switchY - (text.ascent() + text.descent()) * 0.5f, text);
        drawCogCells(canvas, roomSwitch, switchY, stereoOk);

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
        canvas.drawText(resetWord, reset.centerX(),
                reset.centerY() - (text.ascent() + text.descent()) * 0.5f, text);

        if (!stereoOk) {
            // Otherwise a tab of dead rows with no explanation. Under the
            // last of them, where the Room tab says why its size row is dead.
            Paint hint = new Paint(Paint.ANTI_ALIAS_FLAG);
            hint.setTextSize(17.0f);
            hint.setTextAlign(Paint.Align.CENTER);
            hint.setColor(0x50FFFFFF);
            canvas.drawText(context.getString(R.string.vr_panel_3d_off_in_settings),
                    COG_TEX_W * 0.5f,
                    (COG_ROW_V0 + COG_ROW3D_COUNT * COG_ROW_STEP) * COG_TEX_H, hint);
        }
    }

    // Display tab: a label and a row of cells, one press wide each. Which cell
    // is in force and which is under the ray are rings the native side puts
    // over them, so nothing here has to be redrawn when one is chosen. Head
    // aim's row is greyed with the reason in place of its cells where it
    // cannot act.
    private void drawCogOptionRows(Canvas canvas, boolean headAimLive, String headAimReason) {
        Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
        label.setTextSize(22.0f);
        label.setTextAlign(Paint.Align.LEFT);

        final float trackL = COG_RUN_L * COG_TEX_W;
        final float trackR = COG_RUN_R * COG_TEX_W;
        final float cellHalf = COG_DISPLAY_CELL_HALF * COG_TEX_H;

        Paint hint = new Paint(Paint.ANTI_ALIAS_FLAG);
        hint.setTextSize(15.0f);
        hint.setTextAlign(Paint.Align.LEFT);
        hint.setColor(0x99FFFFFF);
        for (int row = 0; row < optionRows.length; row++) {
            boolean live = row != COG_OPTION_HEAD_AIM || headAimLive;
            float y = cogRowV(COG_TAB_DISPLAY, row) * COG_TEX_H;
            label.setColor(live ? Color.WHITE : 0x30FFFFFF);
            float baseline = y - (label.ascent() + label.descent()) * 0.5f;
            if (row == COG_OPTION_GAMEPAD) {
                // The controllers' own shortcut under the label, the two
                // together where the label alone would be
                baseline = y - 2.0f;
                canvas.drawText(context.getString(controllersHint(gamepadShortcut)),
                        0.06f * COG_TEX_W, y - 2.0f + hint.getTextSize(), hint);
            }
            canvas.drawText(optionRows[row], 0.06f * COG_TEX_W, baseline, label);
            if (live) {
                drawCogCells(canvas, optionCells[row], y, true, cellHalf);
            }
            else {
                drawGreyedReason(canvas, y, headAimReason);
            }
        }
        label.setColor(Color.WHITE);

        // How strong the glow is, a track under the cells and the only row on
        // this tab that is dragged rather than pressed
        float y = cogRowV(COG_TAB_DISPLAY, COG_DISPLAY_SLIDER_ROW) * COG_TEX_H;
        canvas.drawText(context.getString(R.string.vr_panel_glow_level), 0.06f * COG_TEX_W,
                y - (label.ascent() + label.descent()) * 0.5f, label);

        Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
        track.setStyle(Paint.Style.STROKE);
        track.setStrokeWidth(6.0f);
        track.setStrokeCap(Paint.Cap.ROUND);
        track.setColor(0x66FFFFFF);
        canvas.drawLine(trackL, y, trackR, y, track);
        drawCogChevrons(canvas, y, true, cellHalf);

        // Marks the default, halfway, the same way the 3D tab marks its two
        Paint tick = new Paint(Paint.ANTI_ALIAS_FLAG);
        tick.setColor(0xCCFFFFFF);
        float midX = (trackL + trackR) * 0.5f;
        canvas.drawRect(midX - 2.0f, y - cellHalf, midX + 2.0f, y + cellHalf, tick);
    }

    // Picture tab: a label and a track per value of the grade, each ticked
    // where it leaves the picture as streamed, and the reset button under
    // them. What each value is now is on the strip beside the tracks.
    private void drawCogPictureRows(Canvas canvas) {
        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextSize(22.0f);
        text.setTextAlign(Paint.Align.LEFT);
        text.setColor(Color.WHITE);

        Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
        track.setStyle(Paint.Style.STROKE);
        track.setStrokeWidth(6.0f);
        track.setStrokeCap(Paint.Cap.ROUND);
        track.setColor(0x66FFFFFF);

        Paint tick = new Paint(Paint.ANTI_ALIAS_FLAG);
        tick.setColor(0xCCFFFFFF);

        final float trackL = COG_RUN_L * COG_TEX_W;
        final float trackR = COG_RUN_R * COG_TEX_W;
        final float tickHalf = COG_CELL_HALF * COG_TEX_H;

        for (int row = 0; row < pictureRows.length; row++) {
            float y = cogRowV(COG_TAB_PICTURE, row) * COG_TEX_H;
            canvas.drawText(pictureRows[row], 0.06f * COG_TEX_W,
                    y - (text.ascent() + text.descent()) * 0.5f, text);
            canvas.drawLine(trackL, y, trackR, y, track);
            drawCogChevrons(canvas, y, true, tickHalf);

            float markX = trackL + pictureTickT(row) * (trackR - trackL);
            canvas.drawRect(markX - 2.0f, y - tickHalf, markX + 2.0f, y + tickHalf, tick);
        }

        // Back to the picture as streamed, all four at once
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(4.0f);
        RectF reset = new RectF(COG_RESET_L * COG_TEX_W, COG_RESET_T * COG_TEX_H,
                COG_RESET_R * COG_TEX_W, COG_RESET_B * COG_TEX_H);
        canvas.drawRoundRect(reset, 14.0f, 14.0f, paint);

        text.setTextAlign(Paint.Align.CENTER);
        canvas.drawText(resetWord, reset.centerX(),
                reset.centerY() - (text.ascent() + text.descent()) * 0.5f, text);
    }

    // About tab: the app's name and version, where its log is, then the button
    // that opens the Ko-fi sheet over the one that opens the report sheet,
    // both drawn the way the reset buttons are, each with a quiet line under
    // it. The ring under the ray is the native side's.
    private void drawCogAbout(Canvas canvas) {
        final float mid = COG_TEX_W * 0.5f;
        final float room = COG_TEX_W - 80.0f;
        Paint name = new Paint(Paint.ANTI_ALIAS_FLAG);
        name.setTextSize(30.0f);
        name.setTextAlign(Paint.Align.CENTER);
        name.setColor(Color.WHITE);
        canvas.drawText(COG_ABOUT_NAME, mid, 0.25f * COG_TEX_H, name);

        Paint line = new Paint(Paint.ANTI_ALIAS_FLAG);
        line.setTextSize(19.0f);
        line.setTextAlign(Paint.Align.CENTER);
        line.setColor(0xB0FFFFFF);
        String version = BuildConfig.GIT_HASH.isEmpty()
                ? context.getString(R.string.vr_panel_version_no_commit, BuildConfig.VERSION_NAME)
                : context.getString(R.string.vr_panel_version, BuildConfig.VERSION_NAME,
                        BuildConfig.GIT_HASH);
        canvas.drawText(Toast.fit(version, line, room), mid, 0.32f * COG_TEX_H, line);
        String log = FileLog.getLogPath();
        canvas.drawText(Toast.fit(log != null
                        ? context.getString(R.string.vr_panel_log_file, BugReport.shortPath(log))
                        : context.getString(R.string.vr_panel_log_file_off),
                line, room), mid, 0.38f * COG_TEX_H, line);

        // Each button's line, where the other tabs say why a row is dead
        Paint hint = new Paint(Paint.ANTI_ALIAS_FLAG);
        hint.setTextSize(17.0f);
        hint.setTextAlign(Paint.Align.CENTER);
        hint.setColor(0x80FFFFFF);

        drawAboutButton(canvas, COG_KOFI_L, COG_KOFI_T, COG_KOFI_R, COG_KOFI_B,
                context.getString(R.string.vr_kofi_title));
        canvas.drawText(Toast.fit(context.getString(R.string.vr_kofi_free), hint, room),
                mid, (COG_KOFI_B + 0.05f) * COG_TEX_H, hint);

        drawAboutButton(canvas, COG_REPORT_L, COG_REPORT_T, COG_REPORT_R, COG_REPORT_B,
                context.getString(R.string.title_bug_report));
        canvas.drawText(Toast.fit(context.getString(BugReport.collectorConfigured()
                        ? R.string.vr_panel_report_sends : R.string.vr_panel_report_saves),
                hint, room), mid, (COG_REPORT_B + 0.05f) * COG_TEX_H, hint);
    }

    // One of the About tab's buttons, plain like the reset buttons
    private static void drawAboutButton(Canvas canvas, float l, float t, float r, float b,
                                        String text) {
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(0xEEFFFFFF);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(4.0f);
        RectF button = new RectF(l * COG_TEX_W, t * COG_TEX_H, r * COG_TEX_W, b * COG_TEX_H);
        canvas.drawRoundRect(button, 14.0f, 14.0f, paint);
        Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
        label.setTextSize(24.0f);
        label.setTextAlign(Paint.Align.CENTER);
        label.setColor(Color.WHITE);
        canvas.drawText(Toast.fit(text, label, button.width() - 24.0f), button.centerX(),
                button.centerY() - (label.ascent() + label.descent()) * 0.5f, label);
    }

    /**
     * The report sheet: a title, the note and the address under their
     * labels, the line saying what goes, and Cancel and Send. Unlike the other
     * panels it is drawn again whenever what it shows changes, off the frame
     * loop, into the one bitmap and buffer, which the upload has finished with
     * before the next drawing starts. Moonlight's own dark sheet and white
     * strokes, like the exit prompt.
     */
    static final class ReportSheet {
        // Lines of the note shown at once, the last ones, so the end being
        // typed at is always in view
        private static final int NOTE_LINES = 4;
        private static final float PAD = 14.0f;

        private final Bitmap bitmap = Bitmap.createBitmap(REPORT_TEX_W, REPORT_TEX_H,
                Bitmap.Config.ARGB_8888);
        private final Canvas canvas = new Canvas(bitmap);
        private final ByteBuffer pixels = ByteBuffer.allocateDirect(REPORT_TEX_W * REPORT_TEX_H * 4);
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint title = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint field = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint small = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint button = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final String titleText;
        private final String noteLabel;
        private final String noteHint;
        private final String addressLabel;
        private final String addressHint;
        private final String addressBad;
        private final String whatGoes;
        private final String cancelText;
        private final String sendText;

        ReportSheet(Context context, boolean collector) {
            titleText = context.getString(R.string.title_bug_report);
            noteLabel = context.getString(R.string.bug_report_message_hint);
            noteHint = context.getString(R.string.vr_report_note_hint);
            addressLabel = context.getString(R.string.bug_report_email_hint);
            addressHint = context.getString(R.string.vr_report_optional);
            addressBad = context.getString(R.string.vr_report_email_bad);
            whatGoes = context.getString(collector ? R.string.vr_report_sends
                                                   : R.string.vr_report_saves);
            cancelText = context.getString(android.R.string.cancel);
            sendText = context.getString(collector ? R.string.bug_report_send_direct
                                                   : R.string.bug_report_save);
            stroke.setStyle(Paint.Style.STROKE);
            title.setTextSize(32.0f);
            title.setTextAlign(Paint.Align.CENTER);
            title.setColor(Color.WHITE);
            label.setTextSize(21.0f);
            label.setColor(0xB0FFFFFF);
            field.setTextSize(25.0f);
            small.setTextSize(18.0f);
            button.setTextSize(28.0f);
            button.setTextAlign(Paint.Align.CENTER);
        }

        /**
         * focus and hover are REPORT_ZONE_ values: the field with the keys,
         * and the part under the ray or none.
         */
        ByteBuffer draw(String note, String address, int focus, int hover, boolean canSend,
                        boolean addressWrong) {
            canvas.drawColor(0, PorterDuff.Mode.CLEAR);
            fill.setColor(0xF0141416);
            canvas.drawRoundRect(new RectF(1.0f, 1.0f, REPORT_TEX_W - 1.0f, REPORT_TEX_H - 1.0f),
                    32.0f, 32.0f, fill);

            final float left = REPORT_FIELD_L * REPORT_TEX_W;
            final float right = REPORT_FIELD_R * REPORT_TEX_W;
            canvas.drawText(Toast.fit(titleText, title, right - left), REPORT_TEX_W * 0.5f,
                    0.085f * REPORT_TEX_H, title);

            // The note: its label, the box, and its last lines
            RectF noteBox = new RectF(left, REPORT_NOTE_T * REPORT_TEX_H, right,
                    REPORT_NOTE_B * REPORT_TEX_H);
            canvas.drawText(Toast.fit(noteLabel, label, right - left), left,
                    noteBox.top - 12.0f, label);
            drawBox(noteBox, focus == REPORT_ZONE_NOTE, hover == REPORT_ZONE_NOTE);
            float lineH = (noteBox.height() - 2.0f * PAD) / NOTE_LINES;
            float textW = noteBox.width() - 2.0f * PAD;
            // An empty field's hint stands a little clear of the caret
            final float hintIn = 8.0f;
            if (note.isEmpty()) {
                field.setColor(0x60FFFFFF);
                canvas.drawText(Toast.fit(noteHint, field, textW - hintIn), noteBox.left + PAD
                        + hintIn, noteBox.top + PAD + lineH * 0.78f, field);
                if (focus == REPORT_ZONE_NOTE) {
                    drawCaret(noteBox.left + PAD, noteBox.top + PAD, lineH);
                }
            }
            else {
                List<String> lines = wrap(note, field, textW);
                int first = Math.max(0, lines.size() - NOTE_LINES);
                field.setColor(Color.WHITE);
                for (int i = first; i < lines.size(); i++) {
                    float top = noteBox.top + PAD + (i - first) * lineH;
                    canvas.drawText(lines.get(i), noteBox.left + PAD, top + lineH * 0.78f, field);
                }
                if (focus == REPORT_ZONE_NOTE) {
                    String last = lines.get(lines.size() - 1);
                    drawCaret(noteBox.left + PAD + field.measureText(last) + 2.0f,
                            noteBox.top + PAD + (lines.size() - 1 - first) * lineH, lineH);
                }
            }

            // The address on one line, its end in view when it runs long
            RectF addressBox = new RectF(left, REPORT_EMAIL_T * REPORT_TEX_H, right,
                    REPORT_EMAIL_B * REPORT_TEX_H);
            canvas.drawText(Toast.fit(addressLabel, label, right - left), left,
                    addressBox.top - 12.0f, label);
            drawBox(addressBox, focus == REPORT_ZONE_EMAIL, hover == REPORT_ZONE_EMAIL);
            float addressH = addressBox.height() - 2.0f * PAD;
            float baseline = addressBox.centerY() - (field.ascent() + field.descent()) * 0.5f;
            if (address.isEmpty()) {
                field.setColor(0x60FFFFFF);
                canvas.drawText(addressHint, addressBox.left + PAD + hintIn, baseline, field);
                if (focus == REPORT_ZONE_EMAIL) {
                    drawCaret(addressBox.left + PAD, addressBox.top + PAD, addressH);
                }
            }
            else {
                field.setColor(Color.WHITE);
                String shown = tail(address, field, textW - 8.0f);
                canvas.drawText(shown, addressBox.left + PAD, baseline, field);
                if (focus == REPORT_ZONE_EMAIL) {
                    drawCaret(addressBox.left + PAD + field.measureText(shown) + 2.0f,
                            addressBox.top + PAD, addressH);
                }
            }
            if (addressWrong) {
                small.setColor(0xFFFFB74D);
                canvas.drawText(Toast.fit(addressBad, small, right - left), left,
                        addressBox.bottom + 26.0f, small);
            }

            // What goes, and where
            small.setColor(0x99FFFFFF);
            List<String> said = wrap(whatGoes, small, right - left);
            for (int i = 0; i < said.size() && i < 3; i++) {
                canvas.drawText(said.get(i), left, 0.69f * REPORT_TEX_H + i * 23.0f, small);
            }

            drawButton(REPORT_CANCEL_L, REPORT_CANCEL_R, cancelText, true, false,
                    hover == REPORT_ZONE_CANCEL);
            drawButton(REPORT_SEND_L, REPORT_SEND_R, sendText, canSend, true,
                    hover == REPORT_ZONE_SEND);

            pixels.rewind();
            bitmap.copyPixelsToBuffer(pixels);
            pixels.rewind();
            return pixels;
        }

        // A field's box: brightest with the keys, a little brighter under the ray
        private void drawBox(RectF box, boolean focused, boolean hot) {
            fill.setColor(focused ? 0x1EFFFFFF : 0x12FFFFFF);
            canvas.drawRoundRect(box, 12.0f, 12.0f, fill);
            stroke.setStrokeWidth(focused ? 3.0f : 2.0f);
            stroke.setColor(focused ? 0xEEFFFFFF : hot ? 0x99FFFFFF : 0x50FFFFFF);
            canvas.drawRoundRect(box, 12.0f, 12.0f, stroke);
        }

        // Where the next key lands
        private void drawCaret(float x, float top, float height) {
            fill.setColor(0xEEFFFFFF);
            canvas.drawRect(x, top + height * 0.12f, x + 2.5f, top + height * 0.92f, fill);
        }

        // A button the shape the exit prompt's are. Send is the one that does
        // something, so it carries a faint fill, and greys out until it can.
        private void drawButton(float l, float r, String text, boolean live, boolean primary,
                                boolean hot) {
            RectF box = new RectF(l * REPORT_TEX_W, REPORT_BTN_T * REPORT_TEX_H,
                    r * REPORT_TEX_W, REPORT_BTN_B * REPORT_TEX_H);
            int ink = live ? 0xEEFFFFFF : 0x40FFFFFF;
            if (live && (hot || primary)) {
                fill.setColor(hot ? 0x38FFFFFF : 0x18FFFFFF);
                canvas.drawRoundRect(box, 16.0f, 16.0f, fill);
            }
            stroke.setStrokeWidth(live && hot ? 5.0f : 3.0f);
            stroke.setColor(ink);
            canvas.drawRoundRect(box, 16.0f, 16.0f, stroke);
            button.setColor(live ? Color.WHITE : 0x50FFFFFF);
            canvas.drawText(Toast.fit(text, button, box.width() - 24.0f), box.centerX(),
                    box.centerY() - (button.ascent() + button.descent()) * 0.5f, button);
        }
    }

    /**
     * Text broken into lines that fit the width: at the line breaks it has,
     * then at the last space that fits, or between characters where there is
     * none, which is how a language without spaces wraps.
     */
    static List<String> wrap(String text, Paint paint, float width) {
        List<String> lines = new ArrayList<>();
        for (String paragraph : text.split("\n", -1)) {
            String rest = paragraph;
            if (rest.isEmpty()) {
                lines.add("");
                continue;
            }
            while (!rest.isEmpty()) {
                int fits = paint.breakText(rest, true, width, null);
                if (fits >= rest.length()) {
                    lines.add(rest);
                    break;
                }
                fits = Math.max(1, fits);
                int space = rest.lastIndexOf(' ', fits);
                int cut = space > 0 ? space : fits;
                lines.add(rest.substring(0, cut));
                rest = space > 0 ? rest.substring(space + 1) : rest.substring(cut);
            }
        }
        return lines;
    }

    // The end of a line too long to show whole, after an ellipsis, so the
    // part being typed at stays in view
    static String tail(String text, Paint paint, float width) {
        if (paint.measureText(text) <= width) {
            return text;
        }
        int start = 0;
        while (start < text.length()
                && paint.measureText("…" + text.substring(start)) > width) {
            start++;
        }
        return "…" + text.substring(start);
    }

    // Where a picture row's tick sits along its run, 0 to 1: the place of its
    // default in its lane
    static float pictureTickT(int row) {
        int min, max, def;
        switch (row) {
            case PICTURE_CONTRAST:
                min = PICTURE_CONTRAST_MIN;
                max = PICTURE_CONTRAST_MAX;
                def = PICTURE_CONTRAST_DEFAULT;
                break;
            case PICTURE_GAMMA:
                min = PICTURE_GAMMA_MIN;
                max = PICTURE_GAMMA_MAX;
                def = PICTURE_GAMMA_DEFAULT;
                break;
            case PICTURE_SATURATION:
                min = PICTURE_SATURATION_MIN;
                max = PICTURE_SATURATION_MAX;
                def = PICTURE_SATURATION_DEFAULT;
                break;
            default:
                min = PICTURE_BRIGHTNESS_MIN;
                max = PICTURE_BRIGHTNESS_MAX;
                def = PICTURE_BRIGHTNESS_DEFAULT;
                break;
        }
        return (def - min) / (float)(max - min);
    }

    // The fallback cog, drawn only when the icon asset is missing. About as
    // much of a gear as reads at this size.
    private Bitmap buildCogFallback() {
        Bitmap button = Bitmap.createBitmap(BUTTON_TEX, BUTTON_TEX,
                                            Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(button);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);

        final float mid = BUTTON_TEX * 0.5f;
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
     * it, and the layout the native side hit tests against. All the sheets
     * share one set of key rectangles, so the art and the hit test are built
     * from the same numbers and cannot drift apart. Nothing is held when it
     * opens, so every sheet starts with no modifier lit.
     */
    Keyboard buildKeyboard() {
        float[] keyRects = buildKeyRects();
        int[][] codes = new int[KB_STATE_COUNT][];
        ByteBuffer[] sheets = new ByteBuffer[KB_STATE_COUNT];
        for (int state = 0; state < KB_STATE_COUNT; state++) {
            codes[state] = flatten(KB_CODES[state]);
            sheets[state] = buildKeyboardSheet(state, 0);
        }

        Bitmap button = buildKeyboardButton();
        ByteBuffer buttonPixels = toBuffer(button);
        button.recycle();

        return new Keyboard(sheets, buttonPixels, keyRects, codes);
    }

    /**
     * One keyboard sheet with the modifiers in mods, KB_MOD_ bits, drawn lit:
     * the keys themselves on the Fn sheet, and a tag naming them on the
     * space bar of the others, since they hold over every sheet.
     */
    ByteBuffer buildKeyboardSheet(int state, int mods) {
        Bitmap sheet = buildKeyboardSheet(kbLabels[state], KB_CODES[state], buildKeyRects(), mods);
        ByteBuffer pixels = toBuffer(sheet);
        sheet.recycle();
        return pixels;
    }

    // The held modifiers by name, "Ctrl Alt", or null with none
    private String heldModifiers(int mods) {
        StringBuilder names = new StringBuilder();
        for (int i = 0; i < KB_MOD_BITS.length; i++) {
            if ((mods & KB_MOD_BITS[i]) != 0) {
                if (names.length() > 0) {
                    names.append(' ');
                }
                names.append(kbModNames[i]);
            }
        }
        return names.length() > 0 ? names.toString() : null;
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
    // the settings use. A lit modifier is a white cap with dark letters, the
    // way a pressed key reads.
    private Bitmap buildKeyboardSheet(String[][] labels, int[][] codes, float[] rects, int mods) {
        Bitmap bitmap = Bitmap.createBitmap(KB_TEX_W, KB_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);

        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        paint.setColor(0xF0141416);
        canvas.drawRoundRect(new RectF(1.0f, 1.0f, KB_TEX_W - 1.0f, KB_TEX_H - 1.0f),
                32.0f, 32.0f, paint);

        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextAlign(Paint.Align.CENTER);

        // Whether this sheet has the modifier keys on it, which then show
        // what is held themselves
        boolean modifierKeys = false;
        for (int[] row : codes) {
            for (int code : row) {
                modifierKeys |= modifierBit(code) != 0;
            }
        }
        String held = modifierKeys ? null : heldModifiers(mods);

        int at = 0;
        for (int r = 0; r < labels.length; r++) {
            for (int k = 0; k < labels[r].length; k++) {
                String label = labels[r][k];
                RectF box = new RectF(rects[at] * KB_TEX_W, rects[at + 1] * KB_TEX_H,
                        rects[at + 2] * KB_TEX_W, rects[at + 3] * KB_TEX_H);
                at += 4;
                if (label == null) {
                    continue;
                }
                int code = codes[r][k];
                boolean lit = (mods & modifierBit(code)) != 0;

                paint.setStyle(Paint.Style.FILL);
                paint.setColor(lit ? 0xEEFFFFFF : 0x28FFFFFF);
                canvas.drawRoundRect(box, 10.0f, 10.0f, paint);
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(2.0f);
                paint.setColor(lit ? 0xFFFFFFFF : 0x50FFFFFF);
                canvas.drawRoundRect(box, 10.0f, 10.0f, paint);

                // A single character is what the key types, so it gets the
                // room. The named keys are wordier and have to fit.
                text.setColor(lit ? 0xFF141416 : Color.WHITE);
                text.setTextSize(label.length() == 1 ? 34.0f : 22.0f);
                canvas.drawText(label, box.centerX(),
                        box.centerY() - (text.ascent() + text.descent()) * 0.5f, text);

                if (held != null && code == 32) {
                    drawHeldTag(canvas, box, held);
                }
            }
        }

        return bitmap;
    }

    // The modifiers still held, lit at the left end of the space bar
    private static void drawHeldTag(Canvas canvas, RectF space, String held) {
        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setTextSize(20.0f);
        text.setColor(0xFF141416);
        float pad = 10.0f;
        float w = text.measureText(held) + 2.0f * pad;
        float h = space.height() - 16.0f;
        RectF tag = new RectF(space.left + 8.0f, space.top + 8.0f, space.left + 8.0f + w,
                space.top + 8.0f + h);
        Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        fill.setColor(0xEEFFFFFF);
        canvas.drawRoundRect(tag, 8.0f, 8.0f, fill);
        canvas.drawText(held, tag.left + pad,
                tag.centerY() - (text.ascent() + text.descent()) * 0.5f, text);
    }

    // The KB_MOD_ bit a modifier key's code lights, or 0
    private static int modifierBit(int code) {
        switch (code) {
            case KB_CODE_CTRL:
                return KB_MOD_CTRL;
            case KB_CODE_ALT:
                return KB_MOD_ALT;
            case KB_CODE_WIN:
                return KB_MOD_WIN;
            default:
                return 0;
        }
    }

    // A keyboard outline with a few keys in it, which is about as much as reads
    // at this size
    private Bitmap buildKeyboardButton() {
        Bitmap button = Bitmap.createBitmap(BUTTON_TEX, BUTTON_TEX,
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
     * The 3D switch on the bar, off and then on, a swapchain each on the
     * native side so flipping it is a handle rather than an upload. The
     * letters sit in the frame the environment button is drawn in, bright
     * while the picture is in 3D and dimmed with a stroke through them while
     * it is flat.
     */
    ByteBuffer[] buildStereoButtons() {
        ByteBuffer[] faces = new ByteBuffer[2];
        for (int on = 0; on < 2; on++) {
            Bitmap button = buildStereoButton(on == 1);
            faces[on] = toBuffer(button);
            button.recycle();
        }
        return faces;
    }

    private Bitmap buildStereoButton(boolean on) {
        Bitmap button = Bitmap.createBitmap(BUTTON_TEX, BUTTON_TEX,
                                            Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(button);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        int ink = on ? 0xEEFFFFFF : 0x80FFFFFF;

        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(ink);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(6.0f);
        canvas.drawRoundRect(new RectF(14.0f, 14.0f, 114.0f, 114.0f), 22.0f, 22.0f, paint);

        Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
        text.setColor(ink);
        text.setTextSize(48.0f);
        text.setTypeface(Typeface.DEFAULT_BOLD);
        text.setTextAlign(Paint.Align.CENTER);
        final float mid = BUTTON_TEX * 0.5f;
        canvas.drawText("3D", mid, mid - (text.ascent() + text.descent()) * 0.5f, text);

        if (!on) {
            // Corner to corner through the letters, the way a muted speaker
            // is struck through, and at full strength so it reads at a glance
            paint.setColor(0xEEFFFFFF);
            paint.setStrokeWidth(7.0f);
            paint.setStrokeCap(Paint.Cap.ROUND);
            canvas.drawLine(30.0f, 98.0f, 98.0f, 30.0f, paint);
        }
        return button;
    }

    /**
     * The ray's switch on the bar, off and then on, a swapchain each on the
     * native side like the 3D switch's. A beam leaving a hand for a dot, in
     * the frame the other buttons are drawn in, bright while the ray is drawn
     * and dimmed with a stroke through it while it is hidden.
     */
    ByteBuffer[] buildRayButtons() {
        ByteBuffer[] faces = new ByteBuffer[2];
        for (int on = 0; on < 2; on++) {
            Bitmap button = buildRayButton(on == 1);
            faces[on] = toBuffer(button);
            button.recycle();
        }
        return faces;
    }

    private Bitmap buildRayButton(boolean on) {
        Bitmap button = Bitmap.createBitmap(BUTTON_TEX, BUTTON_TEX,
                                            Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(button);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        int ink = on ? 0xEEFFFFFF : 0x80FFFFFF;

        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(ink);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(6.0f);
        canvas.drawRoundRect(new RectF(14.0f, 14.0f, 114.0f, 114.0f), 22.0f, 22.0f, paint);

        // The beam from the bottom left toward the dot at the top right,
        // fading as it goes the way the real one does at its ends
        paint.setStrokeCap(Paint.Cap.ROUND);
        paint.setStrokeWidth(7.0f);
        canvas.drawLine(34.0f, 94.0f, 70.0f, 58.0f, paint);
        paint.setStyle(Paint.Style.FILL);
        canvas.drawCircle(84.0f, 44.0f, 11.0f, paint);

        if (!on) {
            // Struck through like the 3D switch, at full strength so it reads
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(0xEEFFFFFF);
            paint.setStrokeWidth(7.0f);
            canvas.drawLine(30.0f, 30.0f, 98.0f, 98.0f, paint);
        }
        return button;
    }

    /**
     * Gamepad mode's switch on the bar, pointer and then gamepad, a swapchain
     * each on the native side like the ray's. A pad in the frame the other
     * buttons are drawn in, bright while the controllers are the pad and
     * dimmed with a stroke through it while they are the pointer.
     */
    ByteBuffer[] buildPadButtons() {
        ByteBuffer[] faces = new ByteBuffer[2];
        for (int on = 0; on < 2; on++) {
            Bitmap button = buildPadButton(on == 1);
            faces[on] = toBuffer(button);
            button.recycle();
        }
        return faces;
    }

    private Bitmap buildPadButton(boolean on) {
        Bitmap button = Bitmap.createBitmap(BUTTON_TEX, BUTTON_TEX,
                                            Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(button);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        int ink = on ? 0xEEFFFFFF : 0x80FFFFFF;

        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(ink);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(6.0f);
        canvas.drawRoundRect(new RectF(14.0f, 14.0f, 114.0f, 114.0f), 22.0f, 22.0f, paint);

        // A pad: a wide rounded body with a cross on the left and two
        // buttons on the right
        paint.setStrokeWidth(5.0f);
        canvas.drawRoundRect(new RectF(28.0f, 46.0f, 100.0f, 84.0f), 18.0f, 18.0f, paint);
        paint.setStrokeCap(Paint.Cap.ROUND);
        canvas.drawLine(38.0f, 65.0f, 54.0f, 65.0f, paint);
        canvas.drawLine(46.0f, 57.0f, 46.0f, 73.0f, paint);
        paint.setStyle(Paint.Style.FILL);
        canvas.drawCircle(76.0f, 69.0f, 4.5f, paint);
        canvas.drawCircle(86.0f, 60.0f, 4.5f, paint);

        if (!on) {
            // Struck through like the ray's and the 3D switch
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(0xEEFFFFFF);
            paint.setStrokeWidth(7.0f);
            canvas.drawLine(30.0f, 30.0f, 98.0f, 98.0f, paint);
        }
        return button;
    }

    /**
     * Head aim's switch on the bar, off and then on, a swapchain each on the
     * native side like the ray's. A crosshair in the frame the other buttons
     * are drawn in, bright while head aim is on and dimmed with a stroke
     * through it while it is off.
     */
    ByteBuffer[] buildAimButtons() {
        ByteBuffer[] faces = new ByteBuffer[2];
        for (int on = 0; on < 2; on++) {
            Bitmap button = buildAimButton(on == 1);
            faces[on] = toBuffer(button);
            button.recycle();
        }
        return faces;
    }

    private Bitmap buildAimButton(boolean on) {
        Bitmap button = Bitmap.createBitmap(BUTTON_TEX, BUTTON_TEX,
                                            Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(button);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        int ink = on ? 0xEEFFFFFF : 0x80FFFFFF;

        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(ink);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(6.0f);
        canvas.drawRoundRect(new RectF(14.0f, 14.0f, 114.0f, 114.0f), 22.0f, 22.0f, paint);

        // A sight: a ring with a tick out from each side and a dot in it
        final float mid = BUTTON_TEX * 0.5f;
        canvas.drawCircle(mid, mid, 18.0f, paint);
        paint.setStrokeCap(Paint.Cap.ROUND);
        canvas.drawLine(mid, mid - 38.0f, mid, mid - 25.0f, paint);
        canvas.drawLine(mid, mid + 25.0f, mid, mid + 38.0f, paint);
        canvas.drawLine(mid - 38.0f, mid, mid - 25.0f, mid, paint);
        canvas.drawLine(mid + 25.0f, mid, mid + 38.0f, mid, paint);
        paint.setStyle(Paint.Style.FILL);
        canvas.drawCircle(mid, mid, 5.0f, paint);

        if (!on) {
            // Struck through like the ray's and the 3D switch
            paint.setStyle(Paint.Style.STROKE);
            paint.setColor(0xEEFFFFFF);
            paint.setStrokeWidth(7.0f);
            canvas.drawLine(30.0f, 30.0f, 98.0f, 98.0f, paint);
        }
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
        Bitmap button = Bitmap.createBitmap(BUTTON_TEX, BUTTON_TEX,
                                            Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(button);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);

        final float mid = BUTTON_TEX * 0.5f;
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
        canvas.drawText(exitQuestion, EXIT_TEX_W * 0.5f,
                questionY - (text.ascent() + text.descent()) * 0.5f, text);

        // Leaving is the destructive half, so it is the one that reads red.
        // Both are the same shape, so neither is the easier target.
        drawExitChoice(canvas, paint, text, EXIT_EXIT_L, EXIT_EXIT_R,
                context.getString(R.string.vr_exit), 0xFFE05A5A, hot == EXIT_ZONE_EXIT);
        drawExitChoice(canvas, paint, text, EXIT_CANCEL_L, EXIT_CANCEL_R,
                context.getString(android.R.string.cancel), 0xEEFFFFFF, hot == EXIT_ZONE_CANCEL);

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

    /**
     * The hand lock hint: what the triple pinch does, over OK and "Don't show
     * this again", on the exit prompt's dark sheet with its white strokes.
     * Drawn once, since the ring over the button under the ray is a quad of
     * the native side's.
     */
    ByteBuffer buildHandHint() {
        Bitmap bitmap = Bitmap.createBitmap(HINT_TEX_W, HINT_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(0xF0141416);
        canvas.drawRoundRect(new RectF(1.0f, 1.0f, HINT_TEX_W - 1.0f, HINT_TEX_H - 1.0f),
                32.0f, 32.0f, paint);

        final float left = HINT_OK_L * HINT_TEX_W;
        final float width = (HINT_NEVER_R - HINT_OK_L) * HINT_TEX_W;
        Paint title = new Paint(Paint.ANTI_ALIAS_FLAG);
        title.setColor(Color.WHITE);
        title.setTextSize(40.0f);
        canvas.drawText(Toast.fit(context.getString(R.string.vr_hand_hint_title), title, width),
                left, 0.17f * HINT_TEX_H, title);

        // As many lines as the words take in the space over the buttons,
        // smaller if a language needs more of them
        Paint body = new Paint(Paint.ANTI_ALIAS_FLAG);
        body.setColor(0xCCFFFFFF);
        String said = context.getString(R.string.vr_hand_hint_body);
        final float top = 0.27f * HINT_TEX_H;
        final float room = (HINT_BTN_T - 0.06f) * HINT_TEX_H - top;
        float size = 30.0f;
        List<String> lines;
        while (true) {
            body.setTextSize(size);
            lines = wrap(said, body, width);
            if (lines.size() * size * 1.3f <= room || size <= 20.0f) {
                break;
            }
            size -= 2.0f;
        }
        for (int i = 0; i < lines.size(); i++) {
            canvas.drawText(lines.get(i), left, top + size + i * size * 1.3f, body);
        }

        Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
        label.setTextAlign(Paint.Align.CENTER);
        drawHintButton(canvas, paint, label, HINT_OK_L, HINT_OK_R,
                context.getString(android.R.string.ok));
        drawHintButton(canvas, paint, label, HINT_NEVER_L, HINT_NEVER_R,
                context.getString(R.string.vr_hand_hint_never));

        ByteBuffer pixels = toBuffer(bitmap);
        bitmap.recycle();
        return pixels;
    }

    // One of the hint's buttons, the shape the exit prompt's are
    private static void drawHintButton(Canvas canvas, Paint paint, Paint label, float l, float r,
                                       String text) {
        RectF box = new RectF(l * HINT_TEX_W, HINT_BTN_T * HINT_TEX_H, r * HINT_TEX_W,
                HINT_BTN_B * HINT_TEX_H);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(3.0f);
        paint.setColor(0xEEFFFFFF);
        canvas.drawRoundRect(box, 16.0f, 16.0f, paint);
        paint.setStyle(Paint.Style.FILL);
        label.setColor(0xEEFFFFFF);
        label.setTextSize(30.0f);
        canvas.drawText(Toast.fit(text, label, box.width() - 24.0f), box.centerX(),
                box.centerY() - (label.ascent() + label.descent()) * 0.5f, label);
    }

    /**
     * The Ko-fi sheet the About tab opens: the title, the page's QR code with
     * its address and a line beside it, and one Close button, on the exit
     * prompt's dark sheet with its white strokes. Drawn once, since the ring
     * over the button under the ray is a quad of the native side's. The code
     * goes on a pixel for a pixel, unfiltered, on white even if it failed to
     * load, so every module stays sharp for a phone to read.
     */
    ByteBuffer buildKofiSheet(String url) {
        Bitmap bitmap = Bitmap.createBitmap(KOFI_TEX_W, KOFI_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(0xF0141416);
        canvas.drawRoundRect(new RectF(1.0f, 1.0f, KOFI_TEX_W - 1.0f, KOFI_TEX_H - 1.0f),
                32.0f, 32.0f, paint);

        final int qrLeft = Math.round(KOFI_QR_L * KOFI_TEX_W);
        final int qrTop = Math.round(KOFI_QR_T * KOFI_TEX_H);
        final float right = KOFI_CLOSE_R * KOFI_TEX_W;
        Paint title = new Paint(Paint.ANTI_ALIAS_FLAG);
        title.setColor(Color.WHITE);
        title.setTextSize(40.0f);
        canvas.drawText(Toast.fit(context.getString(R.string.vr_kofi_title), title,
                right - qrLeft), qrLeft, 0.14f * KOFI_TEX_H, title);

        Rect code = new Rect(qrLeft, qrTop, qrLeft + KOFI_QR_PX, qrTop + KOFI_QR_PX);
        Paint white = new Paint();
        white.setColor(Color.WHITE);
        canvas.drawRect(code, white);
        Bitmap qr = loadIcon(KOFI_QR, KOFI_QR_PX);
        if (qr != null) {
            canvas.drawBitmap(qr, null, code, new Paint());
            qr.recycle();
        }

        // Beside the code: the address, then what to do with it and the
        // quiet line, smaller if a language needs more lines than fit
        final float column = code.right + 48.0f;
        final float width = right - column;
        Paint address = new Paint(Paint.ANTI_ALIAS_FLAG);
        address.setColor(Color.WHITE);
        address.setTextSize(30.0f);
        // No ligatures in something to be typed: the font joins the f and i
        address.setFontFeatureSettings("'liga' 0");
        canvas.drawText(Toast.fit(url, address, width), column, qrTop + 36.0f, address);

        Paint body = new Paint(Paint.ANTI_ALIAS_FLAG);
        String said = context.getString(R.string.vr_kofi_scan) + "\n"
                + context.getString(R.string.vr_kofi_free);
        final float top = qrTop + 72.0f;
        final float room = (KOFI_BTN_T - 0.04f) * KOFI_TEX_H - top;
        float size = 28.0f;
        List<String> lines;
        while (true) {
            body.setTextSize(size);
            lines = wrap(said, body, width);
            if (lines.size() * size * 1.3f <= room || size <= 20.0f) {
                break;
            }
            size -= 2.0f;
        }
        int scanLines = wrap(context.getString(R.string.vr_kofi_scan), body, width).size();
        for (int i = 0; i < lines.size(); i++) {
            body.setColor(i < scanLines ? 0xDDFFFFFF : 0x99FFFFFF);
            canvas.drawText(lines.get(i), column, top + size + i * size * 1.3f, body);
        }

        RectF box = new RectF(KOFI_CLOSE_L * KOFI_TEX_W, KOFI_BTN_T * KOFI_TEX_H,
                KOFI_CLOSE_R * KOFI_TEX_W, KOFI_BTN_B * KOFI_TEX_H);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(3.0f);
        paint.setColor(0xEEFFFFFF);
        canvas.drawRoundRect(box, 16.0f, 16.0f, paint);
        Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
        label.setTextAlign(Paint.Align.CENTER);
        label.setColor(0xEEFFFFFF);
        label.setTextSize(30.0f);
        canvas.drawText(Toast.fit(context.getString(R.string.vr_kofi_close), label,
                box.width() - 24.0f), box.centerX(),
                box.centerY() - (label.ascent() + label.descent()) * 0.5f, label);

        ByteBuffer pixels = toBuffer(bitmap);
        bitmap.recycle();
        return pixels;
    }

    /**
     * The launch splash: the app's name over the loading word, once for each
     * number of dots and one under the other, so the native side steps the
     * dots by showing another row. The words sit on nothing: the black behind
     * them is the quad that blacks out the view, cut from the strip of black
     * under the rows, so the two fade together without a box showing.
     */
    ByteBuffer buildSplash() {
        Bitmap bitmap = Bitmap.createBitmap(SPLASH_TEX_W, SPLASH_TEX_H, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        canvas.drawColor(0, PorterDuff.Mode.CLEAR);
        Paint black = new Paint();
        black.setColor(Color.BLACK);
        canvas.drawRect(0.0f, SPLASH_TEX_H - SPLASH_BLACK_PX, SPLASH_TEX_W, SPLASH_TEX_H, black);

        Paint name = new Paint(Paint.ANTI_ALIAS_FLAG);
        name.setColor(Color.WHITE);
        name.setTextSize(64.0f);
        name.setTextAlign(Paint.Align.CENTER);

        // Dimmer than the name, and set from where the whole word with all its
        // dots would start, so it does not shuffle along as they come and go
        Paint word = new Paint(Paint.ANTI_ALIAS_FLAG);
        word.setColor(0x99FFFFFF);
        word.setTextSize(30.0f);
        float wordLeft = (SPLASH_TEX_W - word.measureText(splashLoading + "...")) * 0.5f;

        for (int row = 0; row < SPLASH_ROWS; row++) {
            float top = row * SPLASH_ROW_H;
            canvas.drawText(SPLASH_NAME, SPLASH_TEX_W * 0.5f, top + SPLASH_ROW_H * 0.48f, name);
            StringBuilder dots = new StringBuilder(splashLoading);
            for (int dot = 0; dot <= row; dot++) {
                dots.append('.');
            }
            canvas.drawText(dots.toString(), wordLeft, top + SPLASH_ROW_H * 0.74f, word);
        }

        ByteBuffer pixels = toBuffer(bitmap);
        bitmap.recycle();
        return pixels;
    }

    static ByteBuffer toBuffer(Bitmap bitmap) {
        ByteBuffer pixels = ByteBuffer.allocateDirect(
                bitmap.getWidth() * bitmap.getHeight() * 4);
        bitmap.copyPixelsToBuffer(pixels);
        pixels.rewind();
        return pixels;
    }

    // A room's own tile picture, already cropped to the cell, so it goes in
    // whole
    private Bitmap decodeRoomThumb(String path) {
        InputStream in = null;
        try {
            in = context.getAssets().open(path);
            return BitmapFactory.decodeStream(in);
        } catch (IOException | OutOfMemoryError e) {
            LimeLog.warning("Thumbnail " + path + " failed: " + e);
            return null;
        } finally {
            closeQuietly(in);
        }
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

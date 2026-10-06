package com.limelight.binding.video;

import static com.limelight.binding.video.XrShared.*;

/**
 * The 3D tab's three presets, each a separation on its depth track, in the
 * tenths of a percent the preference stores: Balanced is the running model's
 * default, Comfort COG_PRESET_STEPS steps of the track under it and Strong as
 * many over, kept on the track. The native side is handed the three values and
 * marks whichever one the separation is on.
 */
public final class DepthPresets {

    public static final int NONE = -1;
    public static final int COMFORT = COG_PRESET_COMFORT;
    public static final int BALANCED = COG_PRESET_BALANCED;
    public static final int STRONG = COG_PRESET_STRONG;

    // In cell order, as the logs and reports name them. The panel's own
    // labels are strings, in the language the app is in.
    private static final String[] NAMES = { "Comfort", "Balanced", "Strong" };

    private DepthPresets() {
    }

    /** The separation a preset writes, for a model that starts on defaultSeparation. */
    public static int value(int preset, int defaultSeparation) {
        int offset = preset == COMFORT ? -COG_PRESET_STEPS
                : preset == STRONG ? COG_PRESET_STEPS : 0;
        return Math.max(0, Math.min(COG_SEP_STEPS, defaultSeparation + offset));
    }

    /** All three, in cell order. */
    public static int[] values(int defaultSeparation) {
        int[] values = new int[COG_PRESET_CELLS];
        for (int preset = 0; preset < COG_PRESET_CELLS; preset++) {
            values[preset] = value(preset, defaultSeparation);
        }
        return values;
    }

    /**
     * Which preset a separation is, or NONE when it is somewhere else on the
     * track. Balanced first, so a default so near an end that another preset
     * is clamped onto it still reads as Balanced.
     */
    public static int presetFor(int separation, int defaultSeparation) {
        for (int preset : new int[] { BALANCED, COMFORT, STRONG }) {
            if (value(preset, defaultSeparation) == separation) {
                return preset;
            }
        }
        return NONE;
    }

    /** A preset's name, or "none" for NONE, as the logs give it. */
    public static String name(int preset) {
        return preset >= 0 && preset < NAMES.length ? NAMES[preset] : "none";
    }
}

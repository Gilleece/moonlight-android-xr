package com.limelight.binding.video;

import com.limelight.preferences.PreferenceConfiguration;

import static com.limelight.binding.video.XrShared.*;

/**
 * Where the environment picker's cells and the saved environment ids meet.
 * A cell is only where something sits in the grid today; an id names an
 * environment for good, so rearranging the grid never scrambles what anyone
 * picked. Everything else in the renderer works in cells, and only the
 * preference speaks ids.
 */
final class EnvironmentIds {

    // The layout as it shipped before the bands, kept only to read an old saved
    // cell as the environment it meant at the time
    private static final int[] LEGACY_CELL_IDS = {
            PreferenceConfiguration.VR_ENV_PASSTHROUGH,
            PreferenceConfiguration.VR_ENV_VOID,
            PreferenceConfiguration.VR_ENV_FIRST_PHOTO,
            PreferenceConfiguration.VR_ENV_FIRST_PHOTO + 1,
            PreferenceConfiguration.VR_ENV_FIRST_PHOTO + 2,
            PreferenceConfiguration.VR_ENV_FIRST_PHOTO + 3,
            PreferenceConfiguration.VR_ENV_MINIMAL_ROOM,
            PreferenceConfiguration.VR_ENV_PSX_CINEMA,
    };

    private EnvironmentIds() {
    }

    // A cell that is a fully 3d room rather than a photo or a plain background
    static boolean isRoomCell(int cell) {
        return cell == ENV_CELL_MINIMAL_ROOM || cell == ENV_CELL_PSX_CINEMA
                || cell == ENV_CELL_HOME_THEATER;
    }

    static int idForCell(int cell) {
        if (cell >= ENV_CELL_FIRST_PHOTO
                && cell < ENV_CELL_FIRST_PHOTO + XrPanels.MAX_PHOTOS) {
            return PreferenceConfiguration.VR_ENV_FIRST_PHOTO + (cell - ENV_CELL_FIRST_PHOTO);
        }
        switch (cell) {
            case ENV_CELL_PASSTHROUGH: return PreferenceConfiguration.VR_ENV_PASSTHROUGH;
            case ENV_CELL_VOID: return PreferenceConfiguration.VR_ENV_VOID;
            case ENV_CELL_MINIMAL_ROOM: return PreferenceConfiguration.VR_ENV_MINIMAL_ROOM;
            case ENV_CELL_PSX_CINEMA: return PreferenceConfiguration.VR_ENV_PSX_CINEMA;
            case ENV_CELL_HOME_THEATER: return PreferenceConfiguration.VR_ENV_HOME_THEATER;
            default: return -1;
        }
    }

    static int cellForId(int id) {
        if (id >= PreferenceConfiguration.VR_ENV_FIRST_PHOTO
                && id < PreferenceConfiguration.VR_ENV_FIRST_PHOTO + XrPanels.MAX_PHOTOS) {
            return ENV_CELL_FIRST_PHOTO + (id - PreferenceConfiguration.VR_ENV_FIRST_PHOTO);
        }
        switch (id) {
            case PreferenceConfiguration.VR_ENV_PASSTHROUGH: return ENV_CELL_PASSTHROUGH;
            case PreferenceConfiguration.VR_ENV_VOID: return ENV_CELL_VOID;
            case PreferenceConfiguration.VR_ENV_MINIMAL_ROOM: return ENV_CELL_MINIMAL_ROOM;
            case PreferenceConfiguration.VR_ENV_PSX_CINEMA: return ENV_CELL_PSX_CINEMA;
            case PreferenceConfiguration.VR_ENV_HOME_THEATER: return ENV_CELL_HOME_THEATER;
            default: return -1;
        }
    }

    // The id an old install's saved cell meant, or -1 for a cell that layout
    // never had
    static int idForLegacyCell(int legacy) {
        return legacy >= 0 && legacy < LEGACY_CELL_IDS.length ? LEGACY_CELL_IDS[legacy] : -1;
    }
}

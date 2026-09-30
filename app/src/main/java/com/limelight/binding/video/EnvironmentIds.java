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

    // Ids that named environments since removed: the generated minimal room,
    // PSX Cinema, and the 360 photos from 100 up. Never handed out again, so a
    // saved one only ever means something that is gone.
    static final int RETIRED_MINIMAL_ROOM = 2;
    static final int RETIRED_PSX_CINEMA = 3;
    static final int RETIRED_FIRST_PHOTO = 100;

    // The layout as it shipped before the bands, kept only to read an old saved
    // cell as the environment it meant at the time
    private static final int[] LEGACY_CELL_IDS = {
            PreferenceConfiguration.VR_ENV_PASSTHROUGH,
            PreferenceConfiguration.VR_ENV_VOID,
            RETIRED_FIRST_PHOTO,
            RETIRED_FIRST_PHOTO + 1,
            RETIRED_FIRST_PHOTO + 2,
            RETIRED_FIRST_PHOTO + 3,
            RETIRED_MINIMAL_ROOM,
            RETIRED_PSX_CINEMA,
    };

    private EnvironmentIds() {
    }

    // A cell that is a fully 3d room rather than a plain background
    static boolean isRoomCell(int cell) {
        return cell == ENV_CELL_HOME_THEATER || cell == ENV_CELL_GRAND_CINEMA
                || cell == ENV_CELL_SYNTHWAVE;
    }

    static int idForCell(int cell) {
        switch (cell) {
            case ENV_CELL_PASSTHROUGH: return PreferenceConfiguration.VR_ENV_PASSTHROUGH;
            case ENV_CELL_VOID: return PreferenceConfiguration.VR_ENV_VOID;
            case ENV_CELL_HOME_THEATER: return PreferenceConfiguration.VR_ENV_HOME_THEATER;
            case ENV_CELL_GRAND_CINEMA: return PreferenceConfiguration.VR_ENV_GRAND_CINEMA;
            case ENV_CELL_SYNTHWAVE: return PreferenceConfiguration.VR_ENV_SYNTHWAVE;
            default: return -1;
        }
    }

    static int cellForId(int id) {
        switch (id) {
            case PreferenceConfiguration.VR_ENV_PASSTHROUGH: return ENV_CELL_PASSTHROUGH;
            case PreferenceConfiguration.VR_ENV_VOID: return ENV_CELL_VOID;
            case PreferenceConfiguration.VR_ENV_HOME_THEATER: return ENV_CELL_HOME_THEATER;
            case PreferenceConfiguration.VR_ENV_GRAND_CINEMA: return ENV_CELL_GRAND_CINEMA;
            case PreferenceConfiguration.VR_ENV_SYNTHWAVE: return ENV_CELL_SYNTHWAVE;
            default: return -1;
        }
    }

    // The cell a session starts on. An install that never picked one has the
    // passthrough checkbox decide, and one whose saved id no longer names
    // anything, a photo or a room since removed, comes up in the void.
    static int startCell(int id, boolean passthrough) {
        if (id < 0) {
            return passthrough ? ENV_CELL_PASSTHROUGH : ENV_CELL_VOID;
        }
        int cell = cellForId(id);
        return cell >= 0 ? cell : ENV_CELL_VOID;
    }

    // The id an old install's saved cell meant, or -1 for a cell that layout
    // never had
    static int idForLegacyCell(int legacy) {
        return legacy >= 0 && legacy < LEGACY_CELL_IDS.length ? LEGACY_CELL_IDS[legacy] : -1;
    }
}

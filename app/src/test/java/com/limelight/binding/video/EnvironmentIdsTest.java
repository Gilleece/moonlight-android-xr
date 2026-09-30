package com.limelight.binding.video;

import com.limelight.preferences.PreferenceConfiguration;

import org.junit.Test;

import static com.limelight.binding.video.XrShared.*;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

public class EnvironmentIdsTest {

    @Test
    public void savedIdsKeepTheirMeaning() {
        // Written by installs that shipped before the home theater, so these
        // must never move
        assertEquals(ENV_CELL_PASSTHROUGH, EnvironmentIds.cellForId(0));
        assertEquals(ENV_CELL_VOID, EnvironmentIds.cellForId(1));
        assertEquals(ENV_CELL_MINIMAL_ROOM, EnvironmentIds.cellForId(2));
        assertEquals(ENV_CELL_PSX_CINEMA, EnvironmentIds.cellForId(3));
        assertEquals(ENV_CELL_FIRST_PHOTO, EnvironmentIds.cellForId(100));
        assertEquals(ENV_CELL_FIRST_PHOTO + 3, EnvironmentIds.cellForId(103));
    }

    @Test
    public void homeTheaterTakesTheNextFreeId() {
        assertEquals(4, PreferenceConfiguration.VR_ENV_HOME_THEATER);
        assertEquals(ENV_CELL_HOME_THEATER, EnvironmentIds.cellForId(4));
        assertEquals(4, EnvironmentIds.idForCell(ENV_CELL_HOME_THEATER));
        assertTrue(EnvironmentIds.isRoomCell(ENV_CELL_HOME_THEATER));
    }

    @Test
    public void everyCellRoundTrips() {
        for (int cell = 0; cell < PICKER_CELLS; cell++) {
            int id = EnvironmentIds.idForCell(cell);
            assertTrue("cell " + cell, id >= 0);
            assertEquals("cell " + cell, cell, EnvironmentIds.cellForId(id));
        }
    }

    @Test
    public void unknownIdsAndCellsMapToNothing() {
        assertEquals(-1, EnvironmentIds.cellForId(-1));
        assertEquals(-1, EnvironmentIds.cellForId(5));
        assertEquals(-1, EnvironmentIds.cellForId(99));
        assertEquals(-1, EnvironmentIds.cellForId(100 + XrPanels.MAX_PHOTOS));
        assertEquals(-1, EnvironmentIds.idForCell(-1));
        assertEquals(-1, EnvironmentIds.idForCell(PICKER_CELLS));
    }

    @Test
    public void legacyCellsReadAsWhatTheyMeantThen() {
        // The layout before the bands: passthrough, void, four photos, then
        // the two rooms
        int[] expectedCells = {
                ENV_CELL_PASSTHROUGH, ENV_CELL_VOID,
                ENV_CELL_FIRST_PHOTO, ENV_CELL_FIRST_PHOTO + 1,
                ENV_CELL_FIRST_PHOTO + 2, ENV_CELL_FIRST_PHOTO + 3,
                ENV_CELL_MINIMAL_ROOM, ENV_CELL_PSX_CINEMA,
        };
        for (int legacy = 0; legacy < expectedCells.length; legacy++) {
            assertEquals("legacy " + legacy, expectedCells[legacy],
                    EnvironmentIds.cellForId(EnvironmentIds.idForLegacyCell(legacy)));
        }
        assertEquals(-1, EnvironmentIds.idForLegacyCell(-1));
        assertEquals(-1, EnvironmentIds.idForLegacyCell(expectedCells.length));
    }

    @Test
    public void onlyTheRoomsAreRooms() {
        assertFalse(EnvironmentIds.isRoomCell(ENV_CELL_PASSTHROUGH));
        assertFalse(EnvironmentIds.isRoomCell(ENV_CELL_VOID));
        assertTrue(EnvironmentIds.isRoomCell(ENV_CELL_MINIMAL_ROOM));
        assertTrue(EnvironmentIds.isRoomCell(ENV_CELL_PSX_CINEMA));
        assertTrue(EnvironmentIds.isRoomCell(ENV_CELL_HOME_THEATER));
        assertFalse(EnvironmentIds.isRoomCell(ENV_CELL_FIRST_PHOTO));
    }

    @Test
    public void theGridFitsItsCells() {
        // Square cells, the fixed cells all in the first band, and a band of
        // photos under them
        assertEquals(PICKER_CELL_PX * PICKER_COLS, PICKER_TEX_W);
        assertTrue(ENV_CELL_FIRST_PHOTO <= PICKER_COLS);
        assertEquals(PICKER_CELLS - ENV_CELL_FIRST_PHOTO, XrPanels.MAX_PHOTOS);
        assertTrue(XrPanels.MAX_PHOTOS >= 4);
    }
}

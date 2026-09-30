package com.limelight.preferences;

import android.content.SharedPreferences;

import com.limelight.binding.video.XrShared;

import org.junit.Test;

import java.util.HashMap;
import java.util.Map;
import java.util.Set;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertTrue;

/** Each room's own Room tab values: where they are kept, where they start, and the size's lane. */
public class RoomLevelsPrefsTest {

    private static final int THEATER = PreferenceConfiguration.VR_ENV_HOME_THEATER;
    private static final int GRAND = PreferenceConfiguration.VR_ENV_GRAND_CINEMA;
    private static final int SYNTHWAVE = PreferenceConfiguration.VR_ENV_SYNTHWAVE;

    @Test
    public void eachRoomHasKeysOfItsOwnByItsId() {
        assertEquals("room_brightness_4", PreferenceConfiguration.roomBrightnessKey(THEATER));
        assertEquals("room_glow_5", PreferenceConfiguration.roomGlowKey(GRAND));
        assertEquals("room_light_6", PreferenceConfiguration.roomLightKey(SYNTHWAVE));
        assertEquals("room_screen_4", PreferenceConfiguration.roomScreenKey(THEATER));
        assertNotEquals(PreferenceConfiguration.roomBrightnessKey(THEATER),
                PreferenceConfiguration.roomBrightnessKey(SYNTHWAVE));
    }

    @Test
    public void theAppWideKeysAreNotReused() {
        // Outside a room the glow and the screen light keep their one key each
        for (int id : new int[] { THEATER, GRAND, SYNTHWAVE }) {
            assertNotEquals(PreferenceConfiguration.VR_AMBILIGHT_PREF_STRING,
                    PreferenceConfiguration.roomGlowKey(id));
            assertNotEquals(PreferenceConfiguration.VR_ROOM_LIGHT_PREF_STRING,
                    PreferenceConfiguration.roomLightKey(id));
        }
    }

    @Test
    public void onlyTheBakedRoomsKeepValues() {
        assertTrue(PreferenceConfiguration.isRoomEnvironment(THEATER));
        assertTrue(PreferenceConfiguration.isRoomEnvironment(GRAND));
        assertTrue(PreferenceConfiguration.isRoomEnvironment(SYNTHWAVE));
        int[] others = { -1, PreferenceConfiguration.VR_ENV_PASSTHROUGH,
                PreferenceConfiguration.VR_ENV_VOID, 2, 3, 100 };
        for (int id : others) {
            assertFalse("id " + id, PreferenceConfiguration.isRoomEnvironment(id));
        }
    }

    @Test
    public void defaultsComeFromTheRoomTable() {
        // The brightness each room was tuned at in a headset, as the table has it
        assertEquals(37, PreferenceConfiguration.defaultRoomBrightness(THEATER));
        assertEquals(21, PreferenceConfiguration.defaultRoomBrightness(GRAND));
        assertEquals(53, PreferenceConfiguration.defaultRoomBrightness(SYNTHWAVE));
        // The Grand Cinema is dark enough that the glow spoils it
        assertTrue(PreferenceConfiguration.defaultRoomGlow(THEATER));
        assertFalse(PreferenceConfiguration.defaultRoomGlow(GRAND));
        assertTrue(PreferenceConfiguration.defaultRoomGlow(SYNTHWAVE));
        // The Home Theater starts at four fifths of its screen, the others whole
        assertEquals(80, PreferenceConfiguration.defaultRoomScreen(THEATER));
        assertEquals(100, PreferenceConfiguration.defaultRoomScreen(GRAND));
        assertEquals(100, PreferenceConfiguration.defaultRoomScreen(SYNTHWAVE));
        assertTrue(PreferenceConfiguration.roomResizable(THEATER));
        assertFalse(PreferenceConfiguration.roomResizable(GRAND));
        assertTrue(PreferenceConfiguration.roomResizable(SYNTHWAVE));
    }

    @Test
    public void theLightStartsThreeQuartersAlongItsLane() {
        int light = PreferenceConfiguration.defaultRoomLight();
        assertEquals(150, light);
        // One and a half times the gain the room's row gives it
        assertEquals(1.5f, light / 100.0f, 1e-6f);
        assertEquals(0.75f, (light - XrShared.ROOM_LIGHT_MIN)
                / (float)(XrShared.ROOM_LIGHT_MAX - XrShared.ROOM_LIGHT_MIN), 1e-6f);
    }

    @Test
    public void everyDefaultIsInsideItsLane() {
        for (int id : new int[] { THEATER, GRAND, SYNTHWAVE }) {
            int brightness = PreferenceConfiguration.defaultRoomBrightness(id);
            assertTrue(brightness >= XrShared.ROOM_BRIGHTNESS_MIN
                    && brightness <= XrShared.ROOM_BRIGHTNESS_MAX);
            int screen = PreferenceConfiguration.defaultRoomScreen(id);
            assertTrue(screen >= XrShared.ROOM_SCREEN_MIN && screen <= XrShared.ROOM_SCREEN_MAX);
        }
    }

    @Test
    public void theSizeIsClampedToAQuarterAndAll() {
        assertEquals(25, PreferenceConfiguration.clampRoomScreen(THEATER, 10));
        assertEquals(25, PreferenceConfiguration.clampRoomScreen(SYNTHWAVE, -5));
        assertEquals(60, PreferenceConfiguration.clampRoomScreen(THEATER, 60));
        assertEquals(100, PreferenceConfiguration.clampRoomScreen(SYNTHWAVE, 140));
    }

    @Test
    public void aRoomThatKeepsItsPictureWholeIsAlwaysAll() {
        assertEquals(100, PreferenceConfiguration.clampRoomScreen(GRAND, 50));
        assertEquals(100, PreferenceConfiguration.clampRoomScreen(GRAND, 25));
        assertEquals(100, PreferenceConfiguration.clampRoomScreen(GRAND, 100));
    }

    @Test
    public void nothingStoredReadsTheRoomsOwnDefaults() {
        FakePrefs prefs = new FakePrefs();
        PreferenceConfiguration.RoomLevels grand =
                PreferenceConfiguration.readRoomLevels(prefs, GRAND);
        assertEquals(21, grand.brightness);
        assertFalse(grand.glow);
        assertEquals(150, grand.light);
        assertEquals(100, grand.screen);
    }

    @Test
    public void eachRoomReadsOnlyItsOwnKeys() {
        FakePrefs prefs = new FakePrefs();
        prefs.putInt(PreferenceConfiguration.roomBrightnessKey(THEATER), 60);
        prefs.putBoolean(PreferenceConfiguration.roomGlowKey(THEATER), false);
        prefs.putInt(PreferenceConfiguration.roomLightKey(THEATER), 90);
        prefs.putInt(PreferenceConfiguration.roomScreenKey(THEATER), 40);
        // The app wide glow says nothing about a room
        prefs.putBoolean(PreferenceConfiguration.VR_AMBILIGHT_PREF_STRING, false);

        PreferenceConfiguration.RoomLevels theater =
                PreferenceConfiguration.readRoomLevels(prefs, THEATER);
        assertEquals(60, theater.brightness);
        assertFalse(theater.glow);
        assertEquals(90, theater.light);
        assertEquals(40, theater.screen);

        PreferenceConfiguration.RoomLevels synthwave =
                PreferenceConfiguration.readRoomLevels(prefs, SYNTHWAVE);
        assertEquals(53, synthwave.brightness);
        assertTrue(synthwave.glow);
        assertEquals(150, synthwave.light);
        assertEquals(100, synthwave.screen);
    }

    @Test
    public void storedValuesOutsideTheirLanesComeBackInside() {
        FakePrefs prefs = new FakePrefs();
        prefs.putInt(PreferenceConfiguration.roomBrightnessKey(SYNTHWAVE), 900);
        prefs.putInt(PreferenceConfiguration.roomLightKey(SYNTHWAVE), -20);
        prefs.putInt(PreferenceConfiguration.roomScreenKey(SYNTHWAVE), 3);
        prefs.putInt(PreferenceConfiguration.roomScreenKey(GRAND), 40);
        PreferenceConfiguration.RoomLevels synthwave =
                PreferenceConfiguration.readRoomLevels(prefs, SYNTHWAVE);
        assertEquals(XrShared.ROOM_BRIGHTNESS_MAX, synthwave.brightness);
        assertEquals(XrShared.ROOM_LIGHT_MIN, synthwave.light);
        assertEquals(XrShared.ROOM_SCREEN_MIN, synthwave.screen);
        assertEquals(100, PreferenceConfiguration.readRoomLevels(prefs, GRAND).screen);
    }

    @Test
    public void theLogLinesCarryAllFour() {
        PreferenceConfiguration.RoomLevels levels =
                new PreferenceConfiguration.RoomLevels(37, true, 150, 80);
        assertEquals("roomBrightness=37 roomGlow=true roomLight=150 roomScreen=80",
                levels.describe("="));
        assertEquals("roomBrightness 37 roomGlow true roomLight 150 roomScreen 80",
                levels.describe(" "));
    }

    // Just enough of SharedPreferences for the room keys
    private static final class FakePrefs implements SharedPreferences, SharedPreferences.Editor {
        final Map<String, Object> values = new HashMap<>();

        @Override public Map<String, ?> getAll() { return values; }
        @Override public String getString(String key, String def) {
            return values.containsKey(key) ? (String)values.get(key) : def;
        }
        @Override public Set<String> getStringSet(String key, Set<String> def) { return def; }
        @Override public int getInt(String key, int def) {
            return values.containsKey(key) ? (Integer)values.get(key) : def;
        }
        @Override public long getLong(String key, long def) { return def; }
        @Override public float getFloat(String key, float def) { return def; }
        @Override public boolean getBoolean(String key, boolean def) {
            return values.containsKey(key) ? (Boolean)values.get(key) : def;
        }
        @Override public boolean contains(String key) { return values.containsKey(key); }
        @Override public Editor edit() { return this; }
        @Override public void registerOnSharedPreferenceChangeListener(
                OnSharedPreferenceChangeListener listener) { }
        @Override public void unregisterOnSharedPreferenceChangeListener(
                OnSharedPreferenceChangeListener listener) { }

        @Override public Editor putString(String key, String value) {
            values.put(key, value);
            return this;
        }
        @Override public Editor putStringSet(String key, Set<String> value) { return this; }
        @Override public Editor putInt(String key, int value) {
            values.put(key, value);
            return this;
        }
        @Override public Editor putLong(String key, long value) { return this; }
        @Override public Editor putFloat(String key, float value) { return this; }
        @Override public Editor putBoolean(String key, boolean value) {
            values.put(key, value);
            return this;
        }
        @Override public Editor remove(String key) {
            values.remove(key);
            return this;
        }
        @Override public Editor clear() {
            values.clear();
            return this;
        }
        @Override public boolean commit() { return true; }
        @Override public void apply() { }
    }
}

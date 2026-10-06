package com.limelight.preferences;

import android.content.SharedPreferences;

import org.junit.Test;

import java.util.HashMap;
import java.util.Map;
import java.util.Set;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/** The pointer's own switches: their keys, where they start, and how the log lines name them. */
public class PointerPrefsTest {

    @Test
    public void theHandLockHintShowsUntilPutAwayForGood() {
        assertEquals("checkbox_vr_hand_lock_hint_seen",
                PreferenceConfiguration.VR_HAND_LOCK_HINT_SEEN_PREF_STRING);

        FakePrefs prefs = new FakePrefs();
        assertFalse(PreferenceConfiguration.handLockHintSeen(prefs));
        PreferenceConfiguration.markHandLockHintSeen(prefs);
        assertTrue(PreferenceConfiguration.handLockHintSeen(prefs));
        assertEquals(Boolean.TRUE,
                prefs.values.get(PreferenceConfiguration.VR_HAND_LOCK_HINT_SEEN_PREF_STRING));
        // The pointer's switches are left where they were
        assertTrue(PreferenceConfiguration.pointerSleepOn(prefs));
        assertTrue(PreferenceConfiguration.rayShown(prefs));
    }

    @Test
    public void thePointerPausesUnlessSwitchedOff() {
        assertEquals("checkbox_vr_pointer_sleep",
                PreferenceConfiguration.VR_POINTER_SLEEP_PREF_STRING);
        assertTrue(PreferenceConfiguration.DEFAULT_VR_POINTER_SLEEP);

        FakePrefs prefs = new FakePrefs();
        assertTrue(PreferenceConfiguration.pointerSleepOn(prefs));
        prefs.putBoolean(PreferenceConfiguration.VR_POINTER_SLEEP_PREF_STRING, false);
        assertFalse(PreferenceConfiguration.pointerSleepOn(prefs));
        // The other switches are left alone
        assertTrue(PreferenceConfiguration.rayShown(prefs));
        assertFalse(PreferenceConfiguration.handLockHintSeen(prefs));
    }

    @Test
    public void theLogLinesNameThemTheWayTheyJoinTheRest() {
        assertEquals("pointerSleep=true showRay=true controllerModel=false",
                PreferenceConfiguration.inputLabel(true, true, false, "="));
        assertEquals("pointerSleep false showRay false controllerModel false",
                PreferenceConfiguration.inputLabel(false, false, false, " "));
        assertEquals("pointerSleep=true showRay=false controllerModel=true",
                PreferenceConfiguration.inputLabel(true, false, true, "="));
    }

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

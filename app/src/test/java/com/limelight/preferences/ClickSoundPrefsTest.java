package com.limelight.preferences;

import android.content.SharedPreferences;

import org.junit.Test;

import java.util.HashMap;
import java.util.Map;
import java.util.Set;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

/** The click a press on the headset panels makes: its key, where it starts, and how it reads. */
public class ClickSoundPrefsTest {

    @Test
    public void theClickIsOnUnlessSwitchedOff() {
        assertEquals("checkbox_vr_click_sound", PreferenceConfiguration.VR_CLICK_SOUND_PREF_STRING);
        assertTrue(PreferenceConfiguration.DEFAULT_VR_CLICK_SOUND);

        FakePrefs prefs = new FakePrefs();
        assertTrue(PreferenceConfiguration.clickSoundOn(prefs));
        prefs.putBoolean(PreferenceConfiguration.VR_CLICK_SOUND_PREF_STRING, false);
        assertFalse(PreferenceConfiguration.clickSoundOn(prefs));
        prefs.putBoolean(PreferenceConfiguration.VR_CLICK_SOUND_PREF_STRING, true);
        assertTrue(PreferenceConfiguration.clickSoundOn(prefs));
    }

    @Test
    public void itIsItsOwnSwitch() {
        // Turning it off leaves the pointer's switches where they were
        FakePrefs prefs = new FakePrefs();
        prefs.putBoolean(PreferenceConfiguration.VR_CLICK_SOUND_PREF_STRING, false);
        assertTrue(PreferenceConfiguration.pointerSleepOn(prefs));
        assertTrue(PreferenceConfiguration.handLockIconShown(prefs));
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

package com.limelight.preferences;

import android.content.SharedPreferences;

import org.junit.Test;

import java.util.HashMap;
import java.util.Map;
import java.util.Set;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

/** Which depth source values name a model, where MiDaS is offered, and the move to ZipDepth. */
public class DepthSourcePrefsTest {

    @Test
    public void theDefaultIsZipDepth() {
        assertEquals("zipdepth", PreferenceConfiguration.DEFAULT_VR_DEPTH_SOURCE);
    }

    @Test
    public void onlyTheTwoModelsNameAModel() {
        assertTrue(PreferenceConfiguration.isDepthModel("zipdepth"));
        assertTrue(PreferenceConfiguration.isDepthModel("model"));
        assertFalse(PreferenceConfiguration.isDepthModel("off"));
        assertFalse(PreferenceConfiguration.isDepthModel("blob"));
        assertFalse(PreferenceConfiguration.isDepthModel(null));
    }

    @Test
    public void midasIsOfferedOnGen2Only() {
        assertTrue(PreferenceConfiguration.isDepthSourceOffered("model", false));
        assertFalse(PreferenceConfiguration.isDepthSourceOffered("model", true));
    }

    @Test
    public void everythingElseIsOfferedOnBothGenerations() {
        for (String value : new String[] { "zipdepth", "off", "flat", "ramp", "blob", "eyetest",
                "shifttest" }) {
            assertTrue(value, PreferenceConfiguration.isDepthSourceOffered(value, false));
            assertTrue(value, PreferenceConfiguration.isDepthSourceOffered(value, true));
        }
    }

    @Test
    public void aStoredMidasRunsAsZipDepthOnGen1() {
        assertEquals("zipdepth", PreferenceConfiguration.depthSourceForHeadset("model", true));
        assertEquals("model", PreferenceConfiguration.depthSourceForHeadset("model", false));
    }

    @Test
    public void anyOtherStoredValueIsLeftAlone() {
        for (boolean gen1 : new boolean[] { false, true }) {
            assertEquals("off", PreferenceConfiguration.depthSourceForHeadset("off", gen1));
            assertEquals("blob", PreferenceConfiguration.depthSourceForHeadset("blob", gen1));
            assertEquals("zipdepth", PreferenceConfiguration.depthSourceForHeadset("zipdepth", gen1));
        }
    }

    // An install from before ZipDepth, with MiDaS stored, moves once
    @Test
    public void aStoredMidasMovesToZipDepthOnce() {
        FakePrefs prefs = new FakePrefs();
        prefs.values.put("list_vr_depth_source", "model");
        assertTrue(PreferenceConfiguration.moveDepthSourceToZipDepth(prefs));
        assertEquals("zipdepth", prefs.values.get("list_vr_depth_source"));
        assertEquals(true, prefs.values.get("depth_source_zipdepth"));

        // MiDaS picked again afterwards stays picked
        prefs.values.put("list_vr_depth_source", "model");
        assertFalse(PreferenceConfiguration.moveDepthSourceToZipDepth(prefs));
        assertEquals("model", prefs.values.get("list_vr_depth_source"));
    }

    @Test
    public void anyOtherStoredValueIsNotMoved() {
        for (String value : new String[] { "off", "zipdepth", "blob" }) {
            FakePrefs prefs = new FakePrefs();
            prefs.values.put("list_vr_depth_source", value);
            assertFalse(PreferenceConfiguration.moveDepthSourceToZipDepth(prefs));
            assertEquals(value, prefs.values.get("list_vr_depth_source"));
            assertEquals(false, prefs.values.get("depth_source_zipdepth"));
        }
    }

    // A fresh install has nothing stored, and the xml default does the rest
    @Test
    public void aFreshInstallOnlyGetsTheMarker() {
        FakePrefs prefs = new FakePrefs();
        assertFalse(PreferenceConfiguration.moveDepthSourceToZipDepth(prefs));
        assertNull(prefs.values.get("list_vr_depth_source"));
        assertEquals(false, prefs.values.get("depth_source_zipdepth"));
    }

    // Just enough of SharedPreferences for the move, applied as it is edited
    private static final class FakePrefs implements SharedPreferences, SharedPreferences.Editor {
        final Map<String, Object> values = new HashMap<>();

        @Override public Map<String, ?> getAll() { return values; }
        @Override public String getString(String key, String def) {
            return values.containsKey(key) ? (String)values.get(key) : def;
        }
        @Override public Set<String> getStringSet(String key, Set<String> def) { return def; }
        @Override public int getInt(String key, int def) { return def; }
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
        @Override public Editor putInt(String key, int value) { return this; }
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

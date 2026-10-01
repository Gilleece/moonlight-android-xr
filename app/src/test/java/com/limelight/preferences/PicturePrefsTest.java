package com.limelight.preferences;

import android.content.SharedPreferences;

import com.limelight.binding.video.XrShared;

import org.junit.Test;
import org.w3c.dom.Element;
import org.w3c.dom.NodeList;

import java.io.File;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;

import javax.xml.parsers.DocumentBuilderFactory;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;

/**
 * The picture grade's four values: their keys, where they start, the lanes
 * they are held to, the log label, and the 2d seekbars that write them.
 */
public class PicturePrefsTest {

    private static final String SEEKBAR = "http://schemas.moonlight-stream.com/apk/res/seekbar";
    private static final String ANDROID = "http://schemas.android.com/apk/res/android";

    @Test
    public void theKeysAreTheSeekbars() {
        assertEquals("seekbar_vr_picture_brightness",
                PreferenceConfiguration.pictureKey(XrShared.PICTURE_BRIGHTNESS));
        assertEquals("seekbar_vr_picture_contrast",
                PreferenceConfiguration.pictureKey(XrShared.PICTURE_CONTRAST));
        assertEquals("seekbar_vr_picture_gamma",
                PreferenceConfiguration.pictureKey(XrShared.PICTURE_GAMMA));
        assertEquals("seekbar_vr_picture_saturation",
                PreferenceConfiguration.pictureKey(XrShared.PICTURE_SATURATION));
        assertEquals(4, XrShared.PICTURE_VALUES);
    }

    @Test
    public void nothingStoredIsThePictureAsStreamed() {
        assertArrayEquals(new int[] { 0, 100, 100, 100 },
                PreferenceConfiguration.readPicture(new FakePrefs()));
        for (int row = 0; row < XrShared.PICTURE_VALUES; row++) {
            assertEquals(row == XrShared.PICTURE_BRIGHTNESS ? 0 : 100,
                    PreferenceConfiguration.pictureDefault(row));
        }
    }

    @Test
    public void storedValuesAreReadAndHeldToTheirLanes() {
        FakePrefs prefs = new FakePrefs();
        prefs.putInt(PreferenceConfiguration.VR_PICTURE_BRIGHTNESS_PREF_STRING, 12);
        prefs.putInt(PreferenceConfiguration.VR_PICTURE_CONTRAST_PREF_STRING, 120);
        prefs.putInt(PreferenceConfiguration.VR_PICTURE_GAMMA_PREF_STRING, 160);
        prefs.putInt(PreferenceConfiguration.VR_PICTURE_SATURATION_PREF_STRING, 0);
        assertArrayEquals(new int[] { 12, 120, 160, 0 },
                PreferenceConfiguration.readPicture(prefs));

        prefs.putInt(PreferenceConfiguration.VR_PICTURE_BRIGHTNESS_PREF_STRING, -80);
        prefs.putInt(PreferenceConfiguration.VR_PICTURE_CONTRAST_PREF_STRING, 400);
        prefs.putInt(PreferenceConfiguration.VR_PICTURE_GAMMA_PREF_STRING, 10);
        prefs.putInt(PreferenceConfiguration.VR_PICTURE_SATURATION_PREF_STRING, 999);
        assertArrayEquals(new int[] { -50, 150, 50, 200 },
                PreferenceConfiguration.readPicture(prefs));
    }

    @Test
    public void theLanesAreTheRenderers() {
        int[][] lanes = {
                { XrShared.PICTURE_BRIGHTNESS_MIN, XrShared.PICTURE_BRIGHTNESS_MAX },
                { XrShared.PICTURE_CONTRAST_MIN, XrShared.PICTURE_CONTRAST_MAX },
                { XrShared.PICTURE_GAMMA_MIN, XrShared.PICTURE_GAMMA_MAX },
                { XrShared.PICTURE_SATURATION_MIN, XrShared.PICTURE_SATURATION_MAX },
        };
        for (int row = 0; row < lanes.length; row++) {
            assertEquals(lanes[row][0], PreferenceConfiguration.clampPicture(row, -1000));
            assertEquals(lanes[row][1], PreferenceConfiguration.clampPicture(row, 1000));
            int def = PreferenceConfiguration.pictureDefault(row);
            assertEquals(def, PreferenceConfiguration.clampPicture(row, def));
        }
    }

    @Test
    public void theLogSaysAllFour() {
        assertEquals("pictureBrightness=12 pictureContrast=120 pictureGamma=160 pictureSaturation=80",
                PreferenceConfiguration.pictureLabel(new int[] { 12, 120, 160, 80 }, "="));
        assertEquals("pictureBrightness -5 pictureContrast 100 pictureGamma 100 pictureSaturation 100",
                PreferenceConfiguration.pictureLabel(new int[] { -5, 100, 100, 100 }, " "));
    }

    // Each 2d seekbar runs over its whole lane, starts at its default, and
    // shows gamma to the hundredth, read from the settings screen itself
    @Test
    public void theSeekbarsCoverTheLanes() throws Exception {
        File xml = new File("src/main/res/xml/preferences.xml");
        if (!xml.isFile()) {
            xml = new File("app/src/main/res/xml/preferences.xml");
        }
        DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
        factory.setNamespaceAware(true);
        NodeList bars = factory.newDocumentBuilder().parse(xml)
                .getElementsByTagName("com.limelight.preferences.SeekBarPreference");
        int[][] lanes = {
                { XrShared.PICTURE_BRIGHTNESS_MIN, XrShared.PICTURE_BRIGHTNESS_MAX },
                { XrShared.PICTURE_CONTRAST_MIN, XrShared.PICTURE_CONTRAST_MAX },
                { XrShared.PICTURE_GAMMA_MIN, XrShared.PICTURE_GAMMA_MAX },
                { XrShared.PICTURE_SATURATION_MIN, XrShared.PICTURE_SATURATION_MAX },
        };
        for (int row = 0; row < XrShared.PICTURE_VALUES; row++) {
            Element bar = null;
            for (int i = 0; i < bars.getLength(); i++) {
                Element e = (Element)bars.item(i);
                if (PreferenceConfiguration.pictureKey(row).equals(e.getAttributeNS(ANDROID, "key"))) {
                    bar = e;
                }
            }
            assertNotNull(PreferenceConfiguration.pictureKey(row), bar);
            int offset = intAttr(bar, SEEKBAR, "offset", 0);
            int min = intAttr(bar, SEEKBAR, "min", 1);
            int max = intAttr(bar, ANDROID, "max", 100);
            assertEquals(lanes[row][0], min + offset);
            assertEquals(lanes[row][1], max + offset);
            assertEquals(PreferenceConfiguration.pictureDefault(row),
                    intAttr(bar, ANDROID, "defaultValue", -1));
        }
        Element gamma = null;
        for (int i = 0; i < bars.getLength(); i++) {
            Element e = (Element)bars.item(i);
            if ("seekbar_vr_picture_gamma".equals(e.getAttributeNS(ANDROID, "key"))) {
                gamma = e;
            }
        }
        assertNotNull(gamma);
        assertEquals(100, intAttr(gamma, SEEKBAR, "divisor", 1));
        assertEquals(2, intAttr(gamma, SEEKBAR, "decimals", 1));
    }

    private static int intAttr(Element e, String ns, String name, int def) {
        String v = e.getAttributeNS(ns, name);
        return v == null || v.isEmpty() ? def : Integer.parseInt(v);
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

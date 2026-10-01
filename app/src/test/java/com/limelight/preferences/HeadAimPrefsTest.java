package com.limelight.preferences;

import android.content.SharedPreferences;

import com.limelight.binding.video.XrShared;

import org.junit.Test;
import org.w3c.dom.Element;
import org.w3c.dom.NodeList;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import javax.xml.parsers.DocumentBuilderFactory;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

/**
 * Head aim: its switch, its pixels a degree and its dead zone, their keys,
 * where they start and the lanes they are held to, the slots the frame hands
 * its motion back in, where the settings screen has them, how the log lines
 * say them and that every language words them.
 */
public class HeadAimPrefsTest {

    private static final String ANDROID = "http://schemas.android.com/apk/res/android";
    private static final String SEEKBAR = "http://schemas.moonlight-stream.com/apk/res/seekbar";

    private static File res(String path) {
        File f = new File("src/main/res/" + path);
        return f.isFile() ? f : new File("app/src/main/res/" + path);
    }

    // The entry a key names in the settings screen, of that kind
    private static Element entry(String tag, String key) throws Exception {
        DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
        factory.setNamespaceAware(true);
        NodeList all = factory.newDocumentBuilder().parse(res("xml/preferences.xml"))
                .getElementsByTagName(tag);
        Element found = null;
        for (int i = 0; i < all.getLength(); i++) {
            Element e = (Element) all.item(i);
            if (key.equals(e.getAttributeNS(ANDROID, "key"))) {
                found = e;
            }
        }
        return found;
    }

    @Test
    public void itIsOffUntilSwitchedOn() {
        assertEquals("checkbox_vr_head_aim", PreferenceConfiguration.VR_HEAD_AIM_PREF_STRING);
        assertFalse(PreferenceConfiguration.DEFAULT_VR_HEAD_AIM);

        FakePrefs prefs = new FakePrefs();
        assertFalse(PreferenceConfiguration.headAimOn(prefs));
        prefs.putBoolean(PreferenceConfiguration.VR_HEAD_AIM_PREF_STRING, true);
        assertTrue(PreferenceConfiguration.headAimOn(prefs));
        // The pointer's own switches are not touched by it
        assertTrue(PreferenceConfiguration.rayShown(prefs));
        assertTrue(PreferenceConfiguration.pointerSleepOn(prefs));
    }

    @Test
    public void itStartsAtEightPixelsADegreeAndTwoDegreesASecond() {
        assertEquals("seekbar_vr_head_aim_sensitivity",
                PreferenceConfiguration.VR_HEAD_AIM_SENSITIVITY_PREF_STRING);
        assertEquals("seekbar_vr_head_aim_deadzone",
                PreferenceConfiguration.VR_HEAD_AIM_DEADZONE_PREF_STRING);
        assertEquals(8, XrShared.HEAD_AIM_SENSITIVITY_DEFAULT);
        assertEquals(1, XrShared.HEAD_AIM_SENSITIVITY_MIN);
        assertEquals(30, XrShared.HEAD_AIM_SENSITIVITY_MAX);
        assertEquals(2, XrShared.HEAD_AIM_DEADZONE_DEFAULT);
        assertEquals(0, XrShared.HEAD_AIM_DEADZONE_MIN);
        assertEquals(20, XrShared.HEAD_AIM_DEADZONE_MAX);

        FakePrefs prefs = new FakePrefs();
        assertEquals(8, PreferenceConfiguration.headAimSensitivity(prefs));
        assertEquals(2, PreferenceConfiguration.headAimDeadZone(prefs));
        prefs.putInt(PreferenceConfiguration.VR_HEAD_AIM_SENSITIVITY_PREF_STRING, 15);
        prefs.putInt(PreferenceConfiguration.VR_HEAD_AIM_DEADZONE_PREF_STRING, 0);
        assertEquals(15, PreferenceConfiguration.headAimSensitivity(prefs));
        assertEquals(0, PreferenceConfiguration.headAimDeadZone(prefs));
    }

    @Test
    public void storedValuesAreHeldToTheirLanes() {
        FakePrefs prefs = new FakePrefs();
        prefs.putInt(PreferenceConfiguration.VR_HEAD_AIM_SENSITIVITY_PREF_STRING, 0);
        prefs.putInt(PreferenceConfiguration.VR_HEAD_AIM_DEADZONE_PREF_STRING, -3);
        assertEquals(1, PreferenceConfiguration.headAimSensitivity(prefs));
        assertEquals(0, PreferenceConfiguration.headAimDeadZone(prefs));
        prefs.putInt(PreferenceConfiguration.VR_HEAD_AIM_SENSITIVITY_PREF_STRING, 99);
        prefs.putInt(PreferenceConfiguration.VR_HEAD_AIM_DEADZONE_PREF_STRING, 21);
        assertEquals(30, PreferenceConfiguration.headAimSensitivity(prefs));
        assertEquals(20, PreferenceConfiguration.headAimDeadZone(prefs));
    }

    @Test
    public void theMotionComesBackInTheLastSlots() {
        assertEquals(XrShared.IN_REPORT_ZONE + 1, XrShared.IN_HEAD_AIM);
        assertEquals(XrShared.IN_HEAD_AIM + 1, XrShared.IN_MOUSE_DX);
        assertEquals(XrShared.IN_MOUSE_DX + 1, XrShared.IN_MOUSE_DY);
        assertEquals(XrShared.IN_MOUSE_DY + 1, XrShared.IN_SLOTS);
    }

    @Test
    public void theSettingAndItsTwoSeekbarsSitInTheVrSettings() throws Exception {
        Element box = entry("CheckBoxPreference", PreferenceConfiguration.VR_HEAD_AIM_PREF_STRING);
        assertNotNull(box);
        assertEquals("false", box.getAttributeNS(ANDROID, "defaultValue"));
        // The bar can switch head aim on for a session with the setting off,
        // so nothing here waits on another setting
        assertEquals("", box.getAttributeNS(ANDROID, "dependency"));
        assertEquals("category_vr_settings",
                ((Element) box.getParentNode()).getAttributeNS(ANDROID, "key"));

        String seekbar = "com.limelight.preferences.SeekBarPreference";
        Element sens = entry(seekbar, PreferenceConfiguration.VR_HEAD_AIM_SENSITIVITY_PREF_STRING);
        assertNotNull(sens);
        assertEquals(String.valueOf(XrShared.HEAD_AIM_SENSITIVITY_DEFAULT),
                sens.getAttributeNS(ANDROID, "defaultValue"));
        assertEquals(String.valueOf(XrShared.HEAD_AIM_SENSITIVITY_MIN),
                sens.getAttributeNS(SEEKBAR, "min"));
        assertEquals(String.valueOf(XrShared.HEAD_AIM_SENSITIVITY_MAX),
                sens.getAttributeNS(ANDROID, "max"));
        assertEquals("", sens.getAttributeNS(ANDROID, "dependency"));

        Element dead = entry(seekbar, PreferenceConfiguration.VR_HEAD_AIM_DEADZONE_PREF_STRING);
        assertNotNull(dead);
        assertEquals(String.valueOf(XrShared.HEAD_AIM_DEADZONE_DEFAULT),
                dead.getAttributeNS(ANDROID, "defaultValue"));
        assertEquals(String.valueOf(XrShared.HEAD_AIM_DEADZONE_MIN),
                dead.getAttributeNS(SEEKBAR, "min"));
        assertEquals(String.valueOf(XrShared.HEAD_AIM_DEADZONE_MAX),
                dead.getAttributeNS(ANDROID, "max"));

        // Right under the setting, in that order
        assertTrue(box.getNextSibling() != null);
        Element after = nextElement(box);
        assertEquals(PreferenceConfiguration.VR_HEAD_AIM_SENSITIVITY_PREF_STRING,
                after.getAttributeNS(ANDROID, "key"));
        assertEquals(PreferenceConfiguration.VR_HEAD_AIM_DEADZONE_PREF_STRING,
                nextElement(after).getAttributeNS(ANDROID, "key"));
    }

    private static Element nextElement(Element e) {
        org.w3c.dom.Node n = e.getNextSibling();
        while (n != null && !(n instanceof Element)) {
            n = n.getNextSibling();
        }
        return (Element) n;
    }

    @Test
    public void theLogLinesSayHeadAim() {
        assertEquals("headAim=true headAimSensitivity=8 headAimDeadZone=2",
                PreferenceConfiguration.headAimLabel(true, 8, 2, "="));
        assertEquals("headAim false headAimSensitivity 30 headAimDeadZone 0",
                PreferenceConfiguration.headAimLabel(false, 30, 0, " "));
    }

    @Test
    public void itIsWordedInEveryLanguage() throws Exception {
        for (String dir : new String[] { "values", "values-fr", "values-zh-rCN", "values-zh-rTW" }) {
            String strings = new String(Files.readAllBytes(res(dir + "/strings.xml").toPath()),
                    StandardCharsets.UTF_8);
            for (String key : new String[] { "title_vr_head_aim", "summary_vr_head_aim",
                    "title_seekbar_vr_head_aim_sensitivity",
                    "summary_seekbar_vr_head_aim_sensitivity",
                    "suffix_seekbar_vr_head_aim_sensitivity",
                    "title_seekbar_vr_head_aim_deadzone", "summary_seekbar_vr_head_aim_deadzone",
                    "suffix_seekbar_vr_head_aim_deadzone", "vr_toast_head_aim_on",
                    "vr_toast_head_aim_on_more", "vr_toast_head_aim_off" }) {
                Matcher m = Pattern.compile("name=\"" + key + "\">([^<]+)<").matcher(strings);
                assertTrue(dir + " " + key, m.find());
            }
        }
    }

    @Test
    public void theBarSaysWhenItSwitches() {
        assertEquals(6, XrShared.TOAST_HEAD_AIM_OFF);
        assertEquals(7, XrShared.TOAST_HEAD_AIM_ON);
        assertTrue(XrShared.TOAST_HEAD_AIM_OFF >= XrShared.TOAST_TEXT + 1);
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

package com.limelight.preferences;

import android.content.SharedPreferences;

import com.limelight.binding.video.XrShared;

import org.junit.Test;
import org.w3c.dom.Element;
import org.w3c.dom.NodeList;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
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
 * What the controllers are when a session starts, the pointer or one gamepad:
 * its key and values, where it starts, where the settings screen has it, how
 * the settings and report lines say it, and that every language words it and
 * the toasts that say when it switches.
 */
public class GamepadPrefsTest {

    private static final String ANDROID = "http://schemas.android.com/apk/res/android";

    private static File res(String path) {
        File f = new File("src/main/res/" + path);
        return f.isFile() ? f : new File("app/src/main/res/" + path);
    }

    private static org.w3c.dom.Document parse(String path) throws Exception {
        DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
        factory.setNamespaceAware(true);
        return factory.newDocumentBuilder().parse(res(path));
    }

    // The entry a key names in the settings screen, of that kind
    private static Element entry(String tag, String key) throws Exception {
        NodeList all = parse("xml/preferences.xml").getElementsByTagName(tag);
        Element found = null;
        for (int i = 0; i < all.getLength(); i++) {
            Element e = (Element) all.item(i);
            if (key.equals(e.getAttributeNS(ANDROID, "key"))) {
                found = e;
            }
        }
        return found;
    }

    // The items of a string array, as written
    private static List<String> array(String name) throws Exception {
        NodeList arrays = parse("values/arrays.xml").getElementsByTagName("string-array");
        List<String> items = new ArrayList<>();
        for (int i = 0; i < arrays.getLength(); i++) {
            Element a = (Element) arrays.item(i);
            if (!name.equals(a.getAttribute("name"))) {
                continue;
            }
            NodeList children = a.getElementsByTagName("item");
            for (int j = 0; j < children.getLength(); j++) {
                items.add(children.item(j).getTextContent().trim());
            }
        }
        return items;
    }

    @Test
    public void theControllersStartAsThePointer() {
        assertEquals("list_vr_controller_mode",
                PreferenceConfiguration.VR_CONTROLLER_MODE_PREF_STRING);
        assertEquals("pointer", PreferenceConfiguration.VR_CONTROLLER_MODE_POINTER);
        assertEquals("gamepad", PreferenceConfiguration.VR_CONTROLLER_MODE_GAMEPAD);
        assertEquals("pointer", PreferenceConfiguration.DEFAULT_VR_CONTROLLER_MODE);

        FakePrefs prefs = new FakePrefs();
        assertFalse(PreferenceConfiguration.gamepadAtStart(prefs));
        prefs.putString(PreferenceConfiguration.VR_CONTROLLER_MODE_PREF_STRING, "gamepad");
        assertTrue(PreferenceConfiguration.gamepadAtStart(prefs));
        prefs.putString(PreferenceConfiguration.VR_CONTROLLER_MODE_PREF_STRING, "pointer");
        assertFalse(PreferenceConfiguration.gamepadAtStart(prefs));
        // Anything it does not know is the pointer, as it always was
        prefs.putString(PreferenceConfiguration.VR_CONTROLLER_MODE_PREF_STRING, "mouse");
        assertFalse(PreferenceConfiguration.gamepadAtStart(prefs));
        // And the pointer's own switches are not touched by it
        prefs.putString(PreferenceConfiguration.VR_CONTROLLER_MODE_PREF_STRING, "gamepad");
        assertTrue(PreferenceConfiguration.rayShown(prefs));
        assertTrue(PreferenceConfiguration.pointerSleepOn(prefs));
        assertFalse(PreferenceConfiguration.headAimOn(prefs));
    }

    @Test
    public void itIsAListInTheVrSettings() throws Exception {
        Element list = entry("ListPreference",
                PreferenceConfiguration.VR_CONTROLLER_MODE_PREF_STRING);
        assertNotNull(list);
        assertEquals("pointer", list.getAttributeNS(ANDROID, "defaultValue"));
        assertEquals("@array/vr_controller_mode_names", list.getAttributeNS(ANDROID, "entries"));
        assertEquals("@array/vr_controller_mode_values",
                list.getAttributeNS(ANDROID, "entryValues"));
        // A pad works with the mouse switched off, so it waits on nothing
        assertEquals("", list.getAttributeNS(ANDROID, "dependency"));
        assertEquals("category_vr_settings",
                ((Element) list.getParentNode()).getAttributeNS(ANDROID, "key"));

        List<String> values = array("vr_controller_mode_values");
        assertEquals(2, values.size());
        assertEquals(PreferenceConfiguration.VR_CONTROLLER_MODE_POINTER, values.get(0));
        assertEquals(PreferenceConfiguration.VR_CONTROLLER_MODE_GAMEPAD, values.get(1));
        List<String> names = array("vr_controller_mode_names");
        assertEquals(2, names.size());
        assertEquals("@string/vr_controller_mode_pointer", names.get(0));
        assertEquals("@string/vr_controller_mode_gamepad", names.get(1));
    }

    @Test
    public void theSettingsAndReportLinesSayIt() {
        assertEquals("controllerMode=gamepad",
                PreferenceConfiguration.controllerModeLabel(true, "="));
        assertEquals("controllerMode=pointer",
                PreferenceConfiguration.controllerModeLabel(false, "="));
        assertEquals("controllerMode gamepad",
                PreferenceConfiguration.controllerModeLabel(true, " "));
        assertEquals("controllerMode pointer",
                PreferenceConfiguration.controllerModeLabel(false, " "));
    }

    @Test
    public void theToastsSayWhichWayItWent() {
        assertEquals(8, XrShared.TOAST_GAMEPAD_MODE);
        assertEquals(9, XrShared.TOAST_POINTER_MODE);
        assertTrue(XrShared.TOAST_GAMEPAD_MODE > XrShared.TOAST_HEAD_AIM_ON);
    }

    @Test
    public void itIsWordedInEveryLanguage() throws Exception {
        for (String dir : new String[] { "values", "values-fr", "values-zh-rCN", "values-zh-rTW" }) {
            String strings = new String(Files.readAllBytes(res(dir + "/strings.xml").toPath()),
                    StandardCharsets.UTF_8);
            for (String key : new String[] { "title_vr_controller_mode",
                    "summary_vr_controller_mode", "vr_controller_mode_pointer",
                    "vr_controller_mode_gamepad", "vr_toast_gamepad_mode",
                    "vr_toast_gamepad_mode_more", "vr_toast_pointer_mode" }) {
                Matcher m = Pattern.compile("name=\"" + key + "\">([^<]+)<").matcher(strings);
                assertTrue(dir + " " + key, m.find());
            }
        }
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

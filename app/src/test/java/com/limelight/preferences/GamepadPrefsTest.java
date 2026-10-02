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
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

/**
 * Gamepad mode's shortcut on the controllers: its key, values and default,
 * where the settings screen has it, how the settings and report lines say it,
 * the toast and Display tab words for each, and that every language words it.
 * The mode itself is never a setting: every session starts as the pointer.
 */
public class GamepadPrefsTest {

    private static final String ANDROID = "http://schemas.android.com/apk/res/android";
    private static final String[] LANGUAGES =
            { "values", "values-fr", "values-zh-rCN", "values-zh-rTW" };

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

    private static String strings(String dir) throws Exception {
        return new String(Files.readAllBytes(res(dir + "/strings.xml").toPath()),
                StandardCharsets.UTF_8);
    }

    private static String string(String strings, String key) {
        Matcher m = Pattern.compile("name=\"" + key + "\">([^<]+)<").matcher(strings);
        return m.find() ? m.group(1) : null;
    }

    @Test
    public void theMenuButtonAndGripIsTheDefault() {
        assertEquals("list_vr_gamepad_toggle", PreferenceConfiguration.VR_GAMEPAD_TOGGLE_PREF_STRING);
        assertEquals("menu_grip", PreferenceConfiguration.VR_GAMEPAD_TOGGLE_MENU_GRIP);
        assertEquals("sticks", PreferenceConfiguration.VR_GAMEPAD_TOGGLE_STICKS);
        assertEquals("triggers_grips", PreferenceConfiguration.VR_GAMEPAD_TOGGLE_TRIGGERS_GRIPS);
        assertEquals("menu_grip", PreferenceConfiguration.DEFAULT_VR_GAMEPAD_TOGGLE);
        assertEquals(0, XrShared.PAD_SHORTCUT_MENU_GRIP);
        assertEquals(1, XrShared.PAD_SHORTCUT_STICKS);
        assertEquals(2, XrShared.PAD_SHORTCUT_TRIGGERS_GRIPS);

        FakePrefs prefs = new FakePrefs();
        assertEquals(XrShared.PAD_SHORTCUT_MENU_GRIP, PreferenceConfiguration.gamepadToggle(prefs));
        prefs.putString(PreferenceConfiguration.VR_GAMEPAD_TOGGLE_PREF_STRING, "sticks");
        assertEquals(XrShared.PAD_SHORTCUT_STICKS, PreferenceConfiguration.gamepadToggle(prefs));
        prefs.putString(PreferenceConfiguration.VR_GAMEPAD_TOGGLE_PREF_STRING, "triggers_grips");
        assertEquals(XrShared.PAD_SHORTCUT_TRIGGERS_GRIPS,
                PreferenceConfiguration.gamepadToggle(prefs));
        prefs.putString(PreferenceConfiguration.VR_GAMEPAD_TOGGLE_PREF_STRING, "menu_grip");
        assertEquals(XrShared.PAD_SHORTCUT_MENU_GRIP, PreferenceConfiguration.gamepadToggle(prefs));
        // Anything it does not know is the menu button and grip
        prefs.putString(PreferenceConfiguration.VR_GAMEPAD_TOGGLE_PREF_STRING, "gamepad");
        assertEquals(XrShared.PAD_SHORTCUT_MENU_GRIP, PreferenceConfiguration.gamepadToggle(prefs));
        // And the pointer's own switches are not touched by it
        prefs.putString(PreferenceConfiguration.VR_GAMEPAD_TOGGLE_PREF_STRING, "sticks");
        assertTrue(PreferenceConfiguration.rayShown(prefs));
        assertTrue(PreferenceConfiguration.pointerSleepOn(prefs));
        assertFalse(PreferenceConfiguration.headAimOn(prefs));
    }

    @Test
    public void itIsAListInTheVrSettings() throws Exception {
        Element list = entry("ListPreference", PreferenceConfiguration.VR_GAMEPAD_TOGGLE_PREF_STRING);
        assertNotNull(list);
        assertEquals("menu_grip", list.getAttributeNS(ANDROID, "defaultValue"));
        assertEquals("@array/vr_gamepad_toggle_names", list.getAttributeNS(ANDROID, "entries"));
        assertEquals("@array/vr_gamepad_toggle_values",
                list.getAttributeNS(ANDROID, "entryValues"));
        assertEquals("@string/summary_vr_gamepad_toggle", list.getAttributeNS(ANDROID, "summary"));
        // A pad works with the mouse switched off, so it waits on nothing
        assertEquals("", list.getAttributeNS(ANDROID, "dependency"));
        assertEquals("category_vr_settings",
                ((Element) list.getParentNode()).getAttributeNS(ANDROID, "key"));

        // In the PAD_SHORTCUT_ order
        List<String> values = array("vr_gamepad_toggle_values");
        assertEquals(3, values.size());
        assertEquals(PreferenceConfiguration.VR_GAMEPAD_TOGGLE_MENU_GRIP,
                values.get(XrShared.PAD_SHORTCUT_MENU_GRIP));
        assertEquals(PreferenceConfiguration.VR_GAMEPAD_TOGGLE_STICKS,
                values.get(XrShared.PAD_SHORTCUT_STICKS));
        assertEquals(PreferenceConfiguration.VR_GAMEPAD_TOGGLE_TRIGGERS_GRIPS,
                values.get(XrShared.PAD_SHORTCUT_TRIGGERS_GRIPS));
        List<String> names = array("vr_gamepad_toggle_names");
        assertEquals(3, names.size());
        assertEquals("@string/vr_gamepad_toggle_menu_grip", names.get(0));
        assertEquals("@string/vr_gamepad_toggle_sticks", names.get(1));
        assertEquals("@string/vr_gamepad_toggle_triggers_grips", names.get(2));
    }

    @Test
    public void noSessionStartsInGamepadMode() throws Exception {
        // The start mode setting is gone, its list, its words and its field
        assertNull(entry("ListPreference", "list_vr_controller_mode"));
        assertTrue(array("vr_controller_mode_values").isEmpty());
        for (String dir : LANGUAGES) {
            String strings = strings(dir);
            assertFalse(dir, strings.contains("vr_controller_mode\""));
            assertFalse(dir, strings.contains("vr_controller_mode_"));
        }
        for (java.lang.reflect.Field field : PreferenceConfiguration.class.getFields()) {
            assertFalse(field.getName(), field.getName().equals("vrGamepadMode"));
        }
    }

    @Test
    public void theSettingsAndReportLinesSayIt() {
        assertEquals("gamepadShortcut=menu_grip",
                PreferenceConfiguration.gamepadToggleLabel(XrShared.PAD_SHORTCUT_MENU_GRIP, "="));
        assertEquals("gamepadShortcut=sticks",
                PreferenceConfiguration.gamepadToggleLabel(XrShared.PAD_SHORTCUT_STICKS, "="));
        assertEquals("gamepadShortcut triggers_grips",
                PreferenceConfiguration.gamepadToggleLabel(XrShared.PAD_SHORTCUT_TRIGGERS_GRIPS,
                        " "));
        assertEquals("gamepadShortcut menu_grip",
                PreferenceConfiguration.gamepadToggleLabel(7, " "));
    }

    @Test
    public void theToastAndTheListWordTheShortcutPlainly() throws Exception {
        assertEquals(8, XrShared.TOAST_GAMEPAD_MODE);
        assertEquals(9, XrShared.TOAST_POINTER_MODE);
        assertTrue(XrShared.TOAST_GAMEPAD_MODE > XrShared.TOAST_HEAD_AIM_ON);
        String english = strings("values");
        assertEquals("Press both thumbsticks to go back to the pointer",
                string(english, "vr_toast_gamepad_back_sticks"));
        assertEquals("Squeeze both triggers and both grips to go back to the pointer",
                string(english, "vr_toast_gamepad_back_triggers_grips"));
        assertEquals("Hold the left menu button and left grip to go back to the pointer",
                string(english, "vr_toast_gamepad_back_menu_grip"));
        assertEquals("Gamepad mode shortcut", string(english, "title_vr_gamepad_toggle"));
        assertEquals("Hold the left menu button and left grip",
                string(english, "vr_gamepad_toggle_menu_grip"));
        assertEquals("Press both thumbsticks (L3 + R3)", string(english, "vr_gamepad_toggle_sticks"));
        assertEquals("Squeeze both triggers and both grips",
                string(english, "vr_gamepad_toggle_triggers_grips"));
        // The old toast line named the menu and grip whatever was chosen
        for (String dir : LANGUAGES) {
            assertNull(dir, string(strings(dir), "vr_toast_gamepad_mode_more"));
        }
    }

    @Test
    public void itIsWordedInEveryLanguage() throws Exception {
        for (String dir : LANGUAGES) {
            String strings = strings(dir);
            for (String key : new String[] { "title_vr_gamepad_toggle",
                    "summary_vr_gamepad_toggle", "vr_gamepad_toggle_menu_grip",
                    "vr_gamepad_toggle_sticks", "vr_gamepad_toggle_triggers_grips",
                    "vr_toast_gamepad_mode", "vr_toast_gamepad_back_menu_grip",
                    "vr_toast_gamepad_back_sticks", "vr_toast_gamepad_back_triggers_grips",
                    "vr_toast_pointer_mode" }) {
                assertNotNull(dir + " " + key, string(strings, key));
            }
            // The list puts the shortcut chosen in its summary, so it carries
            // exactly one place for it and no other format marks
            String summary = string(strings, "summary_vr_gamepad_toggle");
            assertEquals(dir, summary.indexOf("%s"), summary.lastIndexOf("%s"));
            assertTrue(dir, summary.contains("%s"));
            assertEquals(dir, 1, summary.split("%", -1).length - 1);
            assertTrue(dir, String.format(summary, "x").contains("x"));
            // And says the mode is switched on inside a session
            assertTrue(dir, summary.contains("Display"));
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

package com.limelight.preferences;

import android.content.SharedPreferences;

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

/** How the controllers are drawn: the ray's switch, its key, where it starts, where it sits. */
public class ControllerPrefsTest {

    private static final String ANDROID = "http://schemas.android.com/apk/res/android";

    private static File res(String path) {
        File f = new File("src/main/res/" + path);
        return f.isFile() ? f : new File("app/src/main/res/" + path);
    }

    // The checkbox a key names in the settings screen
    private static Element checkbox(String key) throws Exception {
        DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
        factory.setNamespaceAware(true);
        NodeList boxes = factory.newDocumentBuilder().parse(res("xml/preferences.xml"))
                .getElementsByTagName("CheckBoxPreference");
        Element found = null;
        for (int i = 0; i < boxes.getLength(); i++) {
            Element e = (Element) boxes.item(i);
            if (key.equals(e.getAttributeNS(ANDROID, "key"))) {
                found = e;
            }
        }
        return found;
    }

    @Test
    public void theRayShowsUnlessSwitchedOff() {
        assertEquals("checkbox_vr_show_ray", PreferenceConfiguration.VR_SHOW_RAY_PREF_STRING);
        assertTrue(PreferenceConfiguration.DEFAULT_VR_SHOW_RAY);

        FakePrefs prefs = new FakePrefs();
        assertTrue(PreferenceConfiguration.rayShown(prefs));
        prefs.putBoolean(PreferenceConfiguration.VR_SHOW_RAY_PREF_STRING, false);
        assertFalse(PreferenceConfiguration.rayShown(prefs));
        // The pointer's other switches are left where they were
        assertTrue(PreferenceConfiguration.pointerSleepOn(prefs));
        assertTrue(PreferenceConfiguration.handLockIconShown(prefs));
        prefs.putBoolean(PreferenceConfiguration.VR_SHOW_RAY_PREF_STRING, true);
        assertTrue(PreferenceConfiguration.rayShown(prefs));
    }

    @Test
    public void theRaySitsWithThePointerInTheVrSettings() throws Exception {
        Element box = checkbox(PreferenceConfiguration.VR_SHOW_RAY_PREF_STRING);
        assertNotNull(box);
        assertEquals("true", box.getAttributeNS(ANDROID, "defaultValue"));
        // There is no ray at all with the pointer off
        assertEquals("checkbox_vr_pointer", box.getAttributeNS(ANDROID, "dependency"));
        Element category = (Element) box.getParentNode();
        assertEquals("category_vr_settings", category.getAttributeNS(ANDROID, "key"));
    }

    @Test
    public void theLogLinesSayWhetherTheRayShows() {
        assertTrue(PreferenceConfiguration.inputLabel(true, true, false, "=")
                .endsWith(" showRay=false"));
        assertTrue(PreferenceConfiguration.inputLabel(true, true, true, " ")
                .endsWith(" showRay true"));
    }

    @Test
    public void itIsWordedInEveryLanguage() throws Exception {
        for (String dir : new String[] { "values", "values-fr", "values-zh-rCN", "values-zh-rTW" }) {
            String strings = new String(Files.readAllBytes(res(dir + "/strings.xml").toPath()),
                    StandardCharsets.UTF_8);
            for (String key : new String[] { "title_vr_show_ray", "summary_vr_show_ray" }) {
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

package com.limelight.preferences;

import android.content.SharedPreferences;

import com.limelight.binding.video.EnvironmentIds;
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
 * The environment list in the 2D settings: it shows and writes the same saved
 * id the picker in the headset does, both ways, keeps the passthrough
 * checkbox the way the picker keeps it, and is worded in every language.
 */
public class EnvironmentPrefsTest {

    private static final String ANDROID = "http://schemas.android.com/apk/res/android";
    private static final String[] LANGUAGES =
            { "values", "values-fr", "values-zh-rCN", "values-zh-rTW" };
    private static final String ID = PreferenceConfiguration.VR_ENVIRONMENT_ID_PREF_STRING;
    private static final String PASSTHROUGH = PreferenceConfiguration.VR_PASSTHROUGH_PREF_STRING;
    private static final String LEGACY = PreferenceConfiguration.VR_ENVIRONMENT_PREF_STRING;
    // Every id the picker offers, in its order
    private static final int[] OFFERED = {
            PreferenceConfiguration.VR_ENV_PASSTHROUGH, PreferenceConfiguration.VR_ENV_VOID,
            PreferenceConfiguration.VR_ENV_HOME_THEATER, PreferenceConfiguration.VR_ENV_GRAND_CINEMA,
            PreferenceConfiguration.VR_ENV_SYNTHWAVE,
    };

    @Test
    public void thePickersChoiceIsWhatTheListShows() {
        // Whatever the passthrough checkbox says, once something is picked
        for (int id : OFFERED) {
            for (boolean passthrough : new boolean[] { false, true }) {
                FakePrefs prefs = new FakePrefs();
                prefs.putInt(ID, id);
                prefs.putBoolean(PASSTHROUGH, passthrough);
                assertEquals("id " + id, Integer.toString(id), EnvironmentIds.listValue(prefs));
                assertEquals(Integer.toString(id), EnvironmentIds.listValue(prefs, !passthrough));
            }
        }
    }

    @Test
    public void theListsChoiceIsWhatTheNextSessionOpensWith() {
        for (int id : OFFERED) {
            FakePrefs prefs = new FakePrefs();
            assertEquals(id, EnvironmentIds.idForListValue(Integer.toString(id)));
            assertTrue(EnvironmentIds.store(prefs, EnvironmentIds.idForListValue(Integer.toString(id))));
            // The same key and type the picker writes and the session reads
            assertEquals(id, prefs.getInt(ID, -1));
            assertTrue(prefs.values.get(ID) instanceof Integer);
            // Nothing under the list's own key
            assertFalse(prefs.contains(PreferenceConfiguration.VR_ENVIRONMENT_LIST_PREF_STRING));
            // And back again
            assertEquals(Integer.toString(id), EnvironmentIds.listValue(prefs));
        }
    }

    @Test
    public void thePassthroughCheckboxFollowsTheList() {
        // As the picker keeps it: on for passthrough, off for anything else
        FakePrefs prefs = new FakePrefs();
        EnvironmentIds.store(prefs, PreferenceConfiguration.VR_ENV_PASSTHROUGH);
        assertTrue(prefs.getBoolean(PASSTHROUGH, false));
        for (int id : OFFERED) {
            EnvironmentIds.store(prefs, id);
            assertEquals("id " + id, id == PreferenceConfiguration.VR_ENV_PASSTHROUGH,
                    prefs.getBoolean(PASSTHROUGH, !(id == PreferenceConfiguration.VR_ENV_PASSTHROUGH)));
        }
    }

    @Test
    public void neverPickedTheCheckboxStillDecides() {
        FakePrefs prefs = new FakePrefs();
        // The default is the void, as a fresh install starts
        assertEquals("1", EnvironmentIds.listValue(prefs));
        prefs.putBoolean(PASSTHROUGH, true);
        assertEquals("0", EnvironmentIds.listValue(prefs));
        prefs.putBoolean(PASSTHROUGH, false);
        assertEquals("1", EnvironmentIds.listValue(prefs));
        // The checkbox about to change moves the list with it
        assertEquals("0", EnvironmentIds.listValue(prefs, true));
        assertEquals("1", EnvironmentIds.listValue(prefs, false));
        // Reading writes nothing, so the checkbox keeps deciding
        assertFalse(prefs.contains(ID));
    }

    @Test
    public void anOldInstallsCellReadsAsTheSessionReadsIt() {
        FakePrefs prefs = new FakePrefs();
        prefs.putInt(LEGACY, 0);
        assertEquals("0", EnvironmentIds.listValue(prefs, false));
        prefs.putInt(LEGACY, 1);
        assertEquals("1", EnvironmentIds.listValue(prefs, true));
        // A photo or a room since removed comes up in the void
        for (int legacy = 2; legacy < 8; legacy++) {
            prefs.putInt(LEGACY, legacy);
            assertEquals("cell " + legacy, "1", EnvironmentIds.listValue(prefs, true));
        }
        // One that layout never had is no choice at all
        prefs.putInt(LEGACY, 8);
        assertEquals("0", EnvironmentIds.listValue(prefs, true));
        // And the id wins over it
        prefs.putInt(ID, PreferenceConfiguration.VR_ENV_SYNTHWAVE);
        prefs.putInt(LEGACY, 0);
        assertEquals("6", EnvironmentIds.listValue(prefs, true));
        assertFalse(prefs.contains(PreferenceConfiguration.VR_ENVIRONMENT_LIST_PREF_STRING));
    }

    @Test
    public void aRetiredIdShowsAsTheVoid() {
        for (int id : new int[] { 2, 3, 7, 99, 100, 103 }) {
            FakePrefs prefs = new FakePrefs();
            prefs.putInt(ID, id);
            prefs.putBoolean(PASSTHROUGH, true);
            assertEquals("id " + id, "1", EnvironmentIds.listValue(prefs));
        }
    }

    @Test
    public void nothingElseIsWritten() {
        for (String value : new String[] { "2", "3", "7", "100", "-1", "", "void", null, "4.0" }) {
            FakePrefs prefs = new FakePrefs();
            prefs.putInt(ID, PreferenceConfiguration.VR_ENV_HOME_THEATER);
            prefs.putBoolean(PASSTHROUGH, true);
            assertEquals("value " + value, -1, EnvironmentIds.idForListValue(value));
            assertFalse(EnvironmentIds.store(prefs, EnvironmentIds.idForListValue(value)));
            assertEquals(PreferenceConfiguration.VR_ENV_HOME_THEATER, prefs.getInt(ID, -1));
            assertTrue(prefs.getBoolean(PASSTHROUGH, false));
        }
        assertEquals(5, EnvironmentIds.idForListValue(" 5 "));
    }

    @Test
    public void itIsAListInTheVrSettingsOverThePickersCells() throws Exception {
        Element list = entry("ListPreference", PreferenceConfiguration.VR_ENVIRONMENT_LIST_PREF_STRING);
        assertNotNull(list);
        assertEquals("category_vr_settings",
                ((Element) list.getParentNode()).getAttributeNS(ANDROID, "key"));
        // Kept in the id, so neither stored under its own key nor given a
        // default that would be written over a choice never made
        assertEquals("false", list.getAttributeNS(ANDROID, "persistent"));
        assertEquals("", list.getAttributeNS(ANDROID, "defaultValue"));
        // Reachable with the controller pointer off, which is its point
        assertEquals("", list.getAttributeNS(ANDROID, "dependency"));
        assertEquals("@array/vr_environment_names", list.getAttributeNS(ANDROID, "entries"));
        assertEquals("@array/vr_environment_values", list.getAttributeNS(ANDROID, "entryValues"));
        assertEquals("@string/summary_vr_environment", list.getAttributeNS(ANDROID, "summary"));
        // Next to the passthrough checkbox it keeps
        Element prev = previousElement(list);
        assertEquals(PASSTHROUGH, prev.getAttributeNS(ANDROID, "key"));

        // The values are the ids, in the picker's order
        List<String> values = array("vr_environment_values");
        assertEquals(XrShared.ENV_CELL_COUNT, values.size());
        assertEquals(Integer.toString(PreferenceConfiguration.VR_ENV_PASSTHROUGH),
                values.get(XrShared.ENV_CELL_PASSTHROUGH));
        assertEquals(Integer.toString(PreferenceConfiguration.VR_ENV_VOID),
                values.get(XrShared.ENV_CELL_VOID));
        assertEquals(Integer.toString(PreferenceConfiguration.VR_ENV_HOME_THEATER),
                values.get(XrShared.ENV_CELL_HOME_THEATER));
        assertEquals(Integer.toString(PreferenceConfiguration.VR_ENV_GRAND_CINEMA),
                values.get(XrShared.ENV_CELL_GRAND_CINEMA));
        assertEquals(Integer.toString(PreferenceConfiguration.VR_ENV_SYNTHWAVE),
                values.get(XrShared.ENV_CELL_SYNTHWAVE));
        for (String value : values) {
            assertTrue(value, EnvironmentIds.idForListValue(value) >= 0);
        }
        List<String> names = array("vr_environment_names");
        assertEquals(values.size(), names.size());
        assertEquals("@string/vr_environment_passthrough", names.get(XrShared.ENV_CELL_PASSTHROUGH));
        assertEquals("@string/vr_environment_void", names.get(XrShared.ENV_CELL_VOID));
        assertEquals("@string/vr_environment_home_theater",
                names.get(XrShared.ENV_CELL_HOME_THEATER));
        assertEquals("@string/vr_environment_grand_cinema",
                names.get(XrShared.ENV_CELL_GRAND_CINEMA));
        assertEquals("@string/vr_environment_synthwave", names.get(XrShared.ENV_CELL_SYNTHWAVE));
    }

    @Test
    public void itIsWordedInEveryLanguage() throws Exception {
        String english = strings("values");
        // The picker's own labels
        assertEquals("Passthrough", string(english, "vr_environment_passthrough"));
        assertEquals("Black void", string(english, "vr_environment_void"));
        assertEquals("Home Theater", string(english, "vr_environment_home_theater"));
        assertEquals("Grand Cinema", string(english, "vr_environment_grand_cinema"));
        assertEquals("Synthwave", string(english, "vr_environment_synthwave"));
        for (String dir : LANGUAGES) {
            String strings = strings(dir);
            for (String key : new String[] { "title_vr_environment", "summary_vr_environment",
                    "vr_environment_passthrough", "vr_environment_void",
                    "vr_environment_home_theater", "vr_environment_grand_cinema",
                    "vr_environment_synthwave" }) {
                assertNotNull(dir + " " + key, string(strings, key));
            }
            // The list puts the choice in its summary, one place for it and
            // no other format marks
            String summary = string(strings, "summary_vr_environment");
            assertTrue(dir, summary.contains("%s"));
            assertEquals(dir, 1, summary.split("%", -1).length - 1);
            assertTrue(dir, String.format(summary, "x").contains("x"));
        }
        assertNull(string(english, "vr_environment_photo"));
    }

    private static File res(String path) {
        File f = new File("src/main/res/" + path);
        return f.isFile() ? f : new File("app/src/main/res/" + path);
    }

    private static org.w3c.dom.Document parse(String path) throws Exception {
        DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
        factory.setNamespaceAware(true);
        return factory.newDocumentBuilder().parse(res(path));
    }

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

    private static Element previousElement(Element e) {
        org.w3c.dom.Node n = e.getPreviousSibling();
        while (n != null && !(n instanceof Element)) {
            n = n.getPreviousSibling();
        }
        return (Element) n;
    }

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

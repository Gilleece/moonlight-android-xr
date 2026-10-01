package com.limelight.preferences;

import org.junit.Test;
import org.w3c.dom.Element;
import org.w3c.dom.NodeList;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import javax.xml.parsers.DocumentBuilderFactory;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

/** "Open Desktop automatically": its key, that it starts off, where it sits and its words. */
public class AutoDesktopPrefsTest {

    private static final String ANDROID = "http://schemas.android.com/apk/res/android";

    private static File res(String path) {
        File f = new File("src/main/res/" + path);
        return f.isFile() ? f : new File("app/src/main/res/" + path);
    }

    @Test
    public void itIsAnOffCheckboxAmongTheHostSettings() throws Exception {
        assertEquals("checkbox_auto_launch_desktop", PreferenceConfiguration.AUTO_DESKTOP_PREF_STRING);
        assertFalse(PreferenceConfiguration.DEFAULT_AUTO_DESKTOP);

        DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
        factory.setNamespaceAware(true);
        NodeList boxes = factory.newDocumentBuilder().parse(res("xml/preferences.xml"))
                .getElementsByTagName("CheckBoxPreference");
        Element found = null;
        for (int i = 0; i < boxes.getLength(); i++) {
            Element e = (Element) boxes.item(i);
            if (PreferenceConfiguration.AUTO_DESKTOP_PREF_STRING.equals(e.getAttributeNS(ANDROID, "key"))) {
                found = e;
            }
        }
        assertNotNull(found);
        assertEquals("false", found.getAttributeNS(ANDROID, "defaultValue"));
        Element category = (Element) found.getParentNode();
        assertEquals("@string/category_host_settings", category.getAttributeNS(ANDROID, "title"));
    }

    @Test
    public void itIsWordedInEveryLanguage() throws Exception {
        for (String dir : new String[] { "values", "values-fr", "values-zh-rCN", "values-zh-rTW" }) {
            String strings = new String(Files.readAllBytes(res(dir + "/strings.xml").toPath()),
                    StandardCharsets.UTF_8);
            // The app is looked for by that name, so every language says it
            for (String key : new String[] { "title_checkbox_auto_launch_desktop",
                    "summary_checkbox_auto_launch_desktop" }) {
                Matcher m = Pattern.compile("name=\"" + key + "\">([^<]*)<").matcher(strings);
                assertTrue(dir + " " + key, m.find());
                assertTrue(dir + " " + key, m.group(1).contains("Desktop"));
            }
        }
    }
}

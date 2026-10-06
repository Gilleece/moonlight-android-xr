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
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

/** "Check for updates": its key, that it starts on, where it sits and its words. */
public class CheckUpdatesPrefsTest {

    private static final String ANDROID = "http://schemas.android.com/apk/res/android";
    // English and the nineteen complete languages
    private static final String[] LANGUAGES = { "values", "values-fr", "values-zh-rCN",
            "values-zh-rTW", "values-de", "values-es", "values-it", "values-pt-rBR", "values-ja",
            "values-ko", "values-nl", "values-pl", "values-sv", "values-da", "values-nb-rNO",
            "values-fi", "values-cs", "values-tr", "values-ru", "values-uk" };

    private static File res(String path) {
        File f = new File("src/main/res/" + path);
        return f.isFile() ? f : new File("app/src/main/res/" + path);
    }

    private static String string(String dir, String name) throws Exception {
        String strings = new String(Files.readAllBytes(res(dir + "/strings.xml").toPath()),
                StandardCharsets.UTF_8);
        Matcher m = Pattern.compile("name=\"" + name + "\">([^<]+)<").matcher(strings);
        return m.find() ? m.group(1) : null;
    }

    @Test
    public void itIsAnOnCheckboxInAbout() throws Exception {
        assertEquals("checkbox_check_updates", PreferenceConfiguration.CHECK_UPDATES_PREF_STRING);
        assertTrue(PreferenceConfiguration.DEFAULT_CHECK_UPDATES);

        DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
        factory.setNamespaceAware(true);
        NodeList boxes = factory.newDocumentBuilder().parse(res("xml/preferences.xml"))
                .getElementsByTagName("CheckBoxPreference");
        Element found = null;
        for (int i = 0; i < boxes.getLength(); i++) {
            Element e = (Element) boxes.item(i);
            if (PreferenceConfiguration.CHECK_UPDATES_PREF_STRING.equals(
                    e.getAttributeNS(ANDROID, "key"))) {
                found = e;
            }
        }
        assertNotNull(found);
        assertEquals("true", found.getAttributeNS(ANDROID, "defaultValue"));
        assertEquals("@string/title_checkbox_check_updates", found.getAttributeNS(ANDROID, "title"));
        assertEquals("@string/summary_checkbox_check_updates",
                found.getAttributeNS(ANDROID, "summary"));
        Element category = (Element) found.getParentNode();
        assertEquals("category_about", category.getAttributeNS(ANDROID, "key"));
    }

    @Test
    public void itIsWordedInEveryLanguage() throws Exception {
        for (String dir : LANGUAGES) {
            for (String key : new String[] { "title_checkbox_check_updates",
                    "summary_checkbox_check_updates", "update_notice_view",
                    "update_notice_dismiss" }) {
                assertNotNull(dir + " " + key, string(dir, key));
            }
            // The tag goes in as it is, after the app's name
            String notice = String.format(string(dir, "update_notice"), "v0.4");
            assertTrue(dir, notice.contains("Moonlight XR v0.4"));
            assertTrue(dir, string(dir, "summary_checkbox_check_updates").contains("GitHub"));
        }
        assertEquals("Moonlight XR v0.4 is available",
                String.format(string("values", "update_notice"), "v0.4"));
    }
}

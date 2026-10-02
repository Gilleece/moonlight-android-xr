package com.limelight.preferences;

import org.junit.Test;
import org.w3c.dom.Element;
import org.w3c.dom.Node;
import org.w3c.dom.NodeList;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import javax.xml.parsers.DocumentBuilderFactory;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

/**
 * The About category that ends the settings: the version and commit, the
 * licence, the report and, last and quietly, Ko-fi, in every language.
 */
public class AboutPrefsTest {

    private static final String ANDROID = "http://schemas.android.com/apk/res/android";
    private static final String WEB = "com.limelight.preferences.WebLauncherPreference";
    private static final String[] LANGUAGES =
            { "values", "values-fr", "values-zh-rCN", "values-zh-rTW" };

    private static File file(String path) {
        File f = new File(path);
        return f.exists() ? f : new File("app/" + path);
    }

    private static File res(String path) {
        return file("src/main/res/" + path);
    }

    private static Element screen() throws Exception {
        DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
        factory.setNamespaceAware(true);
        return factory.newDocumentBuilder().parse(res("xml/preferences.xml")).getDocumentElement();
    }

    private static List<Element> children(Element parent) {
        List<Element> out = new ArrayList<>();
        NodeList nodes = parent.getChildNodes();
        for (int i = 0; i < nodes.getLength(); i++) {
            if (nodes.item(i).getNodeType() == Node.ELEMENT_NODE) {
                out.add((Element) nodes.item(i));
            }
        }
        return out;
    }

    private static String key(Element e) {
        return e.getAttributeNS(ANDROID, "key");
    }

    private static Element about() throws Exception {
        List<Element> categories = children(screen());
        return categories.get(categories.size() - 1);
    }

    private static String string(String dir, String name) throws Exception {
        String strings = new String(Files.readAllBytes(res(dir + "/strings.xml").toPath()),
                StandardCharsets.UTF_8);
        Matcher m = Pattern.compile("name=\"" + name + "\">([^<]+)<").matcher(strings);
        return m.find() ? m.group(1) : null;
    }

    @Test
    public void itIsTheLastCategoryWithItsFourRows() throws Exception {
        Element about = about();
        assertEquals("PreferenceCategory", about.getTagName());
        assertEquals("category_about", key(about));
        assertEquals("@string/category_about", about.getAttributeNS(ANDROID, "title"));
        List<Element> rows = children(about);
        assertEquals(4, rows.size());
        assertEquals("pref_about_version", key(rows.get(0)));
        assertEquals("pref_about_licence", key(rows.get(1)));
        assertEquals("pref_bug_report", key(rows.get(2)));
        assertEquals("pref_about_kofi", key(rows.get(3)));
    }

    @Test
    public void theVersionRowIsNotAButton() throws Exception {
        Element version = children(about()).get(0);
        assertEquals("Preference", version.getTagName());
        assertEquals("false", version.getAttributeNS(ANDROID, "selectable"));
        assertEquals("false", version.getAttributeNS(ANDROID, "persistent"));
        for (String dir : LANGUAGES) {
            String withCommit = string(dir, "about_version");
            assertNotNull(dir, withCommit);
            String line = String.format(withCommit, "12.1-xr0.3", "abc123");
            assertTrue(dir, line.startsWith("Moonlight XR 12.1-xr0.3"));
            assertTrue(dir, line.endsWith("abc123"));
            assertEquals(dir, "Moonlight XR 12.1-xr0.3",
                    String.format(string(dir, "about_version_no_commit"), "12.1-xr0.3"));
        }
        assertEquals("Moonlight XR 12.1-xr0.3, commit abc123",
                String.format(string("values", "about_version"), "12.1-xr0.3", "abc123"));
    }

    @Test
    public void theLicenceOpensOnGitHub() throws Exception {
        Element licence = children(about()).get(1);
        assertEquals(WEB, licence.getTagName());
        assertEquals("https://github.com/Gilleece/moonlight-android-xr/blob/master/LICENSE.txt",
                licence.getAttribute("url"));
        assertEquals("@string/title_about_licence", licence.getAttributeNS(ANDROID, "title"));
        assertTrue(file("../LICENSE.txt").isFile());
        assertEquals("Licence: GPLv3", string("values", "title_about_licence"));
    }

    @Test
    public void theReportMovedHereAndNowhereElse() throws Exception {
        Element report = children(about()).get(2);
        assertEquals("Preference", report.getTagName());
        assertEquals("@string/title_bug_report", report.getAttributeNS(ANDROID, "title"));
        int seen = 0;
        for (Element category : children(screen())) {
            for (Element row : children(category)) {
                seen += "pref_bug_report".equals(key(row)) ? 1 : 0;
            }
        }
        assertEquals(1, seen);
    }

    @Test
    public void koFiIsLastAndQuiet() throws Exception {
        Element kofi = children(about()).get(3);
        assertEquals(WEB, kofi.getTagName());
        // The handle the repository's funding file names
        String funding = new String(Files.readAllBytes(file("../.github/FUNDING.yml").toPath()),
                StandardCharsets.UTF_8);
        Matcher handle = Pattern.compile("(?m)^ko_fi:\\s*(\\S+)").matcher(funding);
        assertTrue(handle.find());
        assertEquals("https://ko-fi.com/" + handle.group(1), kofi.getAttribute("url"));
        assertEquals("https://ko-fi.com/moonlightxr", kofi.getAttribute("url"));
        // Plain text: no icon, no layout or widget of its own, nothing coloured
        assertEquals("", kofi.getAttributeNS(ANDROID, "icon"));
        assertEquals("", kofi.getAttributeNS(ANDROID, "layout"));
        assertEquals("", kofi.getAttributeNS(ANDROID, "widgetLayout"));
        for (String dir : LANGUAGES) {
            String title = string(dir, "title_about_kofi");
            String summary = string(dir, "summary_about_kofi");
            assertNotNull(dir, title);
            assertNotNull(dir, summary);
            assertFalse(dir, title.contains("<") || summary.contains("<"));
            assertTrue(dir, title.contains("Ko-fi"));
        }
        assertEquals("Support the project on Ko-fi", string("values", "title_about_kofi"));
        assertEquals("Optional. Moonlight XR is free and stays free.",
                string("values", "summary_about_kofi"));
    }

    @Test
    public void itIsWordedInEveryLanguage() throws Exception {
        for (String dir : LANGUAGES) {
            for (String name : new String[] { "category_about", "about_version",
                    "about_version_no_commit", "title_about_licence", "title_about_kofi",
                    "summary_about_kofi" }) {
                assertNotNull(dir + " " + name, string(dir, name));
            }
        }
    }
}

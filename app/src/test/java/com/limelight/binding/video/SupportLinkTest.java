package com.limelight.binding.video;

import org.junit.Test;
import org.w3c.dom.Element;
import org.w3c.dom.NodeList;

import java.io.File;

import javax.xml.parsers.DocumentBuilderFactory;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;

/** The About tab's Ko-fi button goes where the 2D settings' About row does. */
public class SupportLinkTest {

    private static final String ANDROID = "http://schemas.android.com/apk/res/android";

    private static File file(String path) {
        File f = new File(path);
        return f.exists() ? f : new File("app/" + path);
    }

    @Test
    public void itIsTheSamePageAsTheSettingsRow() throws Exception {
        DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
        factory.setNamespaceAware(true);
        NodeList rows = factory.newDocumentBuilder()
                .parse(file("src/main/res/xml/preferences.xml"))
                .getElementsByTagName("com.limelight.preferences.WebLauncherPreference");
        Element kofi = null;
        for (int i = 0; i < rows.getLength(); i++) {
            Element row = (Element) rows.item(i);
            if ("pref_about_kofi".equals(row.getAttributeNS(ANDROID, "key"))) {
                kofi = row;
            }
        }
        assertNotNull(kofi);
        assertEquals(kofi.getAttribute("url"), XrRenderer.SUPPORT_URL);
        assertEquals("https://ko-fi.com/moonlightxr", XrRenderer.SUPPORT_URL);
    }
}

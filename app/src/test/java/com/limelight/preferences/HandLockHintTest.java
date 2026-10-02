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
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

/**
 * The gesture is the only hand lock: no padlock setting, art or words are
 * left, and the hint that explains the gesture is worded in every language.
 */
public class HandLockHintTest {

    private static final String ANDROID = "http://schemas.android.com/apk/res/android";
    private static final String[] LANGUAGES =
            { "values", "values-fr", "values-zh-rCN", "values-zh-rTW" };
    // What the padlock was called in each, which the hand tracking row no
    // longer mentions
    private static final String[] PADLOCK_WORDS = { "padlock", "cadenas", "图标", "圖示" };

    private static File file(String path) {
        File f = new File(path);
        return f.exists() ? f : new File("app/" + path);
    }

    private static String string(String dir, String name) throws Exception {
        String strings = new String(Files.readAllBytes(
                file("src/main/res/" + dir + "/strings.xml").toPath()), StandardCharsets.UTF_8);
        Matcher m = Pattern.compile("name=\"" + name + "\">([^<]+)<").matcher(strings);
        return m.find() ? m.group(1) : null;
    }

    @Test
    public void thePadlockSettingIsGone() throws Exception {
        DocumentBuilderFactory factory = DocumentBuilderFactory.newInstance();
        factory.setNamespaceAware(true);
        NodeList boxes = factory.newDocumentBuilder()
                .parse(file("src/main/res/xml/preferences.xml"))
                .getElementsByTagName("CheckBoxPreference");
        boolean hands = false;
        for (int i = 0; i < boxes.getLength(); i++) {
            String key = ((Element) boxes.item(i)).getAttributeNS(ANDROID, "key");
            assertFalse(key, key.contains("hand_lock"));
            hands |= key.equals("checkbox_vr_hand_tracking");
        }
        assertTrue(hands);
        for (String dir : LANGUAGES) {
            assertNull(dir, string(dir, "title_vr_show_hand_lock"));
            assertNull(dir, string(dir, "summary_vr_show_hand_lock"));
        }
        assertFalse(file("src/main/assets/images/handtracking_locked.png").exists());
        assertFalse(file("src/main/assets/images/handtracking_unlocked.png").exists());
    }

    @Test
    public void theHandTrackingRowDescribesTheGesture() throws Exception {
        for (int i = 0; i < LANGUAGES.length; i++) {
            String summary = string(LANGUAGES[i], "summary_vr_hand_tracking");
            assertNotNull(LANGUAGES[i], summary);
            assertFalse(LANGUAGES[i], summary.contains(PADLOCK_WORDS[i]));
        }
        String english = string("values", "summary_vr_hand_tracking");
        assertTrue(english.contains("ring finger"));
        assertTrue(english.contains("the same gesture unlocks them"));
    }

    @Test
    public void theHintIsWordedInEveryLanguage() throws Exception {
        for (String dir : LANGUAGES) {
            for (String name : new String[] { "vr_hand_hint_title", "vr_hand_hint_body",
                    "vr_hand_hint_never" }) {
                assertNotNull(dir + " " + name, string(dir, name));
            }
        }
        assertEquals("Hand tracking is on.", string("values", "vr_hand_hint_title"));
        assertEquals("Touch your thumb to your ring finger for a moment to lock your hands out,"
                + " so a stray pinch does not click; the same gesture unlocks them.",
                string("values", "vr_hand_hint_body"));
        assertEquals("Don\\'t show this again", string("values", "vr_hand_hint_never"));
    }
}

package com.limelight.binding.video;

import org.junit.Test;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

/** The Ko-fi sheet in the session names the page the repository's funding file does. */
public class SupportLinkTest {

    private static File file(String path) {
        File f = new File(path);
        return f.exists() ? f : new File("app/" + path);
    }

    @Test
    public void itIsThePageTheFundingFileNames() throws Exception {
        String funding = new String(Files.readAllBytes(file("../.github/FUNDING.yml").toPath()),
                StandardCharsets.UTF_8);
        Matcher handle = Pattern.compile("(?m)^ko_fi:\\s*(\\S+)").matcher(funding);
        assertTrue(handle.find());
        assertEquals("https://ko-fi.com/" + handle.group(1), XrRenderer.SUPPORT_URL);
        assertEquals("https://ko-fi.com/moonlightxr", XrRenderer.SUPPORT_URL);
    }
}

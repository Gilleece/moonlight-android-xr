package com.limelight.utils;

import com.limelight.nvstream.http.HostHttpResponseException;
import com.limelight.nvstream.http.NvApp;

import org.junit.Test;
import org.xmlpull.v1.XmlPullParserException;

import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.fail;

/**
 * What a tap on a PC opens with "Open Desktop automatically" on: the app list
 * a host answers with, read the way the app list reads it, and the choice
 * between its Desktop app and the list.
 */
public class DesktopLaunchTest {

    // Shaped like Sunshine's answer, with its two stock apps
    private static final String SUNSHINE = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            + "<root status_code=\"200\">"
            + "<App><IsHdrSupported>1</IsHdrSupported><AppTitle>Desktop</AppTitle>"
            + "<ID>881448767</ID></App>"
            + "<App><IsHdrSupported>1</IsHdrSupported><AppTitle>Steam Big Picture</AppTitle>"
            + "<ID>1093255277</ID></App>"
            + "</root>";

    private static final String NO_DESKTOP = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            + "<root status_code=\"200\">"
            + "<App><IsHdrSupported>0</IsHdrSupported><AppTitle>Virtual Desktop</AppTitle>"
            + "<ID>12</ID></App>"
            + "<App><IsHdrSupported>0</IsHdrSupported><AppTitle>mstsc.exe</AppTitle>"
            + "<ID>13</ID></App>"
            + "</root>";

    private static final String REFUSED = "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            + "<root status_code=\"401\" status_message=\"The client is not authorized\"/>";

    @Test
    public void sunshinesDesktopIsFoundInItsAnswer() throws Exception {
        List<NvApp> apps = DesktopLaunch.parse(SUNSHINE);
        assertEquals(2, apps.size());
        NvApp desktop = DesktopLaunch.choose(apps, 0);
        assertNotNull(desktop);
        assertEquals("Desktop", desktop.getAppName());
        assertEquals(881448767, desktop.getAppId());
    }

    @Test
    public void aRunningDesktopIsResumed() throws Exception {
        NvApp desktop = DesktopLaunch.choose(DesktopLaunch.parse(SUNSHINE), 881448767);
        assertNotNull(desktop);
        assertEquals(881448767, desktop.getAppId());
    }

    @Test
    public void anotherRunningAppGoesToTheList() throws Exception {
        // Quitting it or going back to it is the list's to offer
        assertNull(DesktopLaunch.choose(DesktopLaunch.parse(SUNSHINE), 1093255277));
    }

    @Test
    public void noDesktopGoesToTheList() throws Exception {
        assertNull(DesktopLaunch.choose(DesktopLaunch.parse(NO_DESKTOP), 0));
        assertNull(DesktopLaunch.choose(new ArrayList<NvApp>(), 0));
        assertNull(DesktopLaunch.choose(null, 0));
    }

    @Test
    public void theNameIsMatchedWholeWhateverItsCase() {
        List<NvApp> apps = Arrays.asList(new NvApp("Virtual Desktop", 1, false),
                new NvApp("Desktop 2", 2, false), new NvApp(" DESKTOP ", 3, false),
                new NvApp("desktop", 4, false));
        // The first whole match, so a renamed copy further down is not picked
        assertEquals(3, DesktopLaunch.findDesktop(apps).getAppId());
        assertNull(DesktopLaunch.findDesktop(Arrays.asList(new NvApp("Desktops", 5, false))));
    }

    @Test
    public void aRefusedAnswerIsAnErrorNotAnEmptyList() throws Exception {
        // Which the caller turns into the app list, where the refusal is said
        try {
            DesktopLaunch.parse(REFUSED);
            fail("a 401 answer should not parse");
        } catch (HostHttpResponseException e) {
            assertEquals(401, e.getErrorCode());
        }
    }

    @Test
    public void aFailureIsLoggedWithoutTheUrl() {
        // NvHTTP's 404 carries the request it was for
        String url = "http://192.0.2.10:47989/applist?uniqueid=0123456789ABCDEF&uuid=abc";
        String logged = DesktopLaunch.describeFailure(new FileNotFoundException(url));
        assertEquals("FileNotFoundException, HTTP 404", logged);
        assertFalse(logged.contains("uniqueid"));
        assertFalse(logged.contains("192.0.2.10"));

        assertEquals("HostHttpResponseException, HTTP 503",
                DesktopLaunch.describeFailure(new HostHttpResponseException(503, "busy")));
        assertEquals("IOException",
                DesktopLaunch.describeFailure(new IOException("connect to 192.0.2.10 failed")));
        assertEquals("XmlPullParserException",
                DesktopLaunch.describeFailure(new XmlPullParserException("Malformed XML")));
    }
}

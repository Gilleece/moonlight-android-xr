package com.limelight.utils;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.zip.GZIPInputStream;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

/**
 * What a report says and how it goes: the text before the logs, the check on
 * the note and the address, the token kept out of the body, and the result
 * for each way a post can go, with a stand in for the network.
 */
public class BugReportTest {

    private static final String TOKEN = "token-that-must-stay-in-the-header";

    @Rule
    public TemporaryFolder folder = new TemporaryFolder();

    private static BugReport.Details details() {
        BugReport.Details d = new BugReport.Details();
        d.version = "12.1-xr0.3";
        d.packageName = "com.gilleece.moonlightxr";
        d.commit = "0123456789";
        d.manufacturer = "Oculus";
        d.brand = "oculus";
        d.model = "Quest 2";
        d.device = "codename";
        d.androidRelease = "12";
        d.sdk = 32;
        d.buildDisplay = "build-display";
        d.headset = true;
        d.runtime = "Oculus 1.2.3";
        d.settings = "res 3840x2160 fps 90\nvr true depthModel zipdepth environment 2\n";
        d.session = "environment Home Theater, 3d on, display 90 Hz";
        return d;
    }

    // A stand in for the network that keeps what it was handed
    private static final class Recorder implements BugReport.Transport {
        final List<String> urls = new ArrayList<>();
        Map<String, String> headers;
        byte[] body;
        int answer = 200;
        IOException failure;

        @Override
        public int post(String url, Map<String, String> headers, File body) throws IOException {
            urls.add(url);
            this.headers = headers;
            this.body = read(new FileInputStream(body));
            if (failure != null) {
                throw failure;
            }
            return answer;
        }
    }

    private static byte[] read(InputStream in) throws IOException {
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] chunk = new byte[4096];
            int n;
            while ((n = in.read(chunk)) > 0) {
                out.write(chunk, 0, n);
            }
            return out.toByteArray();
        } finally {
            in.close();
        }
    }

    private static String gunzip(byte[] packed) throws IOException {
        return new String(read(new GZIPInputStream(new java.io.ByteArrayInputStream(packed))),
                StandardCharsets.UTF_8);
    }

    private File log(String name, String text) throws IOException {
        File file = folder.newFile(name);
        FileOutputStream out = new FileOutputStream(file);
        try {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        } finally {
            out.close();
        }
        return file;
    }

    @Test
    public void theNoteAndEveryDetailAreThere() {
        String text = BugReport.compose("  the picture went black  ", "a@b.co", details());
        assertTrue(text.startsWith("Moonlight XR bug report\nFrom: a@b.co\n\nthe picture went black\n"));
        assertTrue(text.contains(
                "moonlight 12.1-xr0.3 com.gilleece.moonlightxr commit 0123456789\n"));
        assertTrue(text.contains("device Oculus oculus Quest 2 (codename)\n"));
        assertTrue(text.contains("android 12 sdk 32 build build-display\n"));
        assertTrue(text.contains("headset true\n"));
        assertTrue(text.contains("runtime Oculus 1.2.3\n"));
        assertTrue(text.contains("----- settings -----\nres 3840x2160 fps 90\n"));
        assertTrue(text.contains("depthModel zipdepth environment 2\n"));
        assertTrue(text.contains("session environment Home Theater, 3d on, display 90 Hz\n"));
        // The app and device come before the settings
        assertTrue(text.indexOf("----- app and device -----") < text.indexOf("----- settings -----"));
    }

    @Test
    public void whatIsNotKnownIsLeftOut() {
        BugReport.Details d = details();
        d.commit = "";
        d.runtime = null;
        d.session = null;
        d.root = true;
        d.debug = true;
        String text = BugReport.compose("   ", "", d);
        assertTrue(text.contains("From: \n\n(no message)\n"));
        assertTrue(text.contains("moonlight 12.1-xr0.3 com.gilleece.moonlightxr root debug\n"));
        assertFalse(text.contains("commit"));
        assertFalse(text.contains("runtime"));
        assertFalse(text.contains("session"));
        assertFalse(text.contains("VR start"));
    }

    @Test
    public void aFailedVrStartComesStraightAfterTheNote() {
        BugReport.Details d = details();
        d.startFailure = "VR session did not start, 2026-10-01 21:40:00\n"
                + "  stopped at: runtime (xrEnumerateInstanceExtensionProperties)\n"
                + "  result: XR_ERROR_RUNTIME_UNAVAILABLE (-51)\n";
        String text = BugReport.compose("no settings bar", "", d);
        assertTrue(text.startsWith("Moonlight XR bug report\nFrom: \n\nno settings bar\n"
                + "\n----- last VR start -----\n"
                + "VR session did not start, 2026-10-01 21:40:00\n"
                + "  stopped at: runtime (xrEnumerateInstanceExtensionProperties)\n"
                + "  result: XR_ERROR_RUNTIME_UNAVAILABLE (-51)\n"
                + "\n----- app and device -----\n"));
    }

    @Test
    public void theAddressCheckIsLight() {
        assertTrue(BugReport.looksLikeEmail("someone@example.com"));
        assertTrue(BugReport.looksLikeEmail("  first.last+tag@mail.example.co.uk "));
        assertFalse(BugReport.looksLikeEmail(""));
        assertFalse(BugReport.looksLikeEmail("someone"));
        assertFalse(BugReport.looksLikeEmail("@example.com"));
        assertFalse(BugReport.looksLikeEmail("someone@"));
        assertFalse(BugReport.looksLikeEmail("someone@example"));
        assertFalse(BugReport.looksLikeEmail("someone@.com"));
        assertFalse(BugReport.looksLikeEmail("someone@example."));
        assertFalse(BugReport.looksLikeEmail("some one@example.com"));
        assertFalse(BugReport.looksLikeEmail("a@b@example.com"));
        assertFalse(BugReport.looksLikeEmail(null));
    }

    @Test
    public void sendWaitsForANoteAndAnAddressThatCouldBeAnswered() {
        assertFalse(BugReport.canSend("", ""));
        assertFalse(BugReport.canSend("   \n ", ""));
        assertFalse(BugReport.canSend(null, null));
        assertTrue(BugReport.canSend("it froze", ""));
        assertTrue(BugReport.canSend("it froze", null));
        assertTrue(BugReport.canSend("it froze", "   "));
        assertTrue(BugReport.canSend("it froze", "a@b.co"));
        assertFalse(BugReport.canSend("it froze", "a@b"));
    }

    @Test
    public void theTokenGoesInTheHeaderAndNeverTheBody() throws IOException {
        File dir = folder.newFolder("reports");
        File current = log("moonlight.log", "10-01 19:00:00.000 I session start\n");
        String header = BugReport.compose("it froze", "a@b.co", details());
        assertFalse(header.contains(TOKEN));

        Recorder net = new Recorder();
        Map<String, String> headers = BugReport.headers(TOKEN, "Oculus Quest 2", "12.1-xr0.3",
                "a@b.co", "it froze\nafter a minute");
        BugReport.Outcome outcome = BugReport.fileReport(dir, header, new File[] { null, current },
                null, "https://collector.example/report", headers, net);

        assertEquals(BugReport.Result.SENT, outcome.result);
        assertEquals(1, net.urls.size());
        assertEquals(TOKEN, net.headers.get("X-Report-Token"));
        assertEquals("application/gzip", net.headers.get("Content-Type"));
        assertEquals("Oculus Quest 2", net.headers.get("X-Report-Device"));
        assertEquals("it froze / after a minute", net.headers.get("X-Report-Summary"));
        String body = gunzip(net.body);
        assertTrue(body.startsWith(header));
        assertTrue(body.contains("\n----- moonlight.log -----\n10-01 19:00:00.000 I session start\n"));
        assertFalse(body.contains(TOKEN));
        // The packed copy is gone once it has been sent
        File[] left = dir.listFiles();
        assertNotNull(left);
        assertEquals(1, left.length);
        assertTrue(left[0].getName().endsWith(".txt"));
    }

    @Test
    public void noTokenSendsNoTokenHeader() {
        Map<String, String> headers = BugReport.headers("", "d", "v", "", "m");
        assertFalse(headers.containsKey("X-Report-Token"));
    }

    @Test
    public void anEmptyUrlSavesInsteadOfPosting() throws IOException {
        File dir = folder.newFolder("reports");
        File visible = folder.newFolder("visible");
        Recorder net = new Recorder();
        BugReport.Outcome outcome = BugReport.fileReport(dir, "header\n", new File[0], visible, "",
                BugReport.headers(TOKEN, "d", "v", "", "m"), net);

        assertEquals(BugReport.Result.SAVED, outcome.result);
        assertTrue(net.urls.isEmpty());
        assertNull(outcome.detail);
        // Where it is said to be is the copy the file manager can see
        assertTrue(outcome.path.startsWith(visible.getAbsolutePath()));
        assertTrue(new File(outcome.path).isFile());
    }

    @Test
    public void aRefusalIsNotSentAndSaysWhy() throws IOException {
        File dir = folder.newFolder("reports");
        Recorder net = new Recorder();
        net.answer = 403;
        BugReport.Outcome outcome = BugReport.fileReport(dir, "header\n", new File[0], null,
                "https://collector.example/report", BugReport.headers("", "d", "v", "", "m"), net);

        assertEquals(BugReport.Result.NOT_SENT, outcome.result);
        assertEquals("server answered 403", outcome.detail);
        assertTrue(new File(outcome.path).isFile());
        assertTrue(outcome.path.startsWith(dir.getAbsolutePath()));
    }

    @Test
    public void noNetworkIsNotSentAndSaysWhy() throws IOException {
        File dir = folder.newFolder("reports");
        Recorder net = new Recorder();
        net.failure = new IOException("connect timed out");
        BugReport.Outcome outcome = BugReport.fileReport(dir, "header\n", new File[0], null,
                "https://collector.example/report", BugReport.headers("", "d", "v", "", "m"), net);

        assertEquals(BugReport.Result.NOT_SENT, outcome.result);
        assertEquals("connect timed out", outcome.detail);
        File[] left = dir.listFiles();
        assertNotNull(left);
        assertEquals(1, left.length);
    }

    @Test
    public void nowhereToWriteIsNotWritten() throws IOException {
        // A file where the folder should be
        File blocked = folder.newFile("reports");
        Recorder net = new Recorder();
        BugReport.Outcome outcome = BugReport.fileReport(blocked, "header\n", new File[0], null,
                "https://collector.example/report", BugReport.headers("", "d", "v", "", "m"), net);

        assertEquals(BugReport.Result.NOT_WRITTEN, outcome.result);
        assertNull(outcome.path);
        assertNotNull(outcome.detail);
        assertTrue(net.urls.isEmpty());
    }

    @Test
    public void bothLogsFollowInOrder() throws IOException {
        File dir = folder.newFolder("reports");
        File previous = log("moonlight.previous.log", "older\n");
        File current = log("moonlight.log", "newer\n");
        File report = BugReport.write(dir, "header\n", previous, null, current,
                new File(folder.getRoot(), "missing.log"));
        String text = new String(read(new FileInputStream(report)), StandardCharsets.UTF_8);
        assertEquals("header\n\n----- moonlight.previous.log -----\nolder\n"
                + "\n----- moonlight.log -----\nnewer\n", text);
        assertTrue(report.getName().startsWith("moonlight-xr-report-"));
    }

    @Test
    public void headersCarryOneLineOfPlainText() {
        assertEquals("caf? / ok", BugReport.headerSafe("café\r\nok", 120));
        assertEquals("abc", BugReport.headerSafe("abcdef", 3));
        assertEquals("", BugReport.headerSafe(null, 10));
    }

    @Test
    public void aPathIsShownFromDownloadOn() {
        assertEquals("Download/MoonlightXR/moonlight-xr-report-1.txt",
                BugReport.shortPath("/storage/emulated/0/Download/MoonlightXR/moonlight-xr-report-1.txt"));
        assertEquals("/data/user/0/x.txt", BugReport.shortPath("/data/user/0/x.txt"));
        assertNull(BugReport.shortPath(null));
    }
}

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
import java.util.Arrays;
import java.util.List;
import java.util.Map;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

/**
 * What a report says and how it goes: the text before the logs, the check on
 * the note and the address, the JSON the collector takes with the token kept
 * out of it, the logs cut to the newest 6 MB, the result for each way a post
 * can go with a stand in for the network, saved only where it does not go,
 * and the newest five kept.
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
        public int post(String url, Map<String, String> headers, byte[] body) throws IOException {
            urls.add(url);
            this.headers = headers;
            this.body = body;
            if (failure != null) {
                throw failure;
            }
            return answer;
        }

        String text() {
            return new String(body, StandardCharsets.UTF_8);
        }
    }

    private static final String URL = "https://collector.example/report";
    private static final String NAME = "moonlight-xr-report-20261002-201500.txt";

    // One string field of a JSON object as written by BugReport.body, unescaped,
    // read by hand since a regular expression overflows on a 6 MB log
    private static String field(String json, String key) {
        String start = "\"" + key + "\":";
        int at = json.indexOf(start);
        assertTrue(key, at >= 0);
        at += start.length();
        if (json.startsWith("null", at)) {
            return null;
        }
        assertEquals('"', json.charAt(at));
        StringBuilder out = new StringBuilder();
        for (int i = at + 1; ; i++) {
            char c = json.charAt(i);
            if (c == '"') {
                return out.toString();
            }
            if (c != '\\') {
                out.append(c);
                continue;
            }
            char e = json.charAt(++i);
            switch (e) {
                case 'n': out.append('\n'); break;
                case 'r': out.append('\r'); break;
                case 't': out.append('\t'); break;
                case 'u':
                    out.append((char) Integer.parseInt(json.substring(i + 1, i + 5), 16));
                    i += 4;
                    break;
                default: out.append(e); break;
            }
        }
    }

    private BugReport.Outcome sendOrSave(File dir, String summary, File[] logs, String url,
                                         BugReport.Transport net) {
        return BugReport.sendOrSave(dir, NAME, summary, "it froze", "a@b.co", logs, url, TOKEN,
                net);
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
        assertTrue(text.startsWith("Moonlight XR bug report\nVersion: 12.1-xr0.3\nFrom: a@b.co\n\n"
                + "the picture went black\n"));
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
        assertTrue(text.startsWith("Moonlight XR bug report\nVersion: 12.1-xr0.3\nFrom: \n\n"
                + "no settings bar\n"
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
    public void theReportGoesAsJsonWithTheTokenInTheHeader() throws IOException {
        File dir = folder.newFolder("reports");
        File current = log("moonlight.log", "10-01 19:00:00.000 I session start \"quoted\"\n");
        String summary = BugReport.compose("it froze", "a@b.co", details());
        assertFalse(summary.contains(TOKEN));

        Recorder net = new Recorder();
        BugReport.Outcome outcome = BugReport.sendOrSave(dir, NAME, summary,
                "it froze\nafter a minute", " a@b.co ", new File[] { null, current }, URL, TOKEN,
                net);

        assertEquals(BugReport.Result.SENT, outcome.result);
        assertNull(outcome.path);
        assertEquals(Arrays.asList(URL), net.urls);
        assertEquals("Bearer " + TOKEN, net.headers.get("Authorization"));
        assertEquals("application/json", net.headers.get("Content-Type"));
        assertEquals(2, net.headers.size());

        String json = net.text();
        assertTrue(json.startsWith("{\"note\":"));
        assertTrue(json.endsWith("}"));
        assertEquals("it froze\nafter a minute", field(json, "note"));
        assertEquals("a@b.co", field(json, "replyTo"));
        assertEquals(summary, field(json, "summary"));
        assertTrue(field(json, "summary").startsWith("Moonlight XR bug report\nVersion: "));
        assertEquals("\n----- moonlight.log -----\n10-01 19:00:00.000 I session start \"quoted\"\n",
                field(json, "log"));
        assertEquals(NAME, field(json, "fileName"));
        assertFalse(json.contains(TOKEN));
        // Sent is the end of it: nothing is kept on the headset
        String[] left = dir.list();
        assertNotNull(left);
        assertEquals(0, left.length);
    }

    @Test
    public void noAddressIsNullAndNoTokenSendsNoAuthorization() throws IOException {
        Recorder net = new Recorder();
        BugReport.sendOrSave(folder.newFolder("r"), NAME, "Moonlight XR bug report\n", "x", "  ",
                new File[0], URL, "", net);
        assertTrue(net.text().contains("\"replyTo\":null,"));
        assertNull(field(net.text(), "replyTo"));
        assertEquals("", field(net.text(), "log"));
        assertFalse(net.headers.containsKey("Authorization"));
    }

    @Test
    public void theNoteAndSummaryAreHeldToTheirLimits() {
        StringBuilder note = new StringBuilder();
        for (int i = 0; i < 5000; i++) {
            note.append('n');
        }
        StringBuilder summary = new StringBuilder(BugReport.HEADER);
        while (summary.length() < 9000) {
            summary.append("settings line\n");
        }
        String json = new String(BugReport.body(note.toString(), null, summary.toString(), "",
                NAME), StandardCharsets.UTF_8);
        assertEquals(BugReport.NOTE_MAX, field(json, "note").length());
        assertEquals(BugReport.SUMMARY_MAX, field(json, "summary").length());
        // A character is never split at the limit
        String emoji = "\uD83D\uDE00";
        String clipped = BugReport.clip(note.substring(0, 3999) + emoji, 4000);
        assertEquals(3999, clipped.length());
    }

    @Test
    public void controlCharactersAndSeparatorsAreEscaped() {
        String json = new String(BugReport.body("tab\there \u0001 back\\slash", null,
                "line\u2028sep\r\n", "caf\u00e9 \u65e5\u672c", NAME), StandardCharsets.UTF_8);
        assertTrue(json.contains("tab\\there \\u0001 back\\\\slash"));
        assertTrue(json.contains("line\\u2028sep\\r\\n"));
        // Anything else goes as it is, in UTF-8
        assertTrue(json.contains("caf\u00e9 \u65e5\u672c"));
        assertEquals("tab\there \u0001 back\\slash", field(json, "note"));
    }

    @Test
    public void logsUnderTheLimitGoWholeOldestFirst() throws IOException {
        File previous = log("moonlight.previous.log", "older\n");
        File current = log("moonlight.log", "newer\n");
        assertEquals("\n----- moonlight.previous.log -----\nolder\n\n----- moonlight.log -----\nnewer\n",
                BugReport.logText(new File[] { previous, null, current }, 1000));
    }

    @Test
    public void logsOverTheLimitKeepTheirNewestWholeLines() throws IOException {
        // Two logs of numbered lines, 6300 bytes and 2700, cut to 4000
        StringBuilder older = new StringBuilder();
        for (int i = 0; i < 700; i++) {
            older.append(String.format("old %04d\n", i));
        }
        StringBuilder newer = new StringBuilder();
        for (int i = 0; i < 300; i++) {
            newer.append(String.format("new %04d\n", i));
        }
        File previous = log("moonlight.previous.log", older.toString());
        File current = log("moonlight.log", newer.toString());
        String text = BugReport.logText(new File[] { previous, current }, 4000);

        assertTrue(text.length() <= 4000);
        assertTrue(text, text.startsWith(
                "----- cut to the newest 4000 bytes of the logs, the oldest "));
        // The current log whole, under its line, at the end
        assertTrue(text.endsWith("\n----- moonlight.log -----\n" + newer));
        // The older one from a whole line on, to its last line
        int title = text.indexOf("\n----- moonlight.previous.log -----\n");
        assertTrue(title > 0);
        String kept = text.substring(title + "\n----- moonlight.previous.log -----\n".length(),
                text.indexOf("\n----- moonlight.log -----\n"));
        assertTrue(kept, kept.matches("(old \\d{4}\n)+"));
        assertTrue(kept.endsWith("old 0699\n"));
        // And says how much it left out, which with what it kept is the whole
        long leftOut = Long.parseLong(text.replaceAll("(?s)^.*the oldest (\\d+) bytes.*$", "$1"));
        assertEquals(older.length(), leftOut + kept.length());

        // Past the limit with the current log alone, the older one goes whole
        text = BugReport.logText(new File[] { previous, current }, 2000);
        assertFalse(text.contains("moonlight.previous.log"));
        assertTrue(text.contains("\n----- moonlight.log -----\nnew "));
        assertTrue(text.endsWith("new 0299\n"));
        assertTrue(text.length() <= 2000);
    }

    @Test
    public void twoFullLogsAreCutToSixMegabytes() throws IOException {
        // Two 5 MB logs, as the log's roll over leaves them at worst
        StringBuilder line = new StringBuilder();
        while (line.length() < 99) {
            line.append('x');
        }
        line.append('\n');
        StringBuilder five = new StringBuilder(5 * 1024 * 1024 + 100);
        while (five.length() < 5 * 1024 * 1024) {
            five.append(line);
        }
        File previous = log("moonlight.previous.log", five.toString());
        File current = log("moonlight.log", five.toString());
        String text = BugReport.logText(new File[] { previous, current }, BugReport.LOG_MAX_BYTES);
        int bytes = text.getBytes(StandardCharsets.UTF_8).length;
        assertTrue(String.valueOf(bytes), bytes <= BugReport.LOG_MAX_BYTES);
        assertTrue(bytes > BugReport.LOG_MAX_BYTES - 1000);
        assertTrue(text.startsWith("----- cut to the newest 6 MB of the logs, the oldest "));
        byte[] body = BugReport.body("n", null, "s", text, NAME);
        assertTrue(body.length < BugReport.BODY_MAX_BYTES);
    }

    @Test
    public void aBodyEscapedPastEightMegabytesIsCutFurther() {
        // Six megabytes of tabs, each escaped to two bytes
        StringBuilder tabs = new StringBuilder(BugReport.LOG_MAX_BYTES + 100);
        while (tabs.length() < BugReport.LOG_MAX_BYTES - 200) {
            tabs.append("\t\t\t\t\t\t\t\t\t\n");
        }
        byte[] body = BugReport.body("n", null, "s", tabs.toString(), NAME);
        assertTrue(body.length <= BugReport.BODY_MAX_BYTES);
        String log = field(new String(body, StandardCharsets.UTF_8), "log");
        assertTrue(log.startsWith("----- cut further to fit the collector -----\n"));
    }

    @Test
    public void anEmptyUrlSavesAndSaysThereIsNoCollector() throws IOException {
        File dir = folder.newFolder("logs");
        Recorder net = new Recorder();
        BugReport.Outcome outcome = sendOrSave(dir, "header\n", new File[0], "", net);

        assertEquals(BugReport.Result.NO_COLLECTOR, outcome.result);
        assertTrue(net.urls.isEmpty());
        assertNull(outcome.detail);
        // Saved once, where it is said to be and nowhere else
        assertEquals(new File(dir, NAME).getAbsolutePath(), outcome.path);
        assertTrue(new File(outcome.path).isFile());
        File[] saved = dir.listFiles();
        assertNotNull(saved);
        assertEquals(1, saved.length);
    }

    @Test
    public void onlyAnHttpsCollectorIsUsed() throws IOException {
        assertEquals("https://collector.example/report",
                BugReport.usableUrl(" https://collector.example/report "));
        assertEquals("HTTPS://collector.example/report",
                BugReport.usableUrl("HTTPS://collector.example/report"));
        assertEquals("", BugReport.usableUrl("http://collector.example/report"));
        assertEquals("", BugReport.usableUrl("collector.example/report"));
        assertEquals("", BugReport.usableUrl(""));
        assertEquals("", BugReport.usableUrl(null));

        // A plain http collector is treated as none: saved, never posted
        File dir = folder.newFolder("reports");
        Recorder net = new Recorder();
        BugReport.Outcome outcome = sendOrSave(dir, "header\n", new File[0],
                "http://collector.example/report", net);
        assertEquals(BugReport.Result.NO_COLLECTOR, outcome.result);
        assertTrue(net.urls.isEmpty());
    }

    @Test
    public void aRefusalIsSavedAndSaysWhy() throws IOException {
        File dir = folder.newFolder("reports");
        File current = log("moonlight.log", "a line\n");
        for (int code : new int[] { 401, 400, 500, 502 }) {
            Recorder net = new Recorder();
            net.answer = code;
            BugReport.Outcome outcome = sendOrSave(dir, "header\n", new File[] { current }, URL,
                    net);

            assertEquals(BugReport.Result.NOT_SENT, outcome.result);
            assertEquals("server answered " + code, outcome.detail);
            // The saved copy is the whole report, as the file always was
            File saved = new File(outcome.path);
            assertTrue(saved.isFile());
            assertTrue(outcome.path.startsWith(dir.getAbsolutePath()));
            assertEquals("header\n\n----- moonlight.log -----\na line\n",
                    new String(read(new FileInputStream(saved)), StandardCharsets.UTF_8));
        }
    }

    @Test
    public void aCollectorAtItsLimitsIsBusyNotFailed() throws IOException {
        File dir = folder.newFolder("reports");
        for (int code : new int[] { 429, 413 }) {
            Recorder net = new Recorder();
            net.answer = code;
            BugReport.Outcome outcome = sendOrSave(dir, "header\n", new File[0], URL, net);

            assertEquals(String.valueOf(code), BugReport.Result.BUSY, outcome.result);
            assertEquals("server answered " + code, outcome.detail);
            assertTrue(new File(outcome.path).isFile());
        }
        // Any 2xx is taken
        Recorder net = new Recorder();
        net.answer = 204;
        assertEquals(BugReport.Result.SENT, sendOrSave(dir, "header\n", new File[0], URL, net)
                .result);
    }

    @Test
    public void theCollectorLooksForTheSameFirstLine() throws IOException {
        assertTrue(BugReport.compose("x", "", details()).startsWith(BugReport.HEADER));
        // The worker sits outside the app, so the test is what keeps the two alike
        String worker = new String(read(new FileInputStream(
                new File("../tools/report-worker/worker.js"))), StandardCharsets.UTF_8);
        String quoted = BugReport.HEADER.replace("\n", "\\n");
        assertTrue(worker.contains("const REPORT_HEADER = '" + quoted + "';"));
    }

    @Test
    public void noNetworkIsSavedAndSaysWhy() throws IOException {
        File dir = folder.newFolder("reports");
        Recorder net = new Recorder();
        net.failure = new IOException("connect timed out");
        BugReport.Outcome outcome = sendOrSave(dir, "header\n", new File[0], URL, net);

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
        net.answer = 502;
        BugReport.Outcome outcome = sendOrSave(blocked, "header\n", new File[0], URL, net);

        assertEquals(BugReport.Result.NOT_WRITTEN, outcome.result);
        assertNull(outcome.path);
        assertTrue(outcome.detail, outcome.detail.startsWith("server answered 502; "));

        // Sent, it never needed the folder
        net.answer = 200;
        assertEquals(BugReport.Result.SENT, sendOrSave(blocked, "header\n", new File[0], URL, net)
                .result);
        // With no collector there is nothing but the folder
        assertEquals(BugReport.Result.NOT_WRITTEN, sendOrSave(blocked, "header\n", new File[0], "",
                net).result);
    }

    @Test
    public void bothLogsFollowInOrder() throws IOException {
        File dir = folder.newFolder("reports");
        File previous = log("moonlight.previous.log", "older\n");
        File current = log("moonlight.log", "newer\n");
        File report = BugReport.write(dir, NAME, "header\n", previous, null, current,
                new File(folder.getRoot(), "missing.log"));
        String text = new String(read(new FileInputStream(report)), StandardCharsets.UTF_8);
        assertEquals("header\n\n----- moonlight.previous.log -----\nolder\n"
                + "\n----- moonlight.log -----\nnewer\n", text);
        assertEquals(NAME, report.getName());
        assertTrue(BugReport.isReportName(BugReport.reportName(new java.util.Date())));
    }

    // A report by the name it would have been given at that time
    private static File report(File dir, String stamp) throws IOException {
        File file = new File(dir, "moonlight-xr-report-" + stamp + ".txt");
        assertTrue(file.createNewFile());
        return file;
    }

    @Test
    public void onlyTheNewestFiveReportsAreKept() throws IOException {
        File dir = folder.newFolder("logs");
        // Seven reports, made out of order, among the logs and a packed copy
        // a post left behind
        String[] stamps = { "20261001-120000", "20260930-235959", "20261002-090000",
                "20260101-000000", "20261001-120001", "20261002-235959", "20261002-000000" };
        for (String stamp : stamps) {
            report(dir, stamp);
        }
        File current = new File(dir, "moonlight.log");
        File previous = new File(dir, "moonlight.previous.log");
        File packed = new File(dir, "moonlight-xr-report-20250101-000000.txt.gz");
        File other = new File(dir, "notes.txt");
        assertTrue(current.createNewFile() && previous.createNewFile() && packed.createNewFile()
                && other.createNewFile());

        assertEquals(2, BugReport.prune(dir, BugReport.KEEP_REPORTS));
        // The two oldest went, by the time in their names
        assertFalse(new File(dir, "moonlight-xr-report-20260101-000000.txt").exists());
        assertFalse(new File(dir, "moonlight-xr-report-20260930-235959.txt").exists());
        for (String stamp : new String[] { "20261001-120000", "20261001-120001",
                "20261002-000000", "20261002-090000", "20261002-235959" }) {
            assertTrue(stamp, new File(dir, "moonlight-xr-report-" + stamp + ".txt").isFile());
        }
        // Nothing else in the folder is touched
        assertTrue(current.isFile() && previous.isFile() && packed.isFile() && other.isFile());

        // Five or fewer is left alone, and an empty or missing folder is fine
        assertEquals(0, BugReport.prune(dir, BugReport.KEEP_REPORTS));
        assertEquals(0, BugReport.prune(folder.newFolder("empty"), BugReport.KEEP_REPORTS));
        assertEquals(0, BugReport.prune(new File(folder.getRoot(), "missing"),
                BugReport.KEEP_REPORTS));
    }

    @Test
    public void savingPrunesWhatWasThere() throws IOException {
        File dir = folder.newFolder("logs");
        for (int i = 0; i < 6; i++) {
            report(dir, "2020010" + (i + 1) + "-000000");
        }
        BugReport.Outcome outcome = sendOrSave(dir, "header\n", new File[0], "", new Recorder());

        assertEquals(BugReport.Result.NO_COLLECTOR, outcome.result);
        // The new one and the four newest before it
        File[] left = dir.listFiles();
        assertNotNull(left);
        assertEquals(5, left.length);
        assertTrue(new File(outcome.path).isFile());
        assertFalse(new File(dir, "moonlight-xr-report-20200101-000000.txt").exists());
        assertFalse(new File(dir, "moonlight-xr-report-20200102-000000.txt").exists());
        assertTrue(new File(dir, "moonlight-xr-report-20200103-000000.txt").isFile());
    }

    @Test
    public void onlyReportsCountAsReports() {
        assertTrue(BugReport.isReportName("moonlight-xr-report-20261002-090000.txt"));
        assertFalse(BugReport.isReportName("moonlight.log"));
        assertFalse(BugReport.isReportName("moonlight.previous.log"));
        assertFalse(BugReport.isReportName("moonlight-xr-report-20261002-090000.txt.gz"));
        assertFalse(BugReport.isReportName(null));
    }

    @Test
    public void aPathIsShownFromDownloadOn() {
        assertEquals("Download/MoonlightXR/moonlight-xr-report-1.txt",
                BugReport.shortPath("/storage/emulated/0/Download/MoonlightXR/moonlight-xr-report-1.txt"));
        assertEquals("/data/user/0/x.txt", BugReport.shortPath("/data/user/0/x.txt"));
        assertNull(BugReport.shortPath(null));
    }
}

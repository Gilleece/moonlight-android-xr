package com.limelight.binding.video;

import com.limelight.R;

import org.junit.Test;
import org.w3c.dom.Element;
import org.w3c.dom.NodeList;

import java.io.File;
import java.util.HashMap;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import javax.xml.parsers.DocumentBuilderFactory;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

/**
 * What a failed VR start says: the one line the flat panel shows, read
 * through the real English strings, and the block the log and a report
 * carry.
 */
public class XrStartFailureTest {

    private static final String[] LOCALES = { "values", "values-fr", "values-zh-rCN", "values-zh-rTW" };

    private static File res(String path) {
        File f = new File("src/main/res/" + path);
        return f.isFile() ? f : new File("app/src/main/res/" + path);
    }

    // Every string in a strings.xml, unescaped the way Android reads them
    private static Map<String, String> strings(String dir) throws Exception {
        NodeList nodes = DocumentBuilderFactory.newInstance().newDocumentBuilder()
                .parse(res(dir + "/strings.xml")).getElementsByTagName("string");
        Map<String, String> out = new HashMap<>();
        for (int i = 0; i < nodes.getLength(); i++) {
            Element e = (Element) nodes.item(i);
            out.put(e.getAttribute("name"), e.getTextContent()
                    .replace("\\n", "\n").replace("\\'", "'").replace("\\\"", "\""));
        }
        return out;
    }

    private static int stringId(String name) throws Exception {
        return R.string.class.getField(name).getInt(null);
    }

    // The flat panel's reason as an English speaker sees it, checking on the
    // way that it comes from the string expected
    private static String reason(XrStartFailure f, String expected) throws Exception {
        assertEquals(expected, stringId(expected), f.reasonRes());
        String template = strings("values").get(expected);
        assertNotNull(expected, template);
        String arg = f.reasonArg();
        return arg != null ? String.format(template, arg) : template;
    }

    private static XrStartFailure failure(String step, String call, int result, String name,
                                          String detail) {
        return new XrStartFailure(step, call, result, name, detail, null, new String[0],
                new String[0]);
    }

    @Test
    public void noRuntimeIsSaidPlainly() throws Exception {
        assertEquals("No OpenXR runtime answered", reason(failure("runtime",
                "xrEnumerateInstanceExtensionProperties", -51, "XR_ERROR_RUNTIME_UNAVAILABLE", ""),
                "vr_fail_no_runtime"));
        // A loader that could not start comes to the same thing for the user
        assertEquals("No OpenXR runtime answered", reason(failure("loader",
                "xrInitializeLoaderKHR", -2, "XR_ERROR_RUNTIME_FAILURE", ""),
                "vr_fail_no_runtime"));
        // As does a runtime that answered with nothing
        assertEquals("No OpenXR runtime answered", reason(failure("runtime",
                "xrEnumerateInstanceExtensionProperties", 0, "", "the runtime offered no extensions"),
                "vr_fail_no_runtime"));
    }

    @Test
    public void aRefusalNamesTheStepAndTheAnswer() throws Exception {
        assertEquals("The runtime refused the session (XR_ERROR_FORM_FACTOR_UNAVAILABLE)",
                reason(failure("session", "xrCreateSession", -35,
                        "XR_ERROR_FORM_FACTOR_UNAVAILABLE", ""), "vr_fail_session"));
        assertEquals("The runtime refused the instance (XR_ERROR_API_VERSION_UNSUPPORTED)",
                reason(failure("instance", "xrCreateInstance", -4,
                        "XR_ERROR_API_VERSION_UNSUPPORTED", ""), "vr_fail_instance"));
        assertEquals("The runtime found no headset ready (XR_ERROR_FORM_FACTOR_UNAVAILABLE)",
                reason(failure("system", "xrGetSystem", -35,
                        "XR_ERROR_FORM_FACTOR_UNAVAILABLE", ""), "vr_fail_system"));
        assertEquals("The runtime refused a tracking space (XR_ERROR_RUNTIME_FAILURE)",
                reason(failure("space", "create local space", -2, "XR_ERROR_RUNTIME_FAILURE", ""),
                        "vr_fail_space"));
        assertEquals("The runtime refused OpenGL ES (XR_ERROR_RUNTIME_FAILURE)",
                reason(failure("graphics", "get gles requirements", -2,
                        "XR_ERROR_RUNTIME_FAILURE", ""), "vr_fail_graphics"));
        // With no answer to give, what was said about it instead
        assertEquals("The runtime refused the video swapchain (no formats offered)",
                reason(failure("swapchain", "xrEnumerateSwapchainFormats", 0, "",
                        "no formats offered"), "vr_fail_swapchain"));
        // And a number the native side had no name for is still a number
        assertEquals("The runtime refused the session (-1000)",
                reason(failure("session", "xrCreateSession", -1000, "", ""), "vr_fail_session"));
    }

    @Test
    public void missingExtensionsAreNamedFromWhatWasOffered() throws Exception {
        XrStartFailure f = new XrStartFailure("extensions", "", 0, "",
                "missing XR_KHR_opengl_es_enable", null,
                new String[] { "XR_KHR_vulkan_enable2", "XR_KHR_android_create_instance" },
                new String[0]);
        assertEquals("The OpenXR runtime does not offer XR_KHR_opengl_es_enable",
                reason(f, "vr_fail_extensions"));

        XrStartFailure neither = new XrStartFailure("extensions", "", 0, "", "", null,
                new String[] { "XR_KHR_vulkan_enable2" }, new String[0]);
        assertEquals("The OpenXR runtime does not offer XR_KHR_opengl_es_enable, "
                + "XR_KHR_android_create_instance", reason(neither, "vr_fail_extensions"));
    }

    @Test
    public void theRestHaveTheirOwnLines() throws Exception {
        assertEquals("The headset's graphics driver failed (eglCreateContext, EGL error 0x3003)",
                reason(failure("egl", "eglCreateContext", 0, "", "EGL error 0x3003"),
                        "vr_fail_egl"));
        assertEquals("The renderer's shaders did not build",
                reason(failure("gl", "initGl", 0, "", ""), "vr_fail_gl"));
        assertEquals("The runtime did not answer within 5 seconds",
                reason(XrStartFailure.timedOut(5), "vr_fail_timeout"));
        assertEquals("The VR session could not start",
                reason(XrStartFailure.unknown(), "vr_fail_unknown"));
        // A step this side has never heard of
        assertEquals("The VR session could not start",
                reason(failure("frobnicate", "", -2, "XR_ERROR_RUNTIME_FAILURE", ""),
                        "vr_fail_unknown"));
    }

    @Test
    public void theFlatPanelPutsTheReasonFirst() throws Exception {
        String notice = String.format(strings("values").get("vr_unavailable_flat_reason"),
                reason(failure("session", "xrCreateSession", -35,
                        "XR_ERROR_FORM_FACTOR_UNAVAILABLE", ""), "vr_fail_session"));
        String[] lines = notice.split("\n");
        assertEquals(2, lines.length);
        assertEquals("The runtime refused the session (XR_ERROR_FORM_FACTOR_UNAVAILABLE)", lines[0]);
        assertTrue(lines[1].startsWith("The stream is showing on a flat panel."));
    }

    @Test
    public void everyLanguageTakesTheSameArguments() throws Exception {
        Pattern arg = Pattern.compile("%(\\d+)\\$s");
        Map<String, String> english = strings("values");
        for (String locale : LOCALES) {
            Map<String, String> s = strings(locale);
            for (Map.Entry<String, String> e : english.entrySet()) {
                String name = e.getKey();
                if (!name.startsWith("vr_fail_") && !name.equals("vr_unavailable_flat_reason")) {
                    continue;
                }
                String text = s.get(name);
                assertNotNull(locale + " " + name, text);
                Matcher want = arg.matcher(e.getValue());
                Matcher have = arg.matcher(text);
                assertEquals(locale + " " + name, want.find(), have.find());
            }
        }
    }

    @Test
    public void readsWhatTheNativeStartHandsOver() {
        String[] fields = new String[XrStartFailure.FIELDS];
        fields[XrStartFailure.FIELD_STEP] = "session";
        fields[XrStartFailure.FIELD_CALL] = "xrCreateSession";
        fields[XrStartFailure.FIELD_RESULT] = "-35";
        fields[XrStartFailure.FIELD_RESULT_NAME] = "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
        fields[XrStartFailure.FIELD_DETAIL] = "";
        fields[XrStartFailure.FIELD_RUNTIME] = "Some Runtime 1.2.3";
        fields[XrStartFailure.FIELD_OFFERED] = "XR_KHR_opengl_es_enable XR_KHR_android_create_instance"
                + " XR_EXT_hand_tracking";
        fields[XrStartFailure.FIELD_REQUESTED] = "XR_KHR_opengl_es_enable XR_KHR_android_create_instance";

        XrStartFailure f = XrStartFailure.fromNative(fields);
        assertEquals("session", f.step);
        assertEquals(-35, f.result);
        assertEquals("Some Runtime 1.2.3", f.runtime);
        assertEquals(3, f.offered.length);
        assertEquals(2, f.requested.length);
        assertTrue(f.missing().isEmpty());

        String block = f.block(null);
        String[] lines = block.split("\n");
        assertEquals("VR session did not start", lines[0]);
        assertEquals("  stopped at: session (xrCreateSession)", lines[1]);
        assertEquals("  result: XR_ERROR_FORM_FACTOR_UNAVAILABLE (-35)", lines[2]);
        assertEquals("  runtime: Some Runtime 1.2.3", lines[3]);
        assertEquals("  asked for 2: XR_KHR_opengl_es_enable XR_KHR_android_create_instance",
                lines[4]);
        assertEquals("  offered 3: XR_KHR_opengl_es_enable XR_KHR_android_create_instance"
                + " XR_EXT_hand_tracking", lines[5]);
        assertEquals(6, lines.length);
        assertTrue(f.block("2026-10-01 21:40:00").startsWith(
                "VR session did not start, 2026-10-01 21:40:00\n"));
    }

    @Test
    public void aStartThatNeverReachedTheRuntimeSaysSo() {
        String[] fields = new String[XrStartFailure.FIELDS];
        fields[XrStartFailure.FIELD_STEP] = "runtime";
        fields[XrStartFailure.FIELD_CALL] = "xrEnumerateInstanceExtensionProperties";
        fields[XrStartFailure.FIELD_RESULT] = "-51";
        fields[XrStartFailure.FIELD_RESULT_NAME] = "XR_ERROR_RUNTIME_UNAVAILABLE";
        fields[XrStartFailure.FIELD_DETAIL] = "";
        fields[XrStartFailure.FIELD_RUNTIME] = "";
        fields[XrStartFailure.FIELD_OFFERED] = "";
        fields[XrStartFailure.FIELD_REQUESTED] = "";

        String block = XrStartFailure.fromNative(fields).block(null);
        assertEquals("VR session did not start\n"
                + "  stopped at: runtime (xrEnumerateInstanceExtensionProperties)\n"
                + "  result: XR_ERROR_RUNTIME_UNAVAILABLE (-51)\n"
                + "  runtime: none, no instance was made\n"
                + "  offered: nothing\n", block);

        // Not even that much is known after a wait that ran out
        String waited = XrStartFailure.timedOut(5).block(null);
        assertTrue(waited.contains("  stopped at: timeout\n"));
        assertTrue(waited.contains("  detail: no answer within 5 s\n"));
        assertTrue(waited.contains("  offered: not known\n"));
        assertFalse(waited.contains("result:"));
    }

    @Test
    public void nothingUsableFromNativeIsAnUnknownStop() {
        assertEquals("unknown", XrStartFailure.fromNative(null).step);
        assertEquals("unknown", XrStartFailure.fromNative(new String[3]).step);
        assertNull(XrStartFailure.fromNative(null).reasonArg());

        String[] badNumber = new String[XrStartFailure.FIELDS];
        badNumber[XrStartFailure.FIELD_STEP] = "session";
        badNumber[XrStartFailure.FIELD_RESULT] = "x";
        XrStartFailure f = XrStartFailure.fromNative(badNumber);
        assertEquals(0, f.result);
        assertEquals(0, f.offered.length);
    }
}

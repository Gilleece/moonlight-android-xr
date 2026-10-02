package com.limelight.utils;

import android.content.SharedPreferences;

import com.limelight.preferences.PreferenceConfiguration;

import org.junit.Test;

import java.io.IOException;
import java.net.SocketTimeoutException;
import java.net.UnknownHostException;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

/**
 * The update check's rules: which tags read as versions, how a release
 * compares with this build, when a check is due, and when the notice on the
 * PC list shows and stays away.
 */
public class UpdateCheckTest {

    private static final String APP = "12.1-xr0.3";
    private static final String PAGE =
            "https://github.com/Gilleece/moonlight-android-xr/releases/tag/v0.4";

    @Test
    public void tagsReadWithOrWithoutTheirV() {
        assertArrayEquals(new int[] { 0, 4 }, UpdateCheck.parseTag("v0.4"));
        assertArrayEquals(new int[] { 0, 3 }, UpdateCheck.parseTag("0.3"));
        assertArrayEquals(new int[] { 1, 2, 3 }, UpdateCheck.parseTag("V1.2.3"));
        assertArrayEquals(new int[] { 2 }, UpdateCheck.parseTag("v2"));
        assertArrayEquals(new int[] { 0, 10 }, UpdateCheck.parseTag(" v0.10 "));
    }

    @Test
    public void anythingElseIsNotAVersion() {
        for (String tag : new String[] { null, "", "v", "nightly", "v0.4-beta", "v0.4rc1",
                "release-0.4", "v.4", "v0..4", "v0.4.", "vv0.4", "v1234567", "v0.4\nv9.9" }) {
            assertNull(tag, UpdateCheck.parseTag(tag));
        }
    }

    @Test
    public void theAppsNumberIsTheXrPartOfItsVersionName() {
        assertArrayEquals(new int[] { 0, 3 }, UpdateCheck.appVersion("12.1-xr0.3"));
        assertArrayEquals(new int[] { 1, 0, 2 }, UpdateCheck.appVersion("13.0-xr1.0.2"));
        assertArrayEquals(new int[] { 0, 4 }, UpdateCheck.appVersion("xr0.4"));
        for (String name : new String[] { null, "", "12.1", "12.1-xr", "12.1-xr0.3-rc1",
                "12.1-xr0.3 debug", "12.1-foxr0.3" }) {
            assertNull(name, UpdateCheck.appVersion(name));
        }
    }

    @Test
    public void partsCompareAsNumbers() {
        assertEquals(0, UpdateCheck.compare(new int[] { 0, 4 }, new int[] { 0, 4, 0 }));
        assertTrue(UpdateCheck.compare(new int[] { 0, 10 }, new int[] { 0, 9 }) > 0);
        assertTrue(UpdateCheck.compare(new int[] { 0, 4 }, new int[] { 0, 4, 1 }) < 0);
        assertTrue(UpdateCheck.compare(new int[] { 1 }, new int[] { 0, 99 }) > 0);
    }

    @Test
    public void onlyALaterReleaseIsNewer() {
        assertTrue(UpdateCheck.isNewer("v0.4", APP));
        assertTrue(UpdateCheck.isNewer("v0.3.1", APP));
        assertTrue(UpdateCheck.isNewer("v1.0", APP));
        assertTrue(UpdateCheck.isNewer("v0.10", "12.1-xr0.9"));
        assertFalse(UpdateCheck.isNewer("v0.3", APP));
        assertFalse(UpdateCheck.isNewer("v0.3.0", APP));
        assertFalse(UpdateCheck.isNewer("v0.2", APP));
        assertFalse(UpdateCheck.isNewer("v0.1", APP));
        // The 12.1 in front is upstream's number, not the fork's
        assertFalse(UpdateCheck.isNewer("v0.4", "12.1-xr0.5"));
    }

    @Test
    public void whatDoesNotReadIsNeverNewer() {
        assertFalse(UpdateCheck.isNewer("v0.4-beta", APP));
        assertFalse(UpdateCheck.isNewer("nightly", APP));
        assertFalse(UpdateCheck.isNewer(null, APP));
        assertFalse(UpdateCheck.isNewer("v0.4", "12.1"));
        assertFalse(UpdateCheck.isNewer("v0.4", null));
    }

    @Test
    public void aCheckIsDueOnceADay() {
        long day = UpdateCheck.INTERVAL_MS;
        long now = 1_790_000_000_000L;
        assertEquals(24L * 60 * 60 * 1000, day);
        assertTrue(UpdateCheck.isDue(now, 0));
        assertFalse(UpdateCheck.isDue(now, now));
        assertFalse(UpdateCheck.isDue(now, now - day + 1));
        assertTrue(UpdateCheck.isDue(now, now - day));
        // A clock set back must not stop the check for good
        assertTrue(UpdateCheck.isDue(now, now + 1000));
    }

    @Test
    public void viewOpensOnlyThisRepositorysReleases() {
        assertEquals(PAGE, UpdateCheck.viewUrl(PAGE));
        String list = "https://github.com/Gilleece/moonlight-android-xr/releases";
        assertEquals(list, UpdateCheck.RELEASES_PAGE);
        for (String url : new String[] { null, "", "javascript:alert(1)",
                "http://github.com/Gilleece/moonlight-android-xr/releases/tag/v0.4",
                "https://github.com/someone/else/releases/tag/v0.4",
                "https://github.com.example.org/Gilleece/moonlight-android-xr/releases/tag/v0.4",
                "https://example.org/" }) {
            assertEquals(url, list, UpdateCheck.viewUrl(url));
        }
    }

    @Test
    public void theRequestCarriesTheVersionOnly() {
        assertEquals("https://api.github.com/repos/Gilleece/moonlight-android-xr/releases/latest",
                UpdateCheck.LATEST_RELEASE_URL);
        assertEquals("Moonlight XR 12.1-xr0.3", UpdateCheck.userAgent(APP));
        assertEquals(5000, UpdateCheck.TIMEOUT_MS);
    }

    @Test
    public void aFailureIsDescribedByItsKindOrStatus() {
        assertEquals("HTTP 403", UpdateCheck.describeFailure(new UpdateCheck.StatusException(403)));
        assertEquals("SocketTimeoutException",
                UpdateCheck.describeFailure(new SocketTimeoutException("connect timed out")));
        assertEquals("UnknownHostException",
                UpdateCheck.describeFailure(new UnknownHostException("api.github.com")));
        assertEquals("IOException", UpdateCheck.describeFailure(new IOException("x")));
    }

    @Test
    public void aCheckIsClaimedOnceADayAndNotWithTheSettingOff() {
        FakePrefs prefs = new FakePrefs();
        long now = 1_790_000_000_000L;
        assertTrue(UpdateCheck.claim(prefs, now));
        assertEquals(now, prefs.getLong(UpdateCheck.LAST_CHECK_PREF, 0));
        assertFalse(UpdateCheck.claim(prefs, now + 60_000));
        assertFalse(UpdateCheck.claim(prefs, now + UpdateCheck.INTERVAL_MS - 1));
        assertTrue(UpdateCheck.claim(prefs, now + UpdateCheck.INTERVAL_MS));

        FakePrefs off = new FakePrefs();
        off.putBoolean(PreferenceConfiguration.CHECK_UPDATES_PREF_STRING, false);
        assertFalse(UpdateCheck.claim(off, now));
        assertFalse(off.contains(UpdateCheck.LAST_CHECK_PREF));
    }

    @Test
    public void theCurrentReleaseShowsNothing() {
        FakePrefs prefs = new FakePrefs();
        assertEquals("latest v0.3, this build 12.1-xr0.3, no notice",
                UpdateCheck.record(prefs, APP, "v0.3",
                        "https://github.com/Gilleece/moonlight-android-xr/releases/tag/v0.3",
                        false, false));
        assertEquals("v0.3", prefs.getString(UpdateCheck.LATEST_TAG_PREF, null));
        assertNull(UpdateCheck.notice(prefs, APP));
    }

    @Test
    public void aNewerReleaseShowsUntilDismissed() {
        FakePrefs prefs = new FakePrefs();
        assertEquals("latest v0.4, this build 12.1-xr0.3, notice shown",
                UpdateCheck.record(prefs, APP, "v0.4", PAGE, false, false));
        UpdateCheck.Notice notice = UpdateCheck.notice(prefs, APP);
        assertNotNull(notice);
        assertEquals("v0.4", notice.tag);
        assertEquals(PAGE, notice.url);

        UpdateCheck.dismiss(prefs, notice.tag);
        assertNull(UpdateCheck.notice(prefs, APP));
        // The next day's check finds the same release and stays quiet
        assertEquals("latest v0.4, this build 12.1-xr0.3, dismissed earlier, no notice",
                UpdateCheck.record(prefs, APP, "v0.4", PAGE, false, false));
        assertNull(UpdateCheck.notice(prefs, APP));

        // Only a newer one brings it back
        UpdateCheck.record(prefs, APP, "v0.5",
                "https://github.com/Gilleece/moonlight-android-xr/releases/tag/v0.5", false, false);
        notice = UpdateCheck.notice(prefs, APP);
        assertNotNull(notice);
        assertEquals("v0.5", notice.tag);
    }

    @Test
    public void updatingTheAppClearsTheNotice() {
        FakePrefs prefs = new FakePrefs();
        UpdateCheck.record(prefs, APP, "v0.4", PAGE, false, false);
        assertNotNull(UpdateCheck.notice(prefs, APP));
        assertNull(UpdateCheck.notice(prefs, "12.1-xr0.4"));
        assertNull(UpdateCheck.notice(prefs, "12.1-xr0.5"));
    }

    @Test
    public void theSettingOffHidesTheNotice() {
        FakePrefs prefs = new FakePrefs();
        UpdateCheck.record(prefs, APP, "v0.4", PAGE, false, false);
        prefs.putBoolean(PreferenceConfiguration.CHECK_UPDATES_PREF_STRING, false);
        assertNull(UpdateCheck.notice(prefs, APP));
        prefs.putBoolean(PreferenceConfiguration.CHECK_UPDATES_PREF_STRING, true);
        assertNotNull(UpdateCheck.notice(prefs, APP));
    }

    @Test
    public void prereleasesDraftsAndOddTagsAreNeverOffered() {
        FakePrefs prefs = new FakePrefs();
        assertEquals("the latest is a prerelease, ignored",
                UpdateCheck.record(prefs, APP, "v0.4", PAGE, true, false));
        assertEquals("the latest is a draft, ignored",
                UpdateCheck.record(prefs, APP, "v0.4", PAGE, false, true));
        assertEquals("the latest has a tag that does not read as a version, ignored",
                UpdateCheck.record(prefs, APP, "v0.4-beta", PAGE, false, false));
        assertFalse(prefs.contains(UpdateCheck.LATEST_TAG_PREF));
        assertNull(UpdateCheck.notice(prefs, APP));

        // And none of them overwrites a release already known
        UpdateCheck.record(prefs, APP, "v0.4", PAGE, false, false);
        UpdateCheck.record(prefs, APP, "v0.9", PAGE, true, false);
        UpdateCheck.record(prefs, APP, "nightly", PAGE, false, false);
        assertEquals("v0.4", UpdateCheck.notice(prefs, APP).tag);
    }

    @Test
    public void aReleasePageElsewhereOpensTheListInstead() {
        FakePrefs prefs = new FakePrefs();
        UpdateCheck.record(prefs, APP, "v0.4", "https://example.org/v0.4", false, false);
        assertEquals(UpdateCheck.RELEASES_PAGE, UpdateCheck.notice(prefs, APP).url);
    }

    @Test
    public void aDismissedTagThatDoesNotReadDoesNotHideANewRelease() {
        FakePrefs prefs = new FakePrefs();
        UpdateCheck.record(prefs, APP, "v0.4", PAGE, false, false);
        UpdateCheck.dismiss(prefs, "whatever");
        assertNotNull(UpdateCheck.notice(prefs, APP));
        // A dismissed release newer than the latest, if one was pulled, hides it
        UpdateCheck.dismiss(prefs, "v0.5");
        assertNull(UpdateCheck.notice(prefs, APP));
    }

    private static final class FakePrefs implements SharedPreferences, SharedPreferences.Editor {
        final Map<String, Object> values = new HashMap<>();

        @Override public Map<String, ?> getAll() { return values; }
        @Override public String getString(String key, String def) {
            return values.containsKey(key) ? (String) values.get(key) : def;
        }
        @Override public Set<String> getStringSet(String key, Set<String> def) { return def; }
        @Override public int getInt(String key, int def) {
            return values.containsKey(key) ? (Integer) values.get(key) : def;
        }
        @Override public long getLong(String key, long def) {
            return values.containsKey(key) ? (Long) values.get(key) : def;
        }
        @Override public float getFloat(String key, float def) { return def; }
        @Override public boolean getBoolean(String key, boolean def) {
            return values.containsKey(key) ? (Boolean) values.get(key) : def;
        }
        @Override public boolean contains(String key) { return values.containsKey(key); }
        @Override public Editor edit() { return this; }
        @Override public void registerOnSharedPreferenceChangeListener(
                OnSharedPreferenceChangeListener listener) { }
        @Override public void unregisterOnSharedPreferenceChangeListener(
                OnSharedPreferenceChangeListener listener) { }

        @Override public Editor putString(String key, String value) {
            values.put(key, value);
            return this;
        }
        @Override public Editor putStringSet(String key, Set<String> value) { return this; }
        @Override public Editor putInt(String key, int value) {
            values.put(key, value);
            return this;
        }
        @Override public Editor putLong(String key, long value) {
            values.put(key, value);
            return this;
        }
        @Override public Editor putFloat(String key, float value) { return this; }
        @Override public Editor putBoolean(String key, boolean value) {
            values.put(key, value);
            return this;
        }
        @Override public Editor remove(String key) {
            values.remove(key);
            return this;
        }
        @Override public Editor clear() {
            values.clear();
            return this;
        }
        @Override public boolean commit() { return true; }
        @Override public void apply() { }
    }
}

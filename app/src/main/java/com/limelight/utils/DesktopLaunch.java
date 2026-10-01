package com.limelight.utils;

import com.limelight.nvstream.http.NvApp;
import com.limelight.nvstream.http.NvHTTP;

import org.xmlpull.v1.XmlPullParserException;

import java.io.IOException;
import java.io.StringReader;
import java.util.List;

/**
 * Which app a tap on a PC opens straight away when "Open Desktop
 * automatically" is on. Sunshine and Apollo both ship an app called Desktop,
 * and that is the one looked for. Anything less certain goes to the app list.
 */
public final class DesktopLaunch {

    public static final String DESKTOP = "Desktop";

    private DesktopLaunch() {
    }

    /** The apps in a host's applist answer, as the app list itself reads them. */
    public static List<NvApp> parse(String rawAppList) throws XmlPullParserException, IOException {
        return NvHTTP.getAppListByReader(new StringReader(rawAppList));
    }

    /** The first app named Desktop, whatever its case, or null. */
    public static NvApp findDesktop(List<NvApp> apps) {
        if (apps == null) {
            return null;
        }
        for (NvApp app : apps) {
            String name = app.getAppName();
            if (name != null && name.trim().equalsIgnoreCase(DESKTOP)) {
                return app;
            }
        }
        return null;
    }

    /**
     * The app to start, or null for the app list. Desktop is started when
     * nothing is running, and resumed when it is what is running, as the
     * list's own Resume does. With another app running the list is where
     * quitting it or going back to it is offered.
     */
    public static NvApp choose(List<NvApp> apps, int runningAppId) {
        NvApp desktop = findDesktop(apps);
        if (desktop == null) {
            return null;
        }
        if (runningAppId != 0 && runningAppId != desktop.getAppId()) {
            return null;
        }
        return desktop;
    }
}

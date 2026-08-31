package com.limelight;

import android.app.Application;
import android.preference.PreferenceManager;

import com.limelight.preferences.PreferenceConfiguration;

/**
 * Exists only to start the file log before anything else runs. The setting is
 * read straight out of shared preferences rather than through
 * PreferenceConfiguration, since that pulls in display and codec lookups this
 * early in startup.
 */
public class MoonlightApplication extends Application {
    @Override
    public void onCreate() {
        super.onCreate();

        String setting = PreferenceManager.getDefaultSharedPreferences(this)
                .getString(PreferenceConfiguration.FILE_LOG_PREF_STRING,
                           PreferenceConfiguration.DEFAULT_FILE_LOG);
        FileLog.init(this, FileLog.levelFromName(setting));
    }
}

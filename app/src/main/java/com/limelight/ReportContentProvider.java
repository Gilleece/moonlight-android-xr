package com.limelight;

import android.content.ContentProvider;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import java.io.File;
import java.io.FileNotFoundException;
import java.util.List;

/**
 * Hands a saved bug report to whichever app the user sends it with. Email
 * clients cannot read another app's files, so the report is given to them as
 * a content URI with a read grant instead. Only files in the reports folder
 * are reachable, by name, and only for reading.
 */
public class ReportContentProvider extends ContentProvider {
    public static final String AUTHORITY = "report." + BuildConfig.APPLICATION_ID;
    private static final String REPORTS_PATH = "reports";
    private static final String TEXT_MIME_TYPE = "text/plain";

    /** Where the reports the provider serves are written. */
    public static File reportsDir(android.content.Context context) {
        return context.getExternalFilesDir(REPORTS_PATH);
    }

    public static Uri uriFor(File report) {
        return new Uri.Builder()
                .scheme(ContentResolver.SCHEME_CONTENT)
                .authority(AUTHORITY)
                .appendPath(REPORTS_PATH)
                .appendPath(report.getName())
                .build();
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) {
            throw new FileNotFoundException("reports are read only");
        }
        List<String> segments = uri.getPathSegments();
        if (segments.size() != 2 || !REPORTS_PATH.equals(segments.get(0))) {
            throw new FileNotFoundException();
        }
        String name = segments.get(1);
        // The name is the whole of what the caller chooses, so it must not be
        // able to walk anywhere else
        if (name.isEmpty() || name.contains("/") || name.contains("\\") || name.startsWith(".")) {
            throw new FileNotFoundException();
        }
        File dir = reportsDir(getContext());
        if (dir == null) {
            throw new FileNotFoundException();
        }
        File file = new File(dir, name);
        if (!file.isFile()) {
            throw new FileNotFoundException();
        }
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public String getType(Uri uri) {
        return TEXT_MIME_TYPE;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs,
                        String sortOrder) {
        throw new UnsupportedOperationException("reports are files, not rows");
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("reports are read only");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("reports are read only");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("reports are read only");
    }
}

package com.limelight;

import android.content.Context;
import android.os.Build;
import android.os.Environment;
import android.util.Log;

import com.limelight.binding.video.XrShared;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

/**
 * A plain text copy of the log, written to the public Download folder so it
 * can be sent in with a bug report. It goes there rather than beside the
 * app's own files because newer Android hides Android/data from file
 * managers, and on a headset the file manager on the device is the only way
 * most people will ever reach it. Logcat still gets everything it always did:
 * this only ever adds a destination.
 *
 * There are only ever two files. When the log fills it becomes the previous
 * log, replacing the one before it, and a fresh one starts from the device
 * header, so a log left on for months costs a user the same as one left on for
 * a day while the start of a long session is not thrown away mid stream.
 *
 * Nothing here touches the LimeLog call sites. A handler on the same logger
 * picks those up, and the few lines worth having at the quiet level are
 * marked with event() by hand.
 */
public class FileLog {
    public static final int OFF = XrShared.FILE_LOG_OFF;
    public static final int BASIC = XrShared.FILE_LOG_BASIC;
    public static final int VERBOSE = XrShared.FILE_LOG_VERBOSE;

    private static final String TAG = "moonlight";
    private static final String PUBLIC_DIR = "MoonlightXR";
    private static final String LOG_DIR = "logs";
    private static final String LOG_NAME = "moonlight.log";
    private static final String PREVIOUS_LOG_NAME = "moonlight.previous.log";

    // Rotated once at this size and never further, so nothing accumulates on
    // a user's device. Big enough to hold a long session, small enough to
    // attach to a report.
    private static final long MAX_BYTES = 5 * 1024 * 1024;
    // Deep enough that a burst of stats lines rides through, shallow enough
    // that a stuck writer cannot eat the heap
    private static final int QUEUE_CAPACITY = 512;

    private static final SimpleDateFormat TIMESTAMP =
            new SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.US);

    // Everything that touches the file is done holding this, so a crashing
    // thread can write its stack without racing the writer thread
    private static final Object fileLock = new Object();
    private static final AtomicInteger dropped = new AtomicInteger();

    private static volatile int level = OFF;
    private static volatile String logPath;
    private static boolean initialized;

    private static File logFile;
    private static FileOutputStream stream;
    private static BufferedWriter writer;
    private static long fileBytes;

    private static ArrayBlockingQueue<Entry> queue;
    // Held so the log manager cannot collect the logger our handler is on
    private static Logger hookedLogger;
    // Kept because a wipe has to put them at the top of the new file, so a
    // log that started over still says which device it came from
    private static String[] headerLines;

    private static class Entry {
        final long millis;
        final char level;
        final String message;

        Entry(long millis, char level, String message) {
            this.millis = millis;
            this.level = level;
            this.message = message;
        }
    }

    /**
     * Opens the log and starts listening. Called once from the application,
     * before anything worth recording has happened. With logging off this
     * returns without creating a file or attaching anything.
     */
    public static synchronized void init(Context context, int wanted) {
        if (initialized || wanted == OFF) {
            return;
        }
        initialized = true;

        if (!openFirstWritableDir(context)) {
            return;
        }

        level = wanted;
        logPath = logFile.getAbsolutePath();
        // Built before the writer thread starts, since that is the thread a
        // wipe reads them back on
        headerLines = buildHeader(context);
        queue = new ArrayBlockingQueue<>(QUEUE_CAPACITY);

        Thread writerThread = new Thread() {
            @Override
            public void run() {
                drainQueue();
            }
        };
        writerThread.setName("File log");
        writerThread.setDaemon(true);
        writerThread.start();

        hookLimeLog();
        hookCrashes();
        for (String line : headerLines) {
            event(line);
        }
    }

    /**
     * Settles on a directory and opens the log in it. Download first, since
     * that is the one the file manager on the headset can browse. It is not
     * guaranteed to be writable though: no storage permission is asked for,
     * and after a reinstall the old files there belong to the previous
     * install, so the app's own folder stays as a fallback.
     */
    private static boolean openFirstWritableDir(Context context) {
        File download = new File(Environment.getExternalStoragePublicDirectory(
                Environment.DIRECTORY_DOWNLOADS), PUBLIC_DIR);
        File own = context.getExternalFilesDir(null);
        File[] candidates = own != null
                ? new File[] { download, new File(own, LOG_DIR) }
                : new File[] { download };

        for (File dir : candidates) {
            if (!dir.isDirectory() && !dir.mkdirs()) {
                continue;
            }
            logFile = new File(dir, LOG_NAME);
            synchronized (fileLock) {
                if (openFile()) {
                    return true;
                }
            }
            logFile = null;
        }
        return false;
    }

    /**
     * A line worth keeping even at the quiet level. Goes to logcat as well,
     * so logcat stays a superset of the file.
     */
    public static void event(String message) {
        Log.i(TAG, message);
        if (level != OFF) {
            enqueue('I', message);
        }
    }

    /** The file to point a bug reporter at, or null when logging is off. */
    public static String getLogPath() {
        return logPath;
    }

    /** The log before this one, which may not exist, or null when logging is off. */
    public static File getPreviousLogFile() {
        if (logPath == null) {
            return null;
        }
        return new File(new File(logPath).getParentFile(), PREVIOUS_LOG_NAME);
    }

    public static int getLevel() {
        return level;
    }

    public static boolean isVerbose() {
        return level == VERBOSE;
    }

    /** Maps the setting value onto one of the level constants. */
    public static int levelFromName(String name) {
        if ("verbose".equals(name)) {
            return VERBOSE;
        }
        if ("basic".equals(name)) {
            return BASIC;
        }
        return OFF;
    }

    // What the file has to say about the device before anything else, both at
    // the top of a fresh log and again after a wipe
    private static String[] buildHeader(Context context) {
        return new String[] {
                "moonlight " + BuildConfig.VERSION_NAME + " " + context.getPackageName()
                        + (BuildConfig.ROOT_BUILD ? " root" : ""),
                "device " + Build.MANUFACTURER + " " + Build.MODEL + " (" + Build.DEVICE + ")",
                "android " + Build.VERSION.RELEASE + " sdk " + Build.VERSION.SDK_INT,
                "log " + logPath
        };
    }

    /**
     * Everything already going through LimeLog lands here as well. Info is
     * the chatty half of the stream, so it only reaches the file when the
     * user asked for verbose. Parent handlers stay on, so logcat is unchanged.
     */
    private static void hookLimeLog() {
        hookedLogger = Logger.getLogger(LimeLog.class.getName());
        hookedLogger.addHandler(new Handler() {
            @Override
            public void publish(LogRecord record) {
                if (record == null || record.getMessage() == null) {
                    return;
                }
                int value = record.getLevel().intValue();
                if (value >= Level.SEVERE.intValue()) {
                    enqueue('E', record.getMessage());
                }
                else if (value >= Level.WARNING.intValue()) {
                    enqueue('W', record.getMessage());
                }
                else if (level == VERBOSE) {
                    enqueue('V', record.getMessage());
                }
            }

            @Override
            public void flush() {
            }

            @Override
            public void close() {
            }
        });
    }

    /**
     * Crashes are the reason the file exists, and the process is gone before
     * the writer thread would get a turn, so they go straight to disk.
     */
    private static void hookCrashes() {
        final Thread.UncaughtExceptionHandler previous =
                Thread.getDefaultUncaughtExceptionHandler();

        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            @Override
            public void uncaughtException(Thread thread, Throwable error) {
                try {
                    StringWriter trace = new StringWriter();
                    error.printStackTrace(new PrintWriter(trace));
                    writeSync("FATAL EXCEPTION in " + thread.getName() + "\n" + trace);
                } catch (Throwable ignored) {
                    // Never get in the way of the real handler
                }
                if (previous != null) {
                    previous.uncaughtException(thread, error);
                }
            }
        });
    }

    private static void enqueue(char entryLevel, String message) {
        ArrayBlockingQueue<Entry> q = queue;
        if (q == null) {
            return;
        }
        if (!q.offer(new Entry(System.currentTimeMillis(), entryLevel, message))) {
            dropped.incrementAndGet();
        }
    }

    private static void drainQueue() {
        while (true) {
            Entry entry = queue.poll();
            if (entry == null) {
                flushFile();
                try {
                    entry = queue.take();
                } catch (InterruptedException e) {
                    return;
                }
            }

            // Taking that one made room, so the count of what was lost while
            // there was none can go in ahead of it
            int lost = dropped.getAndSet(0);
            if (lost > 0) {
                writeLine(entry.millis, 'W', lost + " log lines dropped");
            }
            writeLine(entry.millis, entry.level, entry.message);
        }
    }

    private static void writeLine(long millis, char entryLevel, String message) {
        synchronized (fileLock) {
            if (writer == null) {
                return;
            }
            try {
                String line = TIMESTAMP.format(new Date(millis)) + " " + entryLevel + " "
                        + message + "\n";
                writer.write(line);
                fileBytes += line.length();
                if (fileBytes > MAX_BYTES) {
                    wipe();
                }
            } catch (IOException e) {
                closeFile();
            }
        }
    }

    private static void writeSync(String message) {
        synchronized (fileLock) {
            if (writer == null) {
                return;
            }
            try {
                writer.write(TIMESTAMP.format(new Date()) + " F " + message + "\n");
                writer.flush();
                stream.getFD().sync();
            } catch (IOException e) {
                closeFile();
            }
        }
    }

    private static void flushFile() {
        synchronized (fileLock) {
            if (writer == null) {
                return;
            }
            try {
                writer.flush();
                // Everything is on disk now, so this also picks up whatever
                // the renderer appended behind our back
                fileBytes = logFile.length();
                if (fileBytes > MAX_BYTES) {
                    wipe();
                }
            } catch (IOException e) {
                closeFile();
            }
        }
    }

    /**
     * The log filled, so it becomes the previous log and a fresh one starts.
     * One previous copy and no more is deliberate: a log left switched on
     * should never cost a user more than two files, and the second is what
     * keeps the first half of a long session readable after the turn.
     *
     * Caller holds fileLock, on the writer thread.
     */
    private static void wipe() {
        closeFile();

        // The rename can fail. Reopen what is there and let it run over the
        // cap, since carrying on with an oversize log beats going quiet for
        // the rest of the session, and the next write tries again.
        File previous = new File(logFile.getParentFile(), PREVIOUS_LOG_NAME);
        previous.delete();
        if (!logFile.renameTo(previous)) {
            openFile();
            return;
        }
        if (!openFile()) {
            return;
        }

        // Fresh file, so it has to say what it came from again. openFile has
        // just put fileBytes back to zero and these few lines cannot reach the
        // cap, so writing them here cannot roll it straight over again.
        long now = System.currentTimeMillis();
        writeLine(now, 'I', "log reached " + (MAX_BYTES / (1024 * 1024))
                + " MB, the earlier part is in " + PREVIOUS_LOG_NAME);
        for (String line : headerLines) {
            writeLine(now, 'I', line);
        }
    }

    // Caller holds fileLock
    private static boolean openFile() {
        try {
            stream = new FileOutputStream(logFile, true);
            writer = new BufferedWriter(new OutputStreamWriter(stream));
            fileBytes = logFile.length();
            return true;
        } catch (IOException e) {
            stream = null;
            writer = null;
            return false;
        }
    }

    // Caller holds fileLock
    private static void closeFile() {
        try {
            if (writer != null) {
                writer.close();
            }
        } catch (IOException ignored) {
        }
        writer = null;
        stream = null;
    }
}

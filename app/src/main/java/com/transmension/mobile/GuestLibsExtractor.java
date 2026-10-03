package com.transmension.mobile;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Process;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class GuestLibsExtractor {
    private static final String TAG = "GuestLibsExtractor";
    private static final String PREF_NAME = "guest_libs_prefs";
    private static final String KEY_APK_TIMESTAMP = "apk_timestamp";

    /**
     * La carpeta de archivos del juego: donde escribe sus partidas, el
     * ZOMBATAR.PNG y su perfil.
     *
     * En 32 bits es la propia carpeta de datos. Bajo el runner de 64 bits el
     * juego recibe data/ como carpeta de archivos, asi que todo lo suyo cuelga
     * de data/. Quien lea o escriba algo del juego tiene que preguntar aqui:
     * usar la carpeta equivocada no da error, simplemente no hace nada.
     */
    public static File getGameFilesDir(File dataDir) {
        return is64Bit() ? new File(dataDir, "data") : dataDir;
    }

    /** Carpeta donde el juego guarda las partidas. */
    public static File getSaveDir(File dataDir) {
        return new File(getGameFilesDir(dataDir), "userdata");
    }

    public static boolean is64Bit() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            return Process.is64Bit();
        }
        try {
            Class<?> vmRuntime = Class.forName("dalvik.system.VMRuntime");
            Object runtime = vmRuntime.getMethod("getRuntime").invoke(null);
            return (Boolean) vmRuntime.getMethod("is64Bit").invoke(runtime);
        } catch (Throwable ignored) {
            String arch = System.getProperty("os.arch");
            return arch != null && arch.contains("64");
        }
    }

    public static void extractIfNeeded(Context context) {
        if (!is64Bit()) {
            Log.i(TAG, "Process is running in 32-bit mode; guest extraction not required.");
            return;
        }

        try {
            String apkPath = context.getPackageCodePath();
            File apkFile = new File(apkPath);
            long currentApkTimestamp = apkFile.lastModified();

            SharedPreferences prefs = context.getSharedPreferences(PREF_NAME, Context.MODE_PRIVATE);
            long savedTimestamp = prefs.getLong(KEY_APK_TIMESTAMP, -1);

            File targetDir = context.getFilesDir();
            File externalDir = context.getExternalFilesDir(null);
            SharedPreferences dataPrefs = context.getSharedPreferences("data", 0);
            boolean useExternal = dataPrefs.getBoolean("useExternalPath", Build.VERSION.SDK_INT >= Build.VERSION_CODES.M);
            File dataDir = (useExternal && externalDir != null) ? externalDir : targetDir;

            File gameMain = new File(targetDir, "libGameMain.so");
            File homura = new File(targetDir, "libHomura.so");
            File assetCheck = new File(dataDir, "assets/properties/resources.xml");

            if (savedTimestamp == currentApkTimestamp && gameMain.exists() && homura.exists() && assetCheck.exists() && gameMain.length() > 0) {
                Log.i(TAG, "Guest libraries and assets are already up to date.");
                return;
            }

            Log.i(TAG, "Extracting 32-bit guest libraries and assets from APK to " + dataDir.getAbsolutePath());
            if (!targetDir.exists()) targetDir.mkdirs();

            File assetsDir = new File(dataDir, "assets");
            if (!assetsDir.exists()) assetsDir.mkdirs();

            File userdataDir = new File(dataDir, "userdata");
            if (!userdataDir.exists()) userdataDir.mkdirs();

            long startTime = System.currentTimeMillis();
            ZipFile zip = new ZipFile(apkFile);
            Enumeration<? extends ZipEntry> entries = zip.entries();
            byte[] buffer = new byte[65536];
            int libCount = 0;
            int assetCount = 0;

            while (entries.hasMoreElements()) {
                ZipEntry entry = entries.nextElement();
                String name = entry.getName();

                if (name.startsWith("lib/armeabi-v7a/") && name.endsWith(".so")) {
                    String fileName = name.substring(name.lastIndexOf('/') + 1);
                    File outFile = new File(targetDir, fileName);
                    extractFile(zip, entry, outFile, buffer);
                    outFile.setReadable(true, false);
                    outFile.setExecutable(true, false);
                    libCount++;
                } else if (name.startsWith("assets/files/")) {
                    String relPath = name.substring("assets/files/".length());
                    if (!relPath.isEmpty() && !entry.isDirectory()) {
                        File outFile = new File(assetsDir, relPath);
                        File parent = outFile.getParentFile();
                        if (parent != null && !parent.exists()) parent.mkdirs();
                        extractFile(zip, entry, outFile, buffer);
                        assetCount++;
                    }
                } else if (name.equals("assets/defaultSetting.xml")) {
                    File outFile = new File(assetsDir, "defaultSetting.xml");
                    File parent = outFile.getParentFile();
                    if (parent != null && !parent.exists()) parent.mkdirs();
                    extractFile(zip, entry, outFile, buffer);
                    assetCount++;
                }
            }
            zip.close();

            long elapsed = System.currentTimeMillis() - startTime;
            Log.i(TAG, "Extracted " + libCount + " guest libs and " + assetCount + " assets in " + elapsed + " ms");

            prefs.edit().putLong(KEY_APK_TIMESTAMP, currentApkTimestamp).apply();

        } catch (Throwable t) {
            Log.e(TAG, "Failed to extract guest libraries/assets: " + t.getMessage(), t);
        }
    }

    private static void extractFile(ZipFile zip, ZipEntry entry, File outFile, byte[] buffer) throws Exception {
        InputStream in = zip.getInputStream(entry);
        try (FileOutputStream out = new FileOutputStream(outFile)) {
            int read;
            while ((read = in.read(buffer)) != -1) {
                out.write(buffer, 0, read);
            }
            out.flush();
        } finally {
            in.close();
        }
    }
}

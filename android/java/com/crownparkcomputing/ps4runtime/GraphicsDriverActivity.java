package com.crownparkcomputing.ps4runtime;

import android.app.*;
import android.content.*;
import android.net.Uri;
import android.os.*;
import android.widget.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.JSONObject;

/** Opens independently of SDL, including when a chosen driver cannot start the game. */
public final class GraphicsDriverActivity extends Activity {
    private static final int IMPORT_DRIVER = 4201;
    private static final AtomicBoolean importing = new AtomicBoolean();
    @Override public void onCreate(Bundle state) { super.onCreate(state); GraphicsDriverSettings.registerShortcut(this); showSettings(); }
    private String bundledName() {
        try (InputStream in = getAssets().open("driver-source.json")) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] block = new byte[4096]; int n;
            while ((n = in.read(block)) != -1) {
                if (out.size() + n > 65536) break;
                out.write(block, 0, n);
            }
            return new JSONObject(new String(out.toByteArray(), StandardCharsets.UTF_8))
                .getJSONObject("metadata").optString("name", "Turnip");
        } catch (Exception e) { return "Turnip"; }
    }
    private void showSettings() {
        if (isFinishing()) return;
        SharedPreferences p = GraphicsDriverSettings.preferences(this);
        String custom = p.getString("custom_name", "");
        boolean available = !custom.isEmpty() && new File(p.getString("custom_path", "")).isFile();
        String[] choices = available
            ? new String[]{"Bundled: " + bundledName() + " (default)", "System Vulkan driver", "Imported: " + custom}
            : new String[]{"Bundled: " + bundledName() + " (default)", "System Vulkan driver"};
        String selected = p.getString("choice", "bundled");
        int index = "system".equals(selected) ? 1 : "custom".equals(selected) && available ? 2 : 0;
        new AlertDialog.Builder(this).setTitle("Graphics driver")
            .setSingleChoiceItems(choices, index, (dialog, which) -> {
                String value = which == 1 ? "system" : which == 2 ? "custom" : "bundled";
                if (p.edit().putString("choice", value).commit())
                    Toast.makeText(this, "Saved for the next launch. Close and reopen the game to apply.", Toast.LENGTH_LONG).show();
                else Toast.makeText(this, "Could not save the driver setting", Toast.LENGTH_LONG).show();
            })
            .setPositiveButton("Done", (dialog, which) -> finish())
            .setNeutralButton("Import driver ZIP", (dialog, which) -> {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
                try { startActivityForResult(intent, IMPORT_DRIVER); }
                catch (ActivityNotFoundException e) { showError("No document picker is available"); }
            })
            .setOnCancelListener(dialog -> finish()).show();
    }
    private void showError(String message) {
        if (isFinishing()) return;
        new AlertDialog.Builder(this).setTitle("Driver import failed").setMessage(message)
            .setPositiveButton("OK", (d, w) -> showSettings()).setOnCancelListener(d -> showSettings()).show();
    }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != IMPORT_DRIVER) return;
        if (result != RESULT_OK || data == null || data.getData() == null) { showSettings(); return; }
        if (!importing.compareAndSet(false, true)) { showError("Another driver import is running"); return; }
        Uri uri = data.getData();
        AlertDialog progress = new AlertDialog.Builder(this).setTitle("Importing graphics driver")
            .setView(new ProgressBar(this)).setCancelable(false).create();
        progress.show();
        new Thread(() -> {
            String error = null;
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                if (in == null) throw new IOException("The selected file cannot be read");
                File root = new File(getFilesDir(), "drivers/imported");
                DriverZipImporter.Result installed = DriverZipImporter.install(in, getCacheDir(), root, Build.VERSION.SDK_INT, json -> {
                    JSONObject m = new JSONObject(json);
                    String name = m.optString("name", "Imported driver");
                    if (name.length() > 160) name = name.substring(0, 160);
                    return new DriverZipImporter.Metadata(m.getString("libraryName"), name, m.optInt("minApi", 0));
                });
                if (!GraphicsDriverSettings.preferences(this).edit().putString("custom_path", installed.library.getPath())
                    .putString("custom_name", installed.name).putString("custom_sha256", installed.sha256)
                    .putString("choice", "custom").commit()) throw new IOException("Could not save the imported driver setting");
            } catch (Exception e) { error = e.getMessage() == null ? "The driver could not be imported" : e.getMessage(); }
            String failure = error;
            runOnUiThread(() -> {
                importing.set(false);
                progress.dismiss();
                if (isFinishing()) return;
                if (failure == null) {
                    Toast.makeText(this, "Driver imported. Close and reopen the game to apply.", Toast.LENGTH_LONG).show();
                    showSettings();
                } else showError(failure);
            });
        }, "GraphicsDriverImport").start();
    }
}

package com.crownparkcomputing.ps4runtime;

import android.app.Activity;
import android.content.*;
import android.content.pm.*;
import android.graphics.drawable.Icon;
import java.io.File;
import java.util.Collections;

/** Per-game preference, read before the native renderer opens Vulkan. */
public final class GraphicsDriverSettings {
    static SharedPreferences preferences(Context context) {
        return context.getSharedPreferences("graphics_driver", Context.MODE_PRIVATE);
    }
    public static String selectedLibraryPath(Context context) {
        SharedPreferences p = preferences(context);
        String choice = p.getString("choice", "bundled");
        if ("system".equals(choice)) return "system";
        if ("custom".equals(choice)) {
            try {
                File library = new File(p.getString("custom_path", "")).getCanonicalFile();
                String root = new File(context.getFilesDir(), "drivers/imported").getCanonicalPath() + File.separator;
                if (library.getPath().startsWith(root) && library.isFile()) return library.getPath();
            } catch (Exception ignored) {}
        }
        return "bundled";
    }
    public static void registerShortcut(Activity activity) {
        try {
            ShortcutManager manager = activity.getSystemService(ShortcutManager.class);
            Intent settings = new Intent(activity, GraphicsDriverActivity.class).setAction(Intent.ACTION_VIEW);
            ShortcutInfo shortcut = new ShortcutInfo.Builder(activity, "graphics-driver")
                .setShortLabel("Graphics driver")
                .setLongLabel("Choose graphics driver")
                .setIcon(Icon.createWithResource(activity, activity.getApplicationInfo().icon))
                .setIntent(settings).build();
            if (manager != null) manager.setDynamicShortcuts(Collections.singletonList(shortcut));
        } catch (RuntimeException ignored) { /* App-info preferences remain available. */ }
    }
}

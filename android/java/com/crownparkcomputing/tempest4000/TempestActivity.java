package com.crownparkcomputing.tempest4000;

import org.libsdl.app.SDLActivity;
import android.os.Bundle;
import com.crownparkcomputing.ps4runtime.GraphicsDriverSettings;

/** SDL owns the surface, gamepad and audio device; libmain runs the port. */
public class TempestActivity extends SDLActivity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        GraphicsDriverSettings.registerShortcut(this);
    }
    public String getGraphicsDriverPath() {
        return GraphicsDriverSettings.selectedLibraryPath(this);
    }
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}

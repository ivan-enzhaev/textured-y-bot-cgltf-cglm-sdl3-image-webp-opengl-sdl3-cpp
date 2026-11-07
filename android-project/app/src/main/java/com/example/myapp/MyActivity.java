package com.example.myapp;

import org.libsdl.app.SDLActivity;

public class MyActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {
            "c++_shared",
            "SDL3",
            "SDL3_image",
            "main"
        };
    }
}
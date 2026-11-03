// The game process's memory-pressure callbacks for the native host: NativeActivity forwards onLowMemory only, which on a tablet already swapping came after the low-memory
// killer had ended the game in the foreground. Registered on the Application by the native front end (app/front_android.c trim_watch), it passes onTrimMemory's level
// (RUNNING_LOW 10, RUNNING_CRITICAL 15, and the background levels) and onLowMemory (as 80) to nativeTrim, which purges what the host can give back
package com.nxnative;

import android.content.ComponentCallbacks2;
import android.content.Context;
import android.content.res.Configuration;

public class TrimWatch implements ComponentCallbacks2 {
    static native void nativeTrim(int level);
    public static void register(Context c) { c.getApplicationContext().registerComponentCallbacks(new TrimWatch()); }
    @Override public void onTrimMemory(int level) { nativeTrim(level); }
    @Override public void onLowMemory() { nativeTrim(80); }
    @Override public void onConfigurationChanged(Configuration c) {}
}

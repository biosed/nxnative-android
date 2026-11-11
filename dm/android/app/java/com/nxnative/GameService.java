// Keeps the game's process (":game") alive while the player is away, as a Switch keeps a suspended game in memory: a foreground service in that process with an ongoing
// notification ("Running" / "Suspended"); tapping it brings the game back. The native front end (app/front_android.c) starts it when the game's activity is made (the app is
// in the foreground then, as Android 12+ requires for starting a foreground service) and switches the notification's text when the game suspends and resumes. The game
// itself does nothing while suspended (the host's graphics gate is closed, its clock frozen, its audio stopped). No type of Android 14's list describes "a suspended game
// kept in memory", so the service is of type specialUse, with the reason in the manifest's PROPERTY_SPECIAL_USE_FGS_SUBTYPE (tools/make_apk.py)
package com.nxnative;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;

public class GameService extends Service {
    static final String CHANNEL = "game";
    static final int ID = 12;
    static final int TYPE_SPECIAL_USE = 0x40000000;   // ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE (API 34)
    static volatile boolean suspended;

    // From the native front end, on the UI thread of the game's activity
    public static void start(Context c) {
        try { c.startForegroundService(new Intent(c, GameService.class)); } catch (Exception e) { /* not allowed now: the game runs without it */ }
    }
    public static void setSuspended(Context c, boolean s) {
        suspended = s;
        try { ((NotificationManager) c.getSystemService(Context.NOTIFICATION_SERVICE)).notify(ID, build(c)); } catch (Exception e) { /* no notification permission */ }
    }

    static Notification build(Context c) {
        NotificationManager nm = (NotificationManager) c.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm.getNotificationChannel(CHANNEL) == null) {
            NotificationChannel ch = new NotificationChannel(CHANNEL, "Game in the background", NotificationManager.IMPORTANCE_LOW);
            ch.setShowBadge(false); nm.createNotificationChannel(ch);
        }
        Intent back = new Intent();
        back.setClassName(c, "android.app.NativeActivity");
        back.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
        PendingIntent pi = PendingIntent.getActivity(c, 0, back, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        CharSequence label = c.getApplicationInfo().loadLabel(c.getPackageManager());
        return new Notification.Builder(c, CHANNEL).setSmallIcon(c.getApplicationInfo().icon).setContentTitle(label)
            .setContentText(suspended ? "Suspended — tap to continue" : "Running").setOngoing(true).setOnlyAlertOnce(true).setContentIntent(pi).build();
    }

    @Override public int onStartCommand(Intent i, int flags, int id) {
        if (Build.VERSION.SDK_INT >= 34) startForeground(ID, build(this), TYPE_SPECIAL_USE);
        else startForeground(ID, build(this));
        return START_NOT_STICKY;   // the process gone, there is no game to keep
    }
    @Override public IBinder onBind(Intent i) { return null; }
}

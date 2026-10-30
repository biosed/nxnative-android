// Start-up menu of the Android app (Spanish, as the Mac's start-up window in dm/host/front_mac.m): console mode, frame rate, screen fit, language, frame-rate counter, the
// cheats of the game's build (each one chosen by the player, none on by default) and "no volver a mostrar". The choices go to <files>/settings.env, which the game's
// native activity (app/front_android.c, process ":game") reads at boot. The menu stays under the game: when the game ends (− and + held two seconds) it shows again.
// On first start the bundled save (assets/save, the player's progress from another device) seeds the app's save folder; the cheat file is refreshed from assets/cheats.txt.
// An APK with the game inside (make_apk.py --bundle: assets/game/ and its list files.txt) unpacks it into the external folder on its first start, and again after an update,
// with a progress bar; Jugar waits for it.
// The menu works with the touch screen and with a controller (A confirms, B goes back, START plays). The screen stays on while it is open, as in the game.
package com.nxnative;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.AssetManager;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

public class MenuActivity extends Activity {
    private SharedPreferences prefs;
    private Spinner mode, fps, fit, lang;
    private CheckBox showFps, skip;
    private final List<CheckBox> cheatBoxes = new ArrayList<>();
    private boolean launched;
    private Button playButton;
    private boolean playKeyDown;
    private TextView status; private android.widget.ProgressBar bar; private volatile boolean unpacking;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        prefs = getSharedPreferences("menu", MODE_PRIVATE);
        seedFiles();
        buildUi();
        if (needsUnpack()) { unpack(); return; }
        boolean forced = new File(getFilesDir(), "show_menu").delete();
        if (prefs.getBoolean("skip", false) && !forced && b == null && gameFilesPresent()) play();
    }
    @Override protected void onPause() { super.onPause(); if (mode != null) save(); }   // choices are kept also when the menu is left without playing
    @Override protected void onResume() {
        super.onResume();
        playKeyDown = false;
        if (launched && new File(getFilesDir(), "show_menu").delete()) launched = false;   // back from the game with − and +
    }

    // The bundled save goes in only when the app has none yet (never over progress made here); the cheat file always follows the APK
    private void seedFiles() {
        File save = new File(getFilesDir(), "save");
        String[] have = save.list();
        if (have == null || have.length == 0) copyAssetDir("save", save);
        copyAsset("cheats.txt", new File(getFilesDir(), "cheats.txt"));
        getExternalFilesDir(null);   // makes the external folder the game files go into
    }
    private void copyAssetDir(String dir, File to) {
        try {
            String[] names = getAssets().list(dir);
            if (names == null || names.length == 0) return;
            to.mkdirs();
            for (String n : names) copyAsset(dir + "/" + n, new File(to, n));
        } catch (Exception e) { /* no bundled save */ }
    }
    private void copyAsset(String name, File to) {
        try (InputStream in = getAssets().open(name, AssetManager.ACCESS_STREAMING); OutputStream out = new FileOutputStream(to)) {
            byte[] buf = new byte[65536]; int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        } catch (Exception e) { /* not bundled */ }
    }
    // ---- The game inside the APK ----
    private long versionCode() { try { return getPackageManager().getPackageInfo(getPackageName(), 0).getLongVersionCode(); } catch (Exception e) { return 0; } }
    private boolean bundled() { try (InputStream in = getAssets().open("game/files.txt")) { return true; } catch (Exception e) { return false; } }
    private boolean needsUnpack() {
        if (!bundled()) return false;
        File mark = new File(getExternalFilesDir(null), ".unpacked");
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new java.io.FileInputStream(mark), StandardCharsets.UTF_8))) { return !String.valueOf(versionCode()).equals(r.readLine()); }
        catch (Exception e) { return true; }
    }
    private void unpack() {
        unpacking = true; playButton.setEnabled(false);
        status.setVisibility(View.VISIBLE); bar.setVisibility(View.VISIBLE);
        status.setText("Preparando el juego por primera vez (solo esta vez, unos minutos)...");
        new Thread(() -> {
            String err = null;
            try {
                List<String[]> files = new ArrayList<>(); long total = 0;
                try (BufferedReader r = new BufferedReader(new InputStreamReader(getAssets().open("game/files.txt"), StandardCharsets.UTF_8))) {
                    String l; while ((l = r.readLine()) != null) { String[] f = l.split("\t"); if (f.length == 2) { files.add(f); total += Long.parseLong(f[1]); } }
                }
                File ext = getExternalFilesDir(null);
                if (ext.getUsableSpace() < total + (200L << 20)) throw new Exception(String.format(Locale.ROOT, "No hay espacio: se necesitan %.1f GB libres.", total / 1e9));
                new File(ext, ".unpacked").delete();
                long done = 0; byte[] buf = new byte[1 << 20]; int lastPct = -1;
                for (String[] f : files) {
                    File to = new File(ext, f[0]); to.getParentFile().mkdirs();
                    try (InputStream in = getAssets().open("game/" + f[0], AssetManager.ACCESS_STREAMING); OutputStream out = new FileOutputStream(to)) {
                        int n; while ((n = in.read(buf)) > 0) { out.write(buf, 0, n); done += n;
                            int pct = (int) (done * 100 / Math.max(1, total));
                            if (pct != lastPct) { lastPct = pct; final int p = pct; final long d = done, t = total; runOnUiThread(() -> { bar.setProgress(p);
                                status.setText(String.format(Locale.ROOT, "Preparando el juego por primera vez: %d%% (%.1f de %.1f GB)", p, d / 1e9, t / 1e9)); }); } }
                    }
                }
                try (OutputStream o = new FileOutputStream(new File(ext, ".unpacked"))) { o.write((versionCode() + "\n").getBytes(StandardCharsets.UTF_8)); }
            } catch (Exception e) { err = e.getMessage() != null ? e.getMessage() : e.toString(); }
            final String fe = err;
            runOnUiThread(() -> {
                unpacking = false;
                if (fe != null) { status.setText("No se pudo preparar el juego: " + fe); status.setTextColor(Color.rgb(255, 120, 120)); return; }
                status.setText("Juego listo."); bar.setVisibility(View.GONE); playButton.setEnabled(true); playButton.requestFocus();
            });
        }).start();
    }
    private boolean gameFilesPresent() {
        File ext = getExternalFilesDir(null);
        return ext != null && new File(ext, "romfs").isDirectory() && new File(ext, "dm").isDirectory() && new File(ext, "vk").isDirectory();
    }

    // The cheats of the bundled file, numbered as the host numbers them (dm/host/hle_cheat.c: [Name] entries with code; {Name} master codes always run and are not listed)
    private List<String> cheatNames() {
        List<String> out = new ArrayList<>();
        File f = new File(getFilesDir(), "cheats.txt");
        if (!f.exists()) return out;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new java.io.FileInputStream(f), StandardCharsets.UTF_8))) {
            String cur = null; boolean code = false; String l;
            while ((l = r.readLine()) != null) {
                l = l.trim();
                if (l.startsWith("[") || l.startsWith("{")) {
                    if (cur != null && code) out.add(cur);
                    int close = l.indexOf(l.startsWith("[") ? ']' : '}');
                    cur = l.startsWith("[") && close > 0 ? l.substring(1, close) : null; code = false;
                } else if (l.length() >= 8) code = true;
            }
            if (cur != null && code) out.add(cur);
        } catch (Exception e) { /* unreadable: no cheats */ }
        return out;
    }

    private TextView text(String s, float size, boolean bold) {
        TextView t = new TextView(this); t.setText(s); t.setTextSize(size); t.setTextColor(Color.WHITE);
        if (bold) t.setTypeface(t.getTypeface(), android.graphics.Typeface.BOLD);
        t.setPadding(0, 18, 0, 6); return t;
    }
    private Spinner spinner(LinearLayout parent, String label, String[] items, int sel) {
        parent.addView(text(label, 16, false));
        Spinner s = new Spinner(this);
        ArrayAdapter<String> a = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, items);
        a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        s.setAdapter(a); s.setSelection(Math.max(0, Math.min(sel, items.length - 1)));
        parent.addView(s); return s;
    }
    private void buildUi() {
        ScrollView sv = new ScrollView(this); sv.setBackgroundColor(Color.rgb(16, 18, 28));
        LinearLayout col = new LinearLayout(this); col.setOrientation(LinearLayout.VERTICAL); col.setPadding(56, 32, 56, 48);
        sv.addView(col, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        col.addView(text(getApplicationInfo().loadLabel(getPackageManager()) + " — Configuración", 24, true));
        col.addView(text("Elige cómo quieres jugar. Durante el juego, mantén − y + dos segundos para volver aquí.", 14, false));
        status = text("", 15, true); status.setVisibility(View.GONE); col.addView(status);
        bar = new android.widget.ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal); bar.setMax(100); bar.setVisibility(View.GONE); col.addView(bar);
        if (!gameFilesPresent() && !bundled()) {
            TextView w = text("Faltan los archivos del juego en " + getExternalFilesDir(null) + " (romfs, dm, vk). Cópialos desde la computadora y vuelve a abrir la app.", 15, true);
            w.setTextColor(Color.rgb(255, 120, 120)); col.addView(w);
        }
        mode = spinner(col, "Modo de consola", new String[]{"Portátil (más fluido)", "Dock (TV, más resolución)"}, prefs.getInt("opmode", 0) == 1 ? 1 : 0);
        fps = spinner(col, "Cuadros por segundo", new String[]{"60 (predeterminado)", "30 (ahorra batería)"}, prefs.getInt("fps", 60) == 30 ? 1 : 0);
        fit = spinner(col, "Pantalla", new String[]{"Mantener proporción (franjas negras)", "Llenar la pantalla (estirar)"}, prefs.getBoolean("stretch", false) ? 1 : 0);
        String sys = systemLang();
        String[] langs = {"es-419", "en-US", "system"};
        int ls = 0; for (int i = 0; i < langs.length; i++) if (langs[i].equals(prefs.getString("lang", "es-419"))) ls = i;
        lang = spinner(col, "Idioma", new String[]{"Español (Latinoamérica)", "English", "Idioma del sistema (" + sys + ")"}, ls);
        showFps = new CheckBox(this); showFps.setText("Mostrar los FPS en pantalla"); showFps.setTextColor(Color.WHITE); showFps.setChecked(prefs.getBoolean("showfps", false));
        col.addView(showFps);
        List<String> cheats = cheatNames();
        Set<String> on = prefs.getStringSet("cheats", new HashSet<>());
        if (cheats.isEmpty()) col.addView(text("Este juego no tiene trucos para esta versión.", 15, false));
        else {
            col.addView(text("Trucos (activa solo los que quieras)", 18, true));
            for (int i = 0; i < cheats.size(); i++) {
                CheckBox c = new CheckBox(this); c.setText(cheats.get(i)); c.setTextColor(Color.WHITE); c.setChecked(on.contains(String.valueOf(i + 1)));
                cheatBoxes.add(c); col.addView(c);
            }
        }
        skip = new CheckBox(this); skip.setText("No volver a mostrar este menú (− y + durante el juego lo abre otra vez)"); skip.setTextColor(Color.WHITE);
        skip.setChecked(prefs.getBoolean("skip", false)); col.addView(skip);
        Button play = playButton = new Button(this); play.setText("Jugar (START)"); play.setTextSize(20);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT); lp.topMargin = 28;
        play.setOnClickListener(v -> { save(); play(); });
        col.addView(play, lp);
        setContentView(sv);
    }
    private static String systemLang() {
        Locale l = Locale.getDefault(); String g = l.getLanguage(), c = l.getCountry();
        if (g.equals("es")) return c.equals("ES") ? "es" : "es-419";
        if (g.equals("en")) return c.equals("US") ? "en-US" : "en-GB";
        if (g.equals("pt")) return c.equals("BR") ? "pt-BR" : "pt";
        if (g.equals("fr")) return c.equals("CA") ? "fr-CA" : "fr";
        if (g.equals("zh")) return c.equals("TW") || c.equals("HK") ? "zh-Hant" : "zh-Hans";
        return g;
    }
    private void save() {
        Set<String> on = new HashSet<>(); StringBuilder list = new StringBuilder();
        for (int i = 0; i < cheatBoxes.size(); i++) if (cheatBoxes.get(i).isChecked()) { on.add(String.valueOf(i + 1)); if (list.length() > 0) list.append(','); list.append(i + 1); }
        String[] langs = {"es-419", "en-US", "system"};
        prefs.edit().putInt("opmode", mode.getSelectedItemPosition() == 1 ? 1 : 0).putInt("fps", fps.getSelectedItemPosition() == 1 ? 30 : 60)
             .putBoolean("stretch", fit.getSelectedItemPosition() == 1).putString("lang", langs[lang.getSelectedItemPosition()])
             .putBoolean("showfps", showFps.isChecked()).putStringSet("cheats", on).putBoolean("skip", skip.isChecked()).apply();
        String l = langs[lang.getSelectedItemPosition()]; if (l.equals("system")) l = systemLang();
        String env = "DM_OPMODE=" + (mode.getSelectedItemPosition() == 1 ? 1 : 0) + "\nDM_FPS=" + (fps.getSelectedItemPosition() == 1 ? 30 : 60)
                   + "\nDM_SCREEN_FIT=" + (fit.getSelectedItemPosition() == 1 ? "stretch" : "keep") + "\nDM_LANG=" + l + "\nDM_SHOW_FPS=" + (showFps.isChecked() ? 1 : 0)
                   + "\nDM_CHEATS_ON=" + list + "\n";
        try (OutputStream o = new FileOutputStream(new File(getFilesDir(), "settings.env"))) { o.write(env.getBytes(StandardCharsets.UTF_8)); } catch (Exception e) { /* the defaults apply */ }
    }
    private void play() {
        if (unpacking) return;
        if (!new File(getFilesDir(), "settings.env").exists()) save();
        launched = true;
        Intent i = new Intent(); i.setClassName(this, "android.app.NativeActivity");
        startActivity(i);
    }

    // A controller: A confirms like the centre of the D-pad, B goes back
    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        int k = e.getKeyCode();
        if (k == KeyEvent.KEYCODE_BUTTON_START || (k == KeyEvent.KEYCODE_BUTTON_A && getCurrentFocus() == null)) {   // nothing chosen yet (touch mode): A plays, as START always does
            // only a press that began here: the + still held from leaving the game with − and + must not start it again when it is let go
            if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0) playKeyDown = true;
            else if (e.getAction() == KeyEvent.ACTION_UP && playKeyDown) { playKeyDown = false; save(); play(); }
            return true;
        }
        if (k == KeyEvent.KEYCODE_BUTTON_A) return super.dispatchKeyEvent(new KeyEvent(e.getDownTime(), e.getEventTime(), e.getAction(), KeyEvent.KEYCODE_DPAD_CENTER, e.getRepeatCount()));
        if (k == KeyEvent.KEYCODE_BUTTON_B) return super.dispatchKeyEvent(new KeyEvent(e.getDownTime(), e.getEventTime(), e.getAction(), KeyEvent.KEYCODE_BACK, e.getRepeatCount()));
        return super.dispatchKeyEvent(e);
    }
}

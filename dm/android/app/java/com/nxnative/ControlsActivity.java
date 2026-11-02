// "Atur tombol": the on-screen controls' layout editor, opened from the launcher's menu. The controls are drawn over a plain background from the same sprite atlas the
// game draws them from (assets/controls), where the game puts them (ControlsLayout's geometry). Touch a group to select it (A B X Y, D-pad, L + ZL, R + ZR, − +,
// either stick), drag it to move it, and drag the round handle at its corner or pinch with two fingers to resize it. A floating stick is edited as its zone (where a
// finger that comes down starts it), a fixed one as its base. The panel (☰ shows and hides it, so what is under it can be reached too): the profile (choose, new,
// delete), the opacity, hiding the selected group or single buttons of it, a stick's mode (floating / fixed), back to the default layout, save and close.
// Saved in the app's files folder (ControlsLayout.Store: controls.json, and controls_active.txt for the game), on every close and when the editor is left.
package com.nxnative;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.DashPathEffect;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Shader;
import android.os.Bundle;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public class ControlsActivity extends Activity {
    private ControlsLayout.Store store;
    private EditorView editor;
    private LinearLayout panel, buttonsRow;
    private Spinner profiles;
    private SeekBar opacity;
    private TextView selLabel;
    private CheckBox hideGroup, fixedStick;
    private boolean filling;   // the panel is being filled from the layout: its listeners change nothing
    private final Map<String, Bitmap> sprites = new HashMap<>();

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        loadSprites();
        FrameLayout root = new FrameLayout(this);
        editor = new EditorView(this);
        root.addView(editor, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        panel = buildPanel();
        FrameLayout.LayoutParams pl = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
        root.addView(panel, pl);
        Button toggle = new Button(this); toggle.setText("☰"); toggle.setTextSize(18); toggle.setAlpha(0.85f);
        toggle.setOnClickListener(v -> panel.setVisibility(panel.getVisibility() == View.VISIBLE ? View.GONE : View.VISIBLE));
        root.addView(toggle, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.CENTER_HORIZONTAL));
        setContentView(root);
    }
    @Override protected void onResume() {
        super.onResume();
        WindowInsetsController c = getWindow().getInsetsController();   // the whole screen, as the game has it (immersive)
        if (c != null) { c.hide(WindowInsets.Type.systemBars()); c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE); }
    }
    @Override protected void onPause() { super.onPause(); save(false); }

    private void loadSprites() {   // the atlas and its cell names (dm/android/tools/controls_atlas.py)
        try (InputStream in = getAssets().open("controls/atlas.png"); BufferedReader r = new BufferedReader(new InputStreamReader(getAssets().open("controls/atlas.txt"), StandardCharsets.UTF_8))) {
            Bitmap atlas = BitmapFactory.decodeStream(in); int cell = 0, cols = 0; String l;
            while ((l = r.readLine()) != null) {
                String[] f = l.trim().split("\\s+"); if (f.length < 2 || f[0].startsWith("#")) continue;
                if (f[0].equals("cell")) { cell = Integer.parseInt(f[1]); cols = Integer.parseInt(f[2]); continue; }
                int i = Integer.parseInt(f[1]); if (cell > 0) sprites.put(f[0], Bitmap.createBitmap(atlas, (i % cols) * cell, (i / cols) * cell, cell, cell));
            }
        } catch (Exception e) { /* no atlas: the editor draws plain circles */ }
    }

    // ---- Files ----
    private File json() { return new File(getFilesDir(), "controls.json"); }
    private void load(float w, float h) {
        String s = "";
        try (InputStream in = new FileInputStream(json())) {   // (InputStream.readAllBytes is Android 13 and later; the app runs from Android 11)
            java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream(); byte[] buf = new byte[8192]; int n; while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
            s = new String(o.toByteArray(), StandardCharsets.UTF_8);
        } catch (Exception e) { /* none yet: the defaults */ }
        store = ControlsLayout.Store.parse(s, w, h);
        fillPanel();
    }
    private void save(boolean say) {
        if (store == null) return;
        try (FileOutputStream o = new FileOutputStream(json())) { o.write(store.toJson().getBytes(StandardCharsets.UTF_8)); }
        catch (Exception e) { Toast.makeText(this, "Tidak tersimpan: " + e.getMessage(), Toast.LENGTH_LONG).show(); return; }
        try (FileOutputStream o = new FileOutputStream(new File(getFilesDir(), "controls_active.txt"))) { o.write(store.current().toNative().getBytes(StandardCharsets.UTF_8)); }
        catch (Exception e) { Toast.makeText(this, "Tidak tersimpan: " + e.getMessage(), Toast.LENGTH_LONG).show(); return; }
        if (say) Toast.makeText(this, "Tata letak \"" + store.active + "\" tersimpan", Toast.LENGTH_SHORT).show();
    }
    private ControlsLayout L() { return store.current(); }

    // ---- The panel ----
    private Button button(String t, View.OnClickListener l) { Button b = new Button(this); b.setText(t); b.setAllCaps(false); b.setOnClickListener(l); return b; }
    private TextView label(String t) { TextView v = new TextView(this); v.setText(t); v.setTextColor(Color.WHITE); v.setTextSize(15); v.setPadding(12, 0, 12, 0); return v; }
    private LinearLayout buildPanel() {
        LinearLayout p = new LinearLayout(this); p.setOrientation(LinearLayout.VERTICAL); p.setPadding(24, 16, 24, 16); p.setBackgroundColor(Color.argb(215, 20, 22, 28));
        TextView title = label("Atur tombol"); title.setTextSize(20); p.addView(title);
        LinearLayout r1 = new LinearLayout(this); r1.setGravity(Gravity.CENTER_VERTICAL);
        r1.addView(label("Profil"));
        profiles = new Spinner(this); r1.addView(profiles);
        profiles.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> a, View v, int pos, long id) { if (filling) return; store.active = (String) a.getItemAtPosition(pos); editor.sel = -1; fillPanel(); }
            @Override public void onNothingSelected(AdapterView<?> a) {}
        });
        r1.addView(button("Baru", v -> newProfile()));
        r1.addView(button("Hapus", v -> deleteProfile()));
        r1.addView(button("Default", v -> resetProfile()));
        r1.addView(button("Simpan & tutup", v -> { save(true); finish(); }));
        p.addView(r1);
        LinearLayout r2 = new LinearLayout(this); r2.setGravity(Gravity.CENTER_VERTICAL);
        r2.addView(label("Transparansi"));
        opacity = new SeekBar(this); opacity.setMax(90);
        opacity.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int v, boolean user) { if (filling || !user) return; L().opacity = (10 + v) / 100f; editor.invalidate(); }
            @Override public void onStartTrackingTouch(SeekBar s) {} @Override public void onStopTrackingTouch(SeekBar s) {}
        });
        r2.addView(opacity, new LinearLayout.LayoutParams(360, ViewGroup.LayoutParams.WRAP_CONTENT));
        p.addView(r2);
        LinearLayout r3 = new LinearLayout(this); r3.setGravity(Gravity.CENTER_VERTICAL);
        selLabel = label(""); r3.addView(selLabel);
        hideGroup = new CheckBox(this); hideGroup.setText("Sembunyikan"); hideGroup.setTextColor(Color.WHITE);
        hideGroup.setOnCheckedChangeListener((v, on) -> { if (filling || editor.sel < 0) return; L().hidden[editor.sel] = on; editor.invalidate(); });
        r3.addView(hideGroup);
        fixedStick = new CheckBox(this); fixedStick.setText("Posisi tetap"); fixedStick.setTextColor(Color.WHITE);
        fixedStick.setOnCheckedChangeListener((v, on) -> { if (filling || !ControlsLayout.isStick(editor.sel)) return; L().fixed[editor.sel - ControlsLayout.LS] = on; editor.invalidate(); });
        r3.addView(fixedStick);
        p.addView(r3);
        buttonsRow = new LinearLayout(this); buttonsRow.setGravity(Gravity.CENTER_VERTICAL); p.addView(buttonsRow);
        return p;
    }
    private void fillPanel() {   // the panel from the layout and the selection
        if (store == null) return;
        filling = true;
        List<String> names = new ArrayList<>(store.profiles.keySet());
        profiles.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, names));
        profiles.setSelection(names.indexOf(store.active));
        opacity.setProgress(Math.round(L().opacity * 100) - 10);
        int g = editor.sel;
        selLabel.setText(g < 0 ? "Sentuh sebuah grup untuk memilihnya" : ControlsLayout.GROUP_LABEL[g] + ":");
        hideGroup.setVisibility(g < 0 ? View.GONE : View.VISIBLE); if (g >= 0) hideGroup.setChecked(L().hidden[g]);
        fixedStick.setVisibility(ControlsLayout.isStick(g) ? View.VISIBLE : View.GONE); if (ControlsLayout.isStick(g)) fixedStick.setChecked(L().fixed[g - ControlsLayout.LS]);
        buttonsRow.removeAllViews();
        if (g >= 0 && !ControlsLayout.isStick(g)) {
            buttonsRow.addView(label("Tampilkan:"));
            for (ControlsLayout.Btn btn : ControlsLayout.BUTTONS) {
                if (btn.group != g) continue;
                CheckBox c = new CheckBox(this); c.setText(nice(btn.name)); c.setTextColor(Color.WHITE); c.setChecked(!L().hiddenButtons.contains(btn.name));
                c.setOnCheckedChangeListener((v, on) -> { if (on) L().hiddenButtons.remove(btn.name); else L().hiddenButtons.add(btn.name); editor.invalidate(); });
                buttonsRow.addView(c);
            }
        }
        filling = false;
        editor.invalidate();
    }
    private static String nice(String n) { switch (n) { case "UP": return "↑"; case "DOWN": return "↓"; case "LEFT": return "←"; case "RIGHT": return "→"; case "MINUS": return "−"; case "PLUS": return "+"; default: return n; } }
    private void newProfile() {
        EditText e = new EditText(this); int k = store.profiles.size() + 1; while (store.profiles.containsKey("Profil " + k)) k++; e.setText("Profil " + k);
        new AlertDialog.Builder(this).setTitle("Profil baru (salinan dari \"" + store.active + "\")").setView(e)
            .setPositiveButton("Buat", (d, w) -> { String n = e.getText().toString().trim(); if (n.isEmpty() || store.profiles.containsKey(n)) { Toast.makeText(this, "Nama sudah dipakai atau kosong", Toast.LENGTH_SHORT).show(); return; }
                                                 store.profiles.put(n, L().copy(n)); store.active = n; editor.sel = -1; fillPanel(); })
            .setNegativeButton("Batal", null).show();
    }
    private void deleteProfile() {
        if (store.profiles.size() < 2) { Toast.makeText(this, "Profil terakhir tidak bisa dihapus", Toast.LENGTH_SHORT).show(); return; }
        new AlertDialog.Builder(this).setMessage("Hapus profil \"" + store.active + "\"?")
            .setPositiveButton("Hapus", (d, w) -> { store.profiles.remove(store.active); store.active = store.profiles.keySet().iterator().next(); editor.sel = -1; fillPanel(); })
            .setNegativeButton("Batal", null).show();
    }
    private void resetProfile() {
        new AlertDialog.Builder(this).setMessage("Kembalikan \"" + store.active + "\" ke tata letak default?")
            .setPositiveButton("Ya", (d, w) -> { ControlsLayout n = ControlsLayout.defaults(editor.getWidth(), editor.getHeight()); n.name = store.active; store.profiles.put(store.active, n); editor.sel = -1; fillPanel(); })
            .setNegativeButton("Batal", null).show();
    }

    // ---- The preview: the screen as the game has it, its controls where the game puts them ----
    final class EditorView extends View {
        int sel = -1;
        private int mode;   // 0 nothing, 1 moving, 2 resizing (the handle), 3 pinching
        private float dx, dy, s0, d0, fx0, fy0; private final float[] z0 = new float[4];
        private final Paint pBg = new Paint(), pSpr = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG), pSel = new Paint(Paint.ANTI_ALIAS_FLAG), pZone = new Paint(), pZoneEdge = new Paint(Paint.ANTI_ALIAS_FLAG),
                            pText = new Paint(Paint.ANTI_ALIAS_FLAG), pCross = new Paint(Paint.ANTI_ALIAS_FLAG), pHandle = new Paint(Paint.ANTI_ALIAS_FLAG);
        EditorView(Context c) {
            super(c);
            pSel.setStyle(Paint.Style.STROKE); pSel.setStrokeWidth(5); pSel.setColor(Color.rgb(255, 214, 64)); pSel.setPathEffect(new DashPathEffect(new float[]{18, 12}, 0));
            pZone.setColor(Color.argb(40, 90, 160, 255)); pZoneEdge.setStyle(Paint.Style.STROKE); pZoneEdge.setStrokeWidth(3); pZoneEdge.setColor(Color.argb(140, 120, 180, 255));
            pText.setColor(Color.argb(200, 255, 255, 255)); pText.setTextSize(34);
            pCross.setColor(Color.rgb(255, 90, 90)); pCross.setStrokeWidth(7); pCross.setStrokeCap(Paint.Cap.ROUND);
            pHandle.setColor(Color.rgb(255, 214, 64));
        }
        @Override protected void onSizeChanged(int w, int h, int ow, int oh) {
            pBg.setShader(new LinearGradient(0, 0, 0, h, Color.rgb(44, 52, 66), Color.rgb(18, 20, 26), Shader.TileMode.CLAMP));
            if (store == null) load(w, h);
        }
        private RectF rf(float[] b) { return new RectF(b[0], b[1], b[0] + b[2], b[1] + b[3]); }
        private void sprite(Canvas c, String name, float[] box, float alpha) {
            Bitmap bm = sprites.get(name); pSpr.setAlpha(Math.round(255 * alpha));
            if (bm != null) c.drawBitmap(bm, null, rf(box), pSpr);
            else { Paint p = new Paint(Paint.ANTI_ALIAS_FLAG); p.setColor(Color.argb(Math.round(255 * alpha), 90, 96, 108)); c.drawOval(rf(box), p); }
        }
        private void cross(Canvas c, float[] b) { c.drawLine(b[0] + b[2] * 0.2f, b[1] + b[3] * 0.2f, b[0] + b[2] * 0.8f, b[1] + b[3] * 0.8f, pCross); c.drawLine(b[0] + b[2] * 0.8f, b[1] + b[3] * 0.2f, b[0] + b[2] * 0.2f, b[1] + b[3] * 0.8f, pCross); }
        @Override protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight(); c.drawRect(0, 0, w, h, pBg);
            if (store == null) return;
            ControlsLayout l = L(); float a = l.opacity;
            for (int k = 0; k < 2; k++) {   // the sticks: a floating one's zone, its base shown in the middle; a fixed one's base
                int g = ControlsLayout.LS + k; float ga = l.hidden[g] ? 0.2f : 1;
                if (!l.fixed[k]) { float[] z = l.zoneBox(k, w, h); if (!l.hidden[g]) c.drawRect(rf(z), pZone); c.drawRect(rf(z), pZoneEdge);
                                   c.drawText(ControlsLayout.GROUP_LABEL[g] + " (mengambang: area)", z[0] + 24, z[1] + z[3] - 28, pText); }
                sprite(c, "stick_base", l.stickBox(k, w, h), a * ga * (l.fixed[k] ? 1 : 0.5f)); sprite(c, "stick_knob", l.knobBox(k, w, h), a * ga * (l.fixed[k] ? 1 : 0.5f));
                if (l.hidden[g]) cross(c, l.stickBox(k, w, h));
            }
            for (int i = 0; i < ControlsLayout.BUTTONS.length; i++) {
                ControlsLayout.Btn b = ControlsLayout.BUTTONS[i]; boolean off = l.hidden[b.group] || l.hiddenButtons.contains(b.name); float[] box = l.buttonBox(i, w, h);
                sprite(c, b.sprite, box, a * (off ? 0.2f : 1)); if (off) cross(c, box);
            }
            if (sel >= 0) { float[] gb = l.groupBox(sel, w, h); RectF r = rf(gb); r.inset(-10, -10); c.drawRect(r, pSel); c.drawCircle(r.right, r.bottom, handleR(), pHandle); }
        }
        private float handleR() { return getHeight() * 0.025f; }
        private int hit(float px, float py) {   // the group under the finger: buttons first, then fixed sticks, then zones
            ControlsLayout l = L(); float w = getWidth(), h = getHeight();
            for (int g = 0; g < ControlsLayout.LS; g++) { float[] b = l.groupBox(g, w, h); if (px >= b[0] && px <= b[0] + b[2] && py >= b[1] && py <= b[1] + b[3]) return g; }
            for (int k = 0; k < 2; k++) if (l.fixed[k]) { float[] b = l.stickBox(k, w, h); if (px >= b[0] && px <= b[0] + b[2] && py >= b[1] && py <= b[1] + b[3]) return ControlsLayout.LS + k; }
            for (int k = 0; k < 2; k++) if (!l.fixed[k]) { float[] b = l.zoneBox(k, w, h); if (px >= b[0] && px <= b[0] + b[2] && py >= b[1] && py <= b[1] + b[3]) return ControlsLayout.LS + k; }
            return -1;
        }
        private boolean isZone(int g) { return ControlsLayout.isStick(g) && !L().fixed[g - ControlsLayout.LS]; }
        @Override public boolean onTouchEvent(MotionEvent e) {
            if (store == null) return true;
            ControlsLayout l = L(); float w = getWidth(), h = getHeight(), px = e.getX(), py = e.getY();
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN: {
                    mode = 0;
                    if (sel >= 0) { float[] gb = l.groupBox(sel, w, h); if (Math.hypot(px - (gb[0] + gb[2] + 10), py - (gb[1] + gb[3] + 10)) < 2.2f * handleR()) { mode = 2; start(px, py); return true; } }
                    int g = hit(px, py); if (g != sel) { sel = g; fillPanel(); }
                    if (sel >= 0) { mode = 1; start(px, py); }
                    invalidate(); return true;
                }
                case MotionEvent.ACTION_POINTER_DOWN:
                    if (sel >= 0 && e.getPointerCount() == 2) { mode = 3; start(px, py); d0 = span(e); }
                    return true;
                case MotionEvent.ACTION_MOVE: {
                    if (sel < 0 || mode == 0) return true;
                    int g = sel; float[] z = isZone(g) ? l.zone[g - ControlsLayout.LS] : null;
                    if (mode == 1) {
                        if (z != null) { z[0] = ControlsLayout.clamp(z0[0] + (px - fx0) / w, 0, 1 - z[2]); z[1] = ControlsLayout.clamp(z0[1] + (py - fy0) / h, 0, 1 - z[3]); }
                        else { l.x[g] = ControlsLayout.clamp((px - dx) / w, 0, 1); l.y[g] = ControlsLayout.clamp((py - dy) / h, 0, 1); }
                    } else if (mode == 2) {
                        if (z != null) { z[2] = ControlsLayout.clamp(z0[2] + (px - fx0) / w, 0.1f, 1 - z[0]); z[3] = ControlsLayout.clamp(z0[3] + (py - fy0) / h, 0.1f, 1 - z[1]); }
                        else { float cx = l.x[g] * w, cy = l.y[g] * h, a = (float) Math.hypot(fx0 - cx, fy0 - cy), b = (float) Math.hypot(px - cx, py - cy); if (a > 1) l.s[g] = ControlsLayout.clamp(s0 * b / a, 0.3f, 3); }
                    } else if (mode == 3 && e.getPointerCount() >= 2 && d0 > 1) {
                        float f = span(e) / d0;
                        if (z != null) { float cx = z0[0] + z0[2] / 2, cy = z0[1] + z0[3] / 2; z[2] = ControlsLayout.clamp(z0[2] * f, 0.1f, 1); z[3] = ControlsLayout.clamp(z0[3] * f, 0.1f, 1);
                                         z[0] = ControlsLayout.clamp(cx - z[2] / 2, 0, 1 - z[2]); z[1] = ControlsLayout.clamp(cy - z[3] / 2, 0, 1 - z[3]); }
                        else l.s[g] = ControlsLayout.clamp(s0 * f, 0.3f, 3);
                    }
                    invalidate(); return true;
                }
                case MotionEvent.ACTION_UP: case MotionEvent.ACTION_CANCEL: mode = 0; return true;
                case MotionEvent.ACTION_POINTER_UP: if (mode == 3) mode = 0; return true;
            }
            return true;
        }
        private void start(float px, float py) {
            ControlsLayout l = L(); int g = sel; fx0 = px; fy0 = py; s0 = l.s[g];
            dx = px - l.x[g] * getWidth(); dy = py - l.y[g] * getHeight();
            if (ControlsLayout.isStick(g)) System.arraycopy(l.zone[g - ControlsLayout.LS], 0, z0, 0, 4);
        }
        private float span(MotionEvent e) { return (float) Math.hypot(e.getX(0) - e.getX(1), e.getY(0) - e.getY(1)); }
    }
}

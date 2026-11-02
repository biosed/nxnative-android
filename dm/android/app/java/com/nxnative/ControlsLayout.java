// The on-screen controls' layout: groups of buttons (face A B X Y, D-pad, L+ZL, R+ZR, − +) and the two sticks, each placed by its centre as a fraction of the screen's
// width and height and sized by a scale on its default size (sizes are in units of the screen's height, as app/front_android.c lays them out), so a layout keeps its
// look across rotations and resolutions. Several named profiles, one active; an opacity; groups and single buttons can be hidden; each stick floats inside a zone
// (it appears where the finger lands, as before) or sits at a fixed place.
// Plain Java (no Android classes): the launcher's editor (ControlsActivity) uses it, and so does the preview renderer on a computer (tools/ControlsPreview.java).
// Files, in the app's files folder: controls.json (every profile, the editor's own) and controls_active.txt (the active profile flattened into lines the game's native
// side reads at launch, front_android.c ctl_load). With neither file the game lays the controls out as it always did; defaults(w, h) gives that same layout.
package com.nxnative;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

public final class ControlsLayout {
    public static final String[] GROUPS = {"face", "dpad", "lzl", "rzr", "mp", "ls", "rs"};
    public static final String[] GROUP_LABEL = {"A B X Y", "D-pad", "L + ZL", "R + ZR", "− +", "Stik kiri", "Stik kanan"};
    public static final int FACE = 0, DPAD = 1, LZL = 2, RZR = 3, MP = 4, LS = 5, RS = 6, NG = 7;
    // name, sprite, group, offset from the group's centre and radius, in units of the screen's height at scale 1 (front_android.c BTN[] holds the same table)
    public static final class Btn { public final String name, sprite; public final int group; public final float dx, dy, r;
        Btn(String n, String sp, int g, float dx, float dy, float r) { name = n; sprite = sp; group = g; this.dx = dx; this.dy = dy; this.r = r; } }
    public static final Btn[] BUTTONS = {
        new Btn("A", "btn_a", FACE, 0.12f, 0, 0.065f), new Btn("B", "btn_b", FACE, 0, 0.12f, 0.065f), new Btn("X", "btn_x", FACE, 0, -0.12f, 0.065f), new Btn("Y", "btn_y", FACE, -0.12f, 0, 0.065f),
        new Btn("UP", "dpad_up", DPAD, 0, -0.1f, 0.05f), new Btn("DOWN", "dpad_down", DPAD, 0, 0.1f, 0.05f), new Btn("LEFT", "dpad_left", DPAD, -0.1f, 0, 0.05f), new Btn("RIGHT", "dpad_right", DPAD, 0.1f, 0, 0.05f),
        new Btn("L", "btn_l", LZL, 0, -0.085f, 0.065f), new Btn("ZL", "btn_zl", LZL, 0, 0.085f, 0.065f), new Btn("R", "btn_r", RZR, 0, -0.085f, 0.065f), new Btn("ZR", "btn_zr", RZR, 0, 0.085f, 0.065f),
        new Btn("MINUS", "btn_minus", MP, -0.1f, 0, 0.065f), new Btn("PLUS", "btn_plus", MP, 0.1f, 0, 0.065f)};
    public static final float STICK_R = 0.12f, KNOB_R = 0.055f;   // the stick's travel (its base's radius) and its knob's, in units of the screen's height at scale 1

    public String name = "Default";
    public float opacity = 0.75f;
    public final float[] x = new float[NG], y = new float[NG], s = new float[NG];   // centre (fractions of width / height) and scale; for a stick, its fixed place
    public final boolean[] hidden = new boolean[NG];
    public final boolean[] fixed = new boolean[2];             // ls, rs: fixed (true) or floating in their zone
    public final float[][] zone = new float[2][4];             // ls, rs: the floating stick's zone, x y w h as fractions of the screen
    public final Set<String> hiddenButtons = new LinkedHashSet<>();

    // Today's layout on a w x h screen (app/front_android.c, before the editor existed)
    public static ControlsLayout defaults(float w, float h) {
        ControlsLayout l = new ControlsLayout();
        l.set(FACE, (w - 0.24f * h) / w, 0.75f); l.set(DPAD, 0.22f * h / w, 0.75f); l.set(LZL, 0.11f * h / w, 0.195f); l.set(RZR, (w - 0.11f * h) / w, 0.195f); l.set(MP, 0.5f, 0.08f);
        l.set(LS, 0.18f, 0.45f); l.set(RS, 0.82f, 0.45f);
        l.zone[0] = new float[]{0, 0, 0.5f, 1}; l.zone[1] = new float[]{0.5f, 0, 0.5f, 1};
        return l;
    }
    private void set(int g, float cx, float cy) { x[g] = cx; y[g] = cy; s[g] = 1; }
    public ControlsLayout copy(String newName) { ControlsLayout c = fromJson(toJson()); c.name = newName; return c; }
    public static boolean isStick(int g) { return g == LS || g == RS; }

    // ---- Geometry on a w x h screen, in pixels: what the game draws (front_android.c vb_layout / front_overlay), so the editor and the preview show the same ----
    public float[] buttonBox(int i, float w, float h) {   // left, top, width, height
        Btn b = BUTTONS[i]; int g = b.group; float cx = x[g] * w + b.dx * h * s[g], cy = y[g] * h + b.dy * h * s[g], r = b.r * h * s[g];
        return new float[]{cx - r, cy - r, 2 * r, 2 * r};
    }
    public float[] stickBox(int k, float w, float h) {   // the stick's base: at its fixed place, or (floating) shown at the middle of its zone
        int g = LS + k; float r = STICK_R * h * s[g];
        float cx = fixed[k] ? x[g] * w : (zone[k][0] + zone[k][2] / 2) * w, cy = fixed[k] ? y[g] * h : (zone[k][1] + zone[k][3] / 2) * h;
        return new float[]{cx - r, cy - r, 2 * r, 2 * r};
    }
    public float[] knobBox(int k, float w, float h) { float[] b = stickBox(k, w, h); float kr = KNOB_R * h * s[LS + k], cx = b[0] + b[2] / 2, cy = b[1] + b[3] / 2; return new float[]{cx - kr, cy - kr, 2 * kr, 2 * kr}; }
    public float[] zoneBox(int k, float w, float h) { return new float[]{zone[k][0] * w, zone[k][1] * h, zone[k][2] * w, zone[k][3] * h}; }
    public float[] groupBox(int g, float w, float h) {   // what selects and moves a group: its buttons' box, a fixed stick's base, a floating stick's zone
        if (isStick(g)) return fixed[g - LS] ? stickBox(g - LS, w, h) : zoneBox(g - LS, w, h);
        float l = Float.MAX_VALUE, t = Float.MAX_VALUE, r = -Float.MAX_VALUE, b = -Float.MAX_VALUE;
        for (int i = 0; i < BUTTONS.length; i++) if (BUTTONS[i].group == g) { float[] q = buttonBox(i, w, h); l = Math.min(l, q[0]); t = Math.min(t, q[1]); r = Math.max(r, q[0] + q[2]); b = Math.max(b, q[1] + q[3]); }
        return new float[]{l, t, r - l, b - t};
    }

    // ---- The native side's lines (front_android.c ctl_load) ----
    public String toNative() {
        StringBuilder b = new StringBuilder("# on-screen controls, profile ").append(name.replace('\n', ' ')).append(" (written by the launcher; fractions of the screen, scales of the default size)\n");
        b.append(String.format(Locale.ROOT, "opacity %.3f\n", opacity));
        for (int g = 0; g < NG; g++) b.append(String.format(Locale.ROOT, "group %s %.5f %.5f %.4f %d\n", GROUPS[g], x[g], y[g], s[g], hidden[g] ? 1 : 0));
        for (int k = 0; k < 2; k++) b.append(String.format(Locale.ROOT, "stick %s %d %.5f %.5f %.5f %.5f\n", GROUPS[LS + k], fixed[k] ? 1 : 0, zone[k][0], zone[k][1], zone[k][2], zone[k][3]));
        for (String n : hiddenButtons) b.append("hide ").append(n).append('\n');
        return b.toString();
    }

    // ---- JSON (the editor's file) ----
    public String toJson() {
        StringBuilder b = new StringBuilder("{\"name\":").append(Json.str(name)).append(String.format(Locale.ROOT, ",\"opacity\":%.3f,\"groups\":{", opacity));
        for (int g = 0; g < NG; g++) b.append(g > 0 ? "," : "").append('"').append(GROUPS[g]).append('"')
                .append(String.format(Locale.ROOT, ":{\"x\":%.5f,\"y\":%.5f,\"s\":%.4f,\"hidden\":%b}", x[g], y[g], s[g], hidden[g]));
        b.append("},\"sticks\":{");
        for (int k = 0; k < 2; k++) b.append(k > 0 ? "," : "").append('"').append(GROUPS[LS + k]).append('"')
                .append(String.format(Locale.ROOT, ":{\"fixed\":%b,\"zone\":[%.5f,%.5f,%.5f,%.5f]}", fixed[k], zone[k][0], zone[k][1], zone[k][2], zone[k][3]));
        b.append("},\"hiddenButtons\":[");
        int i = 0; for (String n : hiddenButtons) b.append(i++ > 0 ? "," : "").append(Json.str(n));
        return b.append("]}").toString();
    }
    public static ControlsLayout fromJson(String s) { return from(Json.parse(s), null); }
    @SuppressWarnings("unchecked")
    static ControlsLayout from(Object o, ControlsLayout base) {   // base: the defaults whatever the file leaves out takes
        ControlsLayout l = base != null ? base : defaults(16, 9);
        Map<String, Object> m = (Map<String, Object>) o;
        if (m.get("name") instanceof String) l.name = (String) m.get("name");
        if (m.get("opacity") instanceof Number) l.opacity = clamp(((Number) m.get("opacity")).floatValue(), 0.1f, 1);
        Object gs = m.get("groups");
        if (gs instanceof Map) for (int g = 0; g < NG; g++) { Object e = ((Map<String, Object>) gs).get(GROUPS[g]); if (!(e instanceof Map)) continue; Map<String, Object> q = (Map<String, Object>) e;
            if (q.get("x") instanceof Number) l.x[g] = ((Number) q.get("x")).floatValue(); if (q.get("y") instanceof Number) l.y[g] = ((Number) q.get("y")).floatValue();
            if (q.get("s") instanceof Number) l.s[g] = clamp(((Number) q.get("s")).floatValue(), 0.3f, 3); if (q.get("hidden") instanceof Boolean) l.hidden[g] = (Boolean) q.get("hidden"); }
        Object st = m.get("sticks");
        if (st instanceof Map) for (int k = 0; k < 2; k++) { Object e = ((Map<String, Object>) st).get(GROUPS[LS + k]); if (!(e instanceof Map)) continue; Map<String, Object> q = (Map<String, Object>) e;
            if (q.get("fixed") instanceof Boolean) l.fixed[k] = (Boolean) q.get("fixed");
            if (q.get("zone") instanceof List && ((List<Object>) q.get("zone")).size() == 4) for (int i = 0; i < 4; i++) l.zone[k][i] = ((Number) ((List<Object>) q.get("zone")).get(i)).floatValue(); }
        Object hb = m.get("hiddenButtons");
        if (hb instanceof List) { l.hiddenButtons.clear(); for (Object n : (List<Object>) hb) if (n instanceof String) l.hiddenButtons.add((String) n); }
        return l;
    }
    static float clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

    // ---- Profiles: the editor's file holds them all and names the active one ----
    public static final class Store {
        public String active = "Default";
        public final LinkedHashMap<String, ControlsLayout> profiles = new LinkedHashMap<>();
        public ControlsLayout current() { return profiles.get(active); }
        public static Store parse(String json, float w, float h) {   // w, h: the screen, for the defaults of anything missing
            Store st = new Store();
            try {
                @SuppressWarnings("unchecked") Map<String, Object> m = (Map<String, Object>) Json.parse(json);
                if (m.get("profiles") instanceof List) for (Object p : (List<?>) m.get("profiles")) { ControlsLayout l = from(p, defaults(w, h)); st.profiles.put(l.name, l); }
                if (m.get("active") instanceof String) st.active = (String) m.get("active");
            } catch (RuntimeException e) { st.profiles.clear(); }
            if (st.profiles.isEmpty()) { ControlsLayout d = defaults(w, h); st.profiles.put(d.name, d); }
            if (!st.profiles.containsKey(st.active)) st.active = st.profiles.keySet().iterator().next();
            return st;
        }
        public String toJson() {
            StringBuilder b = new StringBuilder("{\"version\":1,\"active\":").append(Json.str(active)).append(",\"profiles\":[\n");
            int i = 0; for (ControlsLayout l : profiles.values()) b.append(i++ > 0 ? ",\n" : "").append(l.toJson());
            return b.append("\n]}\n").toString();
        }
    }

    // ---- A small JSON reader and string writer (objects, arrays, strings, numbers, true / false / null) ----
    static final class Json {
        private final String s; private int p;
        private Json(String s) { this.s = s; }
        static Object parse(String s) { Json j = new Json(s); Object o = j.val(); j.ws(); if (j.p != s.length()) throw new IllegalArgumentException("JSON: trailing text at " + j.p); return o; }
        static String str(String v) {
            StringBuilder b = new StringBuilder("\"");
            for (char c : v.toCharArray()) { if (c == '"' || c == '\\') b.append('\\').append(c); else if (c < 0x20) b.append(String.format(Locale.ROOT, "\\u%04x", (int) c)); else b.append(c); }
            return b.append('"').toString();
        }
        private void ws() { while (p < s.length() && Character.isWhitespace(s.charAt(p))) p++; }
        private Object val() {
            ws(); if (p >= s.length()) throw new IllegalArgumentException("JSON: end of text");
            char c = s.charAt(p);
            if (c == '{') { p++; Map<String, Object> m = new LinkedHashMap<>(); ws(); if (s.charAt(p) == '}') { p++; return m; }
                for (;;) { ws(); String k = (String) val(); ws(); expect(':'); m.put(k, val()); ws(); if (s.charAt(p) == ',') { p++; continue; } expect('}'); return m; } }
            if (c == '[') { p++; List<Object> a = new ArrayList<>(); ws(); if (s.charAt(p) == ']') { p++; return a; }
                for (;;) { a.add(val()); ws(); if (s.charAt(p) == ',') { p++; continue; } expect(']'); return a; } }
            if (c == '"') { p++; StringBuilder b = new StringBuilder();
                for (;;) { char d = s.charAt(p++); if (d == '"') return b.toString();
                    if (d == '\\') { char e = s.charAt(p++); if (e == 'u') { b.append((char) Integer.parseInt(s.substring(p, p + 4), 16)); p += 4; } else b.append(e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e); }
                    else b.append(d); } }
            if (s.startsWith("true", p)) { p += 4; return Boolean.TRUE; }
            if (s.startsWith("false", p)) { p += 5; return Boolean.FALSE; }
            if (s.startsWith("null", p)) { p += 4; return null; }
            int q = p; while (p < s.length() && "+-0123456789.eE".indexOf(s.charAt(p)) >= 0) p++;
            if (q == p) throw new IllegalArgumentException("JSON: unexpected '" + c + "' at " + p);
            return Double.parseDouble(s.substring(q, p));
        }
        private void expect(char c) { if (p >= s.length() || s.charAt(p) != c) throw new IllegalArgumentException("JSON: '" + c + "' expected at " + p); p++; }
    }
}

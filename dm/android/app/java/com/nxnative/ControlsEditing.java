// What the layout editor's canvas does with a finger, apart from Android (ControlsActivity's EditorView feeds it touches; tools/ControlsEditingTest.java checks it
// on a computer): which group a finger selects -- the smallest one under it, so a button inside a floating stick's zone is the button --, moving it, resizing it by
// its corner handle or by pinching, and the profile list's choices, of which only a real change of profile clears the selection (the list's widget reports its
// own selection again whenever it is refilled; taken as a change, it cleared the selection after every touch and refilled the panel over and over).
package com.nxnative;

public final class ControlsEditing {
    public int sel = -1;
    private int mode;   // 0 nothing, 1 moving, 2 resizing (the handle), 3 pinching
    private float dx, dy, s0, d0, fx0, fy0; private final float[] z0 = new float[4];

    // The group under (px, py): the smallest of those whose box holds it (a hidden one too, so it can be shown again); -1 none
    public static int hit(ControlsLayout l, float px, float py, float w, float h) {
        int best = -1; float area = Float.MAX_VALUE;
        for (int g = 0; g < ControlsLayout.NG; g++) {
            float[] b = l.groupBox(g, w, h);
            if (px < b[0] || px > b[0] + b[2] || py < b[1] || py > b[1] + b[3]) continue;
            float a = b[2] * b[3]; if (a < area) { area = a; best = g; }
        }
        return best;
    }
    public static float[] handle(ControlsLayout l, int g, float w, float h) { float[] b = l.groupBox(g, w, h); return new float[]{b[0] + b[2] + 10, b[1] + b[3] + 10}; }   // the resize handle's centre
    private static boolean isZone(ControlsLayout l, int g) { return ControlsLayout.isStick(g) && !l.fixed[g - ControlsLayout.LS]; }

    // A first finger: on the selected group's handle, resizing; on a group, selecting and moving it; elsewhere, nothing selected. True when the selection changed
    public boolean down(ControlsLayout l, float px, float py, float w, float h, float handleR) {
        mode = 0;
        if (sel >= 0) { float[] c = handle(l, sel, w, h); if (Math.hypot(px - c[0], py - c[1]) < 2.2f * handleR) { mode = 2; start(l, px, py, w, h); return false; } }
        int g = hit(l, px, py, w, h); boolean changed = g != sel; sel = g;
        if (sel >= 0) { mode = 1; start(l, px, py, w, h); }
        return changed;
    }
    public void secondFinger(ControlsLayout l, float px, float py, float span, float w, float h) { if (sel >= 0) { mode = 3; start(l, px, py, w, h); d0 = span; } }
    public void up() { mode = 0; }
    public void secondUp() { if (mode == 3) mode = 0; }
    // Fingers moved: what the mode does to the selected group. span: the distance between the first two fingers (pinching)
    public boolean move(ControlsLayout l, float px, float py, float span, int fingers, float w, float h) {
        if (sel < 0 || mode == 0) return false;
        int g = sel; float[] z = isZone(l, g) ? l.zone[g - ControlsLayout.LS] : null;
        if (mode == 1) {
            if (z != null) { z[0] = ControlsLayout.clamp(z0[0] + (px - fx0) / w, 0, 1 - z[2]); z[1] = ControlsLayout.clamp(z0[1] + (py - fy0) / h, 0, 1 - z[3]); }
            else { l.x[g] = ControlsLayout.clamp((px - dx) / w, 0, 1); l.y[g] = ControlsLayout.clamp((py - dy) / h, 0, 1); }
        } else if (mode == 2) {
            if (z != null) { z[2] = ControlsLayout.clamp(z0[2] + (px - fx0) / w, 0.1f, 1 - z[0]); z[3] = ControlsLayout.clamp(z0[3] + (py - fy0) / h, 0.1f, 1 - z[1]); }
            else { float cx = l.x[g] * w, cy = l.y[g] * h, a = (float) Math.hypot(fx0 - cx, fy0 - cy), b = (float) Math.hypot(px - cx, py - cy); if (a > 1) l.s[g] = ControlsLayout.clamp(s0 * b / a, 0.3f, 3); }
        } else if (mode == 3 && fingers >= 2 && d0 > 1) {
            float f = span / d0;
            if (z != null) { float cx = z0[0] + z0[2] / 2, cy = z0[1] + z0[3] / 2; z[2] = ControlsLayout.clamp(z0[2] * f, 0.1f, 1); z[3] = ControlsLayout.clamp(z0[3] * f, 0.1f, 1);
                             z[0] = ControlsLayout.clamp(cx - z[2] / 2, 0, 1 - z[2]); z[1] = ControlsLayout.clamp(cy - z[3] / 2, 0, 1 - z[3]); }
            else l.s[g] = ControlsLayout.clamp(s0 * f, 0.3f, 3);
        }
        return true;
    }
    private void start(ControlsLayout l, float px, float py, float w, float h) {
        int g = sel; fx0 = px; fy0 = py; s0 = l.s[g]; dx = px - l.x[g] * w; dy = py - l.y[g] * h;
        if (ControlsLayout.isStick(g)) System.arraycopy(l.zone[g - ControlsLayout.LS], 0, z0, 0, 4);
    }
    // The profile list picked (name, shared): true, and nothing selected, only when that is another profile than the active one
    public boolean pickProfile(ControlsLayout.Store st, String name, boolean shared) {
        if (name == null || (name.equals(st.active) && shared == st.activeShared)) return false;
        st.active = name; st.activeShared = shared; sel = -1; mode = 0; return true;
    }
}

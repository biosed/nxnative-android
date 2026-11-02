// The layout editor's touch handling (ControlsEditing) on a computer, without Android: a selection that holds, the smallest group under a finger, moving, resizing by
// the handle and by pinching, and the profile list's own reports after a refill, which must not clear the selection (they did, on a Xiaomi Pad 8: every touch on a
// group was undone at once and the panel refilled over and over).
//   javac -d <classes> dm/android/app/java/com/nxnative/ControlsLayout.java dm/android/app/java/com/nxnative/ControlsEditing.java dm/android/tools/ControlsEditingTest.java
//   java -cp <classes> ControlsEditingTest
import com.nxnative.ControlsEditing;
import com.nxnative.ControlsLayout;

public class ControlsEditingTest {
    static int fails;
    static void check(boolean ok, String what) { System.out.println((ok ? "ok   " : "FAIL ") + what); if (!ok) fails++; }
    static float[] centre(float[] b) { return new float[]{b[0] + b[2] / 2, b[1] + b[3] / 2}; }
    public static void main(String[] a) {
        float W = 3200, H = 2136, hr = H * 0.032f;
        ControlsLayout.Store st = ControlsLayout.Store.parse("", W, H); ControlsLayout l = st.current(); ControlsEditing ed = new ControlsEditing();
        float[] aBtn = centre(l.buttonBox(0, W, H)), up = centre(l.buttonBox(4, W, H));
        // A tap on A: the face group, though A lies inside the right stick's zone
        check(ed.down(l, aBtn[0], aBtn[1], W, H, hr) && ed.sel == ControlsLayout.FACE, "tap on A selects A B X Y (not the right stick's zone around it)");
        ed.up();
        // The profile list reports the active profile again (a refill): the selection holds
        check(!ed.pickProfile(st, st.active, st.activeShared) && ed.sel == ControlsLayout.FACE, "the list reporting the active profile again keeps the selection");
        // A second tap on the same group: still selected, nothing changed
        check(!ed.down(l, aBtn[0], aBtn[1], W, H, hr) && ed.sel == ControlsLayout.FACE, "a second tap keeps the selection");
        // Dragging it moves it by the finger's travel
        float x0 = l.x[ControlsLayout.FACE], y0 = l.y[ControlsLayout.FACE];
        ed.move(l, aBtn[0] - 320, aBtn[1] - 213.6f, 0, 1, W, H); ed.up();
        check(Math.abs(l.x[ControlsLayout.FACE] - (x0 - 0.1f)) < 1e-4 && Math.abs(l.y[ControlsLayout.FACE] - (y0 - 0.1f)) < 1e-4, "dragging moves the group by the finger's travel");
        // The handle resizes it
        float[] hc = ControlsEditing.handle(l, ControlsLayout.FACE, W, H); float cx = l.x[0] * W, cy = l.y[0] * H;
        check(!ed.down(l, hc[0], hc[1], W, H, hr), "a finger on the handle keeps the selection");
        ed.move(l, cx + (hc[0] - cx) * 1.5f, cy + (hc[1] - cy) * 1.5f, 0, 1, W, H); ed.up();
        check(Math.abs(l.s[ControlsLayout.FACE] - 1.5f) < 0.01f, "dragging the handle out by half again scales by 1.5");
        // Pinching
        float[] fc = centre(l.groupBox(ControlsLayout.FACE, W, H)); ed.down(l, fc[0], fc[1], W, H, hr); ed.secondFinger(l, fc[0], fc[1], 200, W, H);
        ed.move(l, fc[0], fc[1], 100, 2, W, H); ed.secondUp(); ed.up();
        check(Math.abs(l.s[ControlsLayout.FACE] - 0.75f) < 0.01f, "pinching to half the span halves the size");
        // The D-pad inside the left stick's zone: the D-pad
        check(ed.down(l, up[0], up[1], W, H, hr) && ed.sel == ControlsLayout.DPAD, "tap on the D-pad selects the D-pad (not the left stick's zone)"); ed.up();
        // Open space in the left half: the left stick's zone
        check(ed.down(l, W * 0.30f, H * 0.45f, W, H, hr) && ed.sel == ControlsLayout.LS, "tap on open space in the left half selects the left stick's zone");
        float z0 = l.zone[0][0]; ed.move(l, W * 0.30f + 160, H * 0.45f, 0, 1, W, H); ed.up();
        check(Math.abs(l.zone[0][0] - Math.min(z0 + 0.05f, 1 - l.zone[0][2])) < 1e-4, "dragging a zone moves the zone, kept on the screen");
        // A real change of profile clears the selection; picking back as well
        st.profiles.put("Dua", l.copy("Dua"));
        check(ed.pickProfile(st, "Dua", false) && ed.sel == -1 && st.active.equals("Dua"), "choosing another profile clears the selection");
        // Hit order is by size: every button's centre selects its own group
        ControlsLayout d = ControlsLayout.defaults(W, H); boolean all = true;
        for (int i = 0; i < ControlsLayout.BUTTONS.length; i++) { float[] c = centre(d.buttonBox(i, W, H)); all &= ControlsEditing.hit(d, c[0], c[1], W, H) == ControlsLayout.BUTTONS[i].group; }
        check(all, "every button's centre selects its own group on the default layout");
        // The bar: hidden with the panel closed and nothing selected; shown for the panel or a selection; out of the way while dragging, back on release
        check(!ControlsEditing.barShown(false, -1, false), "panel closed, nothing selected: no bar");
        check(ControlsEditing.barShown(true, -1, false) && ControlsEditing.barShown(false, ControlsLayout.FACE, false), "panel open, or a group selected: the bar shows");
        ControlsLayout m = ControlsLayout.defaults(W, H); ControlsEditing e2 = new ControlsEditing(); e2.slop = 24; float[] b2 = centre(m.buttonBox(1, W, H));
        e2.down(m, b2[0], b2[1], W, H, hr); e2.move(m, b2[0] + 10, b2[1], 0, 1, W, H);
        check(!e2.dragging && ControlsEditing.barShown(true, e2.sel, e2.dragging), "a finger wandering within the touch slop is no drag: the bar stays");
        e2.move(m, b2[0] + 80, b2[1], 0, 1, W, H);
        check(e2.dragging && !ControlsEditing.barShown(true, e2.sel, e2.dragging), "dragging a group past the slop: the bar steps aside, even with the panel open");
        e2.up(); check(!e2.dragging && ControlsEditing.barShown(false, e2.sel, e2.dragging), "on release the bar is back with the selected group's options");
        float[] hc2 = ControlsEditing.handle(m, e2.sel, W, H); e2.down(m, hc2[0], hc2[1], W, H, hr); e2.move(m, hc2[0] + 60, hc2[1] + 60, 0, 1, W, H);
        check(e2.dragging, "resizing by the handle also moves the bar aside"); e2.up();
        e2.down(m, b2[0], b2[1], W, H, hr); e2.secondFinger(m, b2[0], b2[1], 200, W, H); e2.move(m, b2[0], b2[1], 210, 2, W, H);
        check(e2.dragging, "pinching moves the bar aside at once"); e2.secondUp(); e2.up();
        // Touches: only the bar's own rectangle is the bar's, and only while it shows
        float[] bar = {800, 0, 1600, 110};
        check(!ControlsEditing.toCanvas(bar, true, 1600, 50), "a touch inside the shown bar is the bar's");
        check(ControlsEditing.toCanvas(bar, true, 1600, 400) && ControlsEditing.toCanvas(bar, true, 200, 50) && ControlsEditing.toCanvas(bar, true, 2500, 50), "touches outside the bar's rectangle are the canvas's, the bar open");
        check(ControlsEditing.toCanvas(bar, false, 1600, 50), "a touch where a hidden bar would be is the canvas's");
        float[] aB = centre(m.buttonBox(0, W, H));
        check(ControlsEditing.toCanvas(bar, true, aB[0], aB[1]), "A, below the bar, is reachable with the bar open");
        System.out.println(fails == 0 ? "all passed" : fails + " failed"); System.exit(fails == 0 ? 0 : 1);
    }
}

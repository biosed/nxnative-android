// The on-screen controls' layout as a picture, on a computer: what the launcher's editor shows (ControlsActivity's preview: a plain background, the controls from the
// sprite atlas where the game puts them, a floating stick's zone, a selected group's frame and handle), drawn with the same layout code (ControlsLayout) into a PNG.
// For checking the art and the layouts without a device:
//   javac -d <classes> dm/android/app/java/com/nxnative/ControlsLayout.java dm/android/tools/ControlsPreview.java
//   java -cp <classes> ControlsPreview <atlas dir (controls_atlas.py's output)> <out.png> [width height] [controls.json] [selected group]
import com.nxnative.ControlsLayout;
import java.awt.BasicStroke;
import java.awt.Color;
import java.awt.Font;
import java.awt.GradientPaint;
import java.awt.Graphics2D;
import java.awt.RenderingHints;
import java.awt.AlphaComposite;
import java.awt.image.BufferedImage;
import java.io.File;
import java.nio.file.Files;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import javax.imageio.ImageIO;

public class ControlsPreview {
    public static void main(String[] a) throws Exception {
        File dir = new File(a[0]); int w = a.length > 3 ? Integer.parseInt(a[2]) : 1600, h = a.length > 3 ? Integer.parseInt(a[3]) : 1068;
        BufferedImage atlas = ImageIO.read(new File(dir, "atlas.png")); Map<String, BufferedImage> spr = new HashMap<>(); int cell = 0, cols = 0;
        for (String l : Files.readAllLines(new File(dir, "atlas.txt").toPath())) {
            String[] f = l.trim().split("\\s+"); if (f.length < 2 || f[0].startsWith("#")) continue;
            if (f[0].equals("cell")) { cell = Integer.parseInt(f[1]); cols = Integer.parseInt(f[2]); continue; }
            int i = Integer.parseInt(f[1]); spr.put(f[0], atlas.getSubimage((i % cols) * cell, (i / cols) * cell, cell, cell));
        }
        ControlsLayout l = a.length > 4 ? ControlsLayout.Store.parse(new String(Files.readAllBytes(new File(a[4]).toPath()), "UTF-8"), w, h).current() : ControlsLayout.defaults(w, h);
        int sel = a.length > 5 ? Integer.parseInt(a[5]) : -1;
        BufferedImage img = new BufferedImage(w, h, BufferedImage.TYPE_INT_ARGB); Graphics2D g = img.createGraphics();
        g.setRenderingHint(RenderingHints.KEY_INTERPOLATION, RenderingHints.VALUE_INTERPOLATION_BICUBIC); g.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
        g.setPaint(new GradientPaint(0, 0, new Color(44, 52, 66), 0, h, new Color(18, 20, 26))); g.fillRect(0, 0, w, h);
        g.setFont(new Font("SansSerif", Font.PLAIN, Math.round(h * 0.032f)));
        for (int k = 0; k < 2; k++) {
            int gi = ControlsLayout.LS + k; float ga = l.hidden[gi] ? 0.2f : 1;
            if (!l.fixed[k]) { float[] z = l.zoneBox(k, w, h); g.setColor(new Color(90, 160, 255, 40)); if (!l.hidden[gi]) g.fillRect(r(z[0]), r(z[1]), r(z[2]), r(z[3]));
                               g.setColor(new Color(120, 180, 255, 140)); g.setStroke(new BasicStroke(3)); g.drawRect(r(z[0]), r(z[1]), r(z[2]), r(z[3]));
                               g.setColor(new Color(255, 255, 255, 200)); g.drawString(ControlsLayout.GROUP_LABEL[gi] + " (mengambang: area)", z[0] + 24, z[1] + z[3] - 28); }
            draw(g, spr.get("stick_base"), l.stickBox(k, w, h), l.opacity * ga * (l.fixed[k] ? 1 : 0.5f)); draw(g, spr.get("stick_knob"), l.knobBox(k, w, h), l.opacity * ga * (l.fixed[k] ? 1 : 0.5f));
        }
        for (int i = 0; i < ControlsLayout.BUTTONS.length; i++) {
            ControlsLayout.Btn b = ControlsLayout.BUTTONS[i]; boolean off = l.hidden[b.group] || l.hiddenButtons.contains(b.name); float[] box = l.buttonBox(i, w, h);
            draw(g, spr.get(b.sprite), box, l.opacity * (off ? 0.2f : 1));
            if (off) { g.setColor(new Color(255, 90, 90)); g.setStroke(new BasicStroke(7, BasicStroke.CAP_ROUND, BasicStroke.JOIN_ROUND));
                       g.drawLine(r(box[0] + box[2] * 0.2f), r(box[1] + box[3] * 0.2f), r(box[0] + box[2] * 0.8f), r(box[1] + box[3] * 0.8f));
                       g.drawLine(r(box[0] + box[2] * 0.8f), r(box[1] + box[3] * 0.2f), r(box[0] + box[2] * 0.2f), r(box[1] + box[3] * 0.8f)); }
        }
        if (sel >= 0) { float[] gb = l.groupBox(sel, w, h); g.setColor(new Color(255, 214, 64));
                        g.setStroke(new BasicStroke(5, BasicStroke.CAP_BUTT, BasicStroke.JOIN_MITER, 10, new float[]{18, 12}, 0)); g.drawRect(r(gb[0] - 10), r(gb[1] - 10), r(gb[2] + 20), r(gb[3] + 20));
                        float hr = h * 0.025f; g.fillOval(r(gb[0] + gb[2] + 10 - hr), r(gb[1] + gb[3] + 10 - hr), r(2 * hr), r(2 * hr)); }
        g.dispose(); ImageIO.write(img, "png", new File(a[1]));
        System.out.println(a[1] + ": " + w + "x" + h + ", profile " + l.name);
    }
    static int r(float v) { return Math.round(v); }
    static void draw(Graphics2D g, BufferedImage s, float[] b, float alpha) {
        if (s == null) return;
        g.setComposite(AlphaComposite.getInstance(AlphaComposite.SRC_OVER, Math.max(0, Math.min(1, alpha))));
        g.drawImage(s, r(b[0]), r(b[1]), r(b[2]), r(b[3]), null);
        g.setComposite(AlphaComposite.SrcOver);
    }
}

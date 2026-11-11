// The launcher's and the layout editor's graphics: icons from assets/ui/atlas.png (+ atlas.txt) and backgrounds from assets/ui/<name>.png, all rasterized at APK
// build time from the SVG masters in app/res_svg (tools/controls_atlas.py); no graphic of the app is drawn by hand or kept as a raster file in the sources.
package com.nxnative;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Drawable;
import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;

public final class UiIcons {
    private static Map<String, Bitmap> icons;
    private UiIcons() {}
    // A sprite atlas (atlas.png + atlas.txt, controls_atlas.py) cut into its named cells
    static Map<String, Bitmap> atlas(Context c, String dir) {
        Map<String, Bitmap> m = new HashMap<>();
        try (InputStream in = c.getAssets().open(dir + "/atlas.png"); BufferedReader r = new BufferedReader(new InputStreamReader(c.getAssets().open(dir + "/atlas.txt"), StandardCharsets.UTF_8))) {
            Bitmap a = BitmapFactory.decodeStream(in); int cell = 0, cols = 0; String l;
            while ((l = r.readLine()) != null) {
                String[] f = l.trim().split("\\s+"); if (f.length < 2 || f[0].startsWith("#")) continue;
                if (f[0].equals("cell")) { cell = Integer.parseInt(f[1]); cols = Integer.parseInt(f[2]); continue; }
                int i = Integer.parseInt(f[1]); if (cell > 0 && a != null) m.put(f[0], Bitmap.createBitmap(a, (i % cols) * cell, (i / cols) * cell, cell, cell));
            }
        } catch (Exception e) { /* an APK without it: no icons */ }
        return m;
    }
    public static synchronized Bitmap icon(Context c, String name) { if (icons == null) icons = atlas(c.getApplicationContext(), "ui"); return icons.get(name); }
    // An icon sized for a widget (dp), or null without the atlas
    public static Drawable drawable(Context c, String name, int dp) {
        Bitmap b = icon(c, name); if (b == null) return null;
        BitmapDrawable d = new BitmapDrawable(c.getResources(), b); int px = Math.round(dp * c.getResources().getDisplayMetrics().density); d.setBounds(0, 0, px, px); d.setFilterBitmap(true);
        return d;
    }
    public static Bitmap background(Context c, String name) {
        try (InputStream in = c.getAssets().open("ui/" + name + ".png")) { return BitmapFactory.decodeStream(in); } catch (Exception e) { return null; }
    }
}

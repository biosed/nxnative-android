// The shared layouts folder: one folder the player picks once in each game app (the system's folder picker, ACTION_OPEN_DOCUMENT_TREE; Documents/NxNative/layouts is
// suggested), whose access Android keeps (a persistable URI permission). Every game app that picked the same folder reads and writes the layouts in it, one profile per
// <name>.json (ControlsLayout's game-independent format), so a layout made in one game is there in every other and survives reinstalling a game. Android 11 and later
// let no app read another's files directly; this needs no storage permission and no shared user ID.
package com.nxnative;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.util.DisplayMetrics;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

final class SharedLayouts {
    private static final String PREFS = "controls", KEY = "shared_tree";
    static final Uri SUGGESTED = DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", "primary:Documents/NxNative/layouts");
    private SharedLayouts() {}
    static final class Entry { final String name; final Uri doc; final ControlsLayout layout; Entry(String n, Uri d, ControlsLayout l) { name = n; doc = d; layout = l; } }

    // The folder, while this app still holds access to it (the player may take it back in the system's settings)
    static Uri tree(Context c) {
        String s = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(KEY, null); if (s == null) return null;
        Uri u = Uri.parse(s);
        for (UriPermission p : c.getContentResolver().getPersistedUriPermissions()) if (p.getUri().equals(u) && p.isReadPermission() && p.isWritePermission()) return u;
        return null;
    }
    static void setTree(Context c, Uri u) {
        c.getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        c.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString(KEY, u.toString()).apply();
    }
    static String folderName(Context c, Uri tree) {
        Uri d = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
        try (Cursor k = c.getContentResolver().query(d, new String[]{DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) { if (k != null && k.moveToFirst()) return k.getString(0); }
        catch (Exception e) { /* the folder went away */ }
        return "?";
    }
    // Every profile in the folder (files that hold no layout are left alone); the name is the profile's own, else the file's
    static List<Entry> list(Context c, Uri tree, float w, float h) {
        List<Entry> out = new ArrayList<>(); ContentResolver r = c.getContentResolver();
        Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
        try (Cursor k = r.query(kids, new String[]{DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
            while (k != null && k.moveToNext()) {
                String id = k.getString(0), fn = k.getString(1); if (fn == null || !fn.toLowerCase(java.util.Locale.ROOT).endsWith(".json")) continue;
                Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                try (InputStream in = r.openInputStream(doc)) {
                    List<ControlsLayout> ls = ControlsLayout.readProfiles(ControlsActivity.readAll(in), w, h); if (ls == null) continue;
                    ControlsLayout l = ls.get(0); if (l.name == null || l.name.trim().isEmpty()) l.name = fn.substring(0, fn.length() - 5);
                    out.add(new Entry(l.name, doc, l));
                } catch (Exception e) { /* unreadable: left out */ }
            }
        } catch (Exception e) { return out; }
        return out;
    }
    // A profile into the folder: over its own file (doc), else a new <name>.json
    static Uri write(Context c, Uri tree, ControlsLayout l, Uri doc) throws Exception {
        ContentResolver r = c.getContentResolver();
        if (doc == null) doc = DocumentsContract.createDocument(r, DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree)), "application/json", ControlsActivity.safeFileName(l.name) + ".json");
        if (doc == null) throw new java.io.IOException("the folder made no file");
        try (OutputStream o = r.openOutputStream(doc, "wt")) { o.write(l.toJson().getBytes(StandardCharsets.UTF_8)); }
        return doc;
    }
    static boolean delete(Context c, Uri doc) { try { return DocumentsContract.deleteDocument(c.getContentResolver(), doc); } catch (Exception e) { return false; } }

    // The launcher, each time its menu shows: when the active profile is a shared one, the game's controls_active.txt follows what is in the folder now (another
    // game may have changed it since this one's editor last saved)
    static void refreshActive(Context c) {
        File js = new File(c.getFilesDir(), "controls.json"); if (!js.exists()) return;
        try {
            String s; try (InputStream in = new FileInputStream(js)) { s = ControlsActivity.readAll(in); }
            DisplayMetrics m = c.getResources().getDisplayMetrics(); float w = Math.max(m.widthPixels, m.heightPixels), h = Math.min(m.widthPixels, m.heightPixels);
            ControlsLayout.Store st = ControlsLayout.Store.parse(s, w, h); if (!st.activeShared) return;
            Uri t = tree(c); if (t == null) return;
            for (Entry e : list(c, t, w, h)) if (e.name.equals(st.active)) {
                try (OutputStream o = new FileOutputStream(new File(c.getFilesDir(), "controls_active.txt"))) { o.write(e.layout.toNative().getBytes(StandardCharsets.UTF_8)); }
                return;
            }
        } catch (Exception e) { /* left as it was */ }
    }
}

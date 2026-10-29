package org.ac1.recomp;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.os.Bundle;
import java.nio.file.Files;
import java.nio.file.Path;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import android.database.Cursor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Imports a user-owned AC1 executable, then calls the real native startup harness. */
public final class LauncherActivity extends Activity {
    private static final int PICK_GAME = 1;
    private static final String EXE_NAME = "AssassinsCreed_Dx9.exe";
    private static final String LIB_NAME = "ac1_android";
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private TextView status;
    private Button choose;
    private Button run;
    private File exeFile;
    private SurfaceView gameSurface;
    private boolean surfaceReady;
    private boolean busy;

    static { System.loadLibrary(LIB_NAME); }

    private static native int nativeRun(String exePath, String userdataPath);
    private static native String nativeLastError();
    private static native boolean nativeSetSurface(Surface surface);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        status = new TextView(this);
        status.setText("Select the folder containing your legally obtained Assassin's Creed installation. Its files will be copied into app-private storage.");
        status.setTextSize(16);
        status.setPadding(24, 20, 24, 20);
        choose = new Button(this);
        choose.setText("Import game folder");
        choose.setOnClickListener(v -> selectGameFolder());
        run = new Button(this);
        run.setText("Run original startup");
        run.setEnabled(false);
        run.setOnClickListener(v -> runGuest());
        gameSurface = new SurfaceView(this);
        gameSurface.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) {
                updateSurface(holder.getSurface());
            }
            @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                updateSurface(holder.getSurface());
            }
            @Override public void surfaceDestroyed(SurfaceHolder holder) {
                surfaceReady = false;
                nativeSetSurface(null);
                refreshRunEnabled();
            }
        });
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.addView(status);
        content.addView(choose);
        content.addView(run);
        content.addView(gameSurface, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f));
        setContentView(content);
        restoreImportedExe();
    }

    private void updateSurface(Surface surface) {
        surfaceReady = surface != null && surface.isValid() && nativeSetSurface(surface);
        refreshRunEnabled();
    }

    private void refreshRunEnabled() {
        run.setEnabled(!busy && exeFile != null && exeFile.isFile() && surfaceReady);
        choose.setEnabled(!busy);
    }

    private void selectGameFolder() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, PICK_GAME);
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_GAME || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        busy = true;
        choose.setEnabled(false);
        run.setEnabled(false);
        status.setText("Copying game files into app-private storage…");
        worker.execute(() -> {
            try {
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
                File directory = getGameDirectory();
                if (!directory.isDirectory() && !directory.mkdirs()) throw new IllegalStateException("Cannot create game folder");
                copyTree(uri, DocumentsContract.getTreeDocumentId(uri), directory);
                File target = new File(directory, EXE_NAME);
                if (!target.isFile() || target.length() < 1024 * 1024) throw new IllegalArgumentException("Selected folder must contain AssassinsCreed_Dx9.exe");
                runOnUiThread(() -> {
                    exeFile = target;
                    busy = false;
                    refreshRunEnabled();
                    status.setText("Original executable and game folder copied. Tap Run original startup to execute the lifted code.");
                });
            } catch (Exception error) {
                runOnUiThread(() -> { busy = false; refreshRunEnabled(); status.setText("Import failed: " + error.getMessage()); });
            }
        });
    }

    private File getGameDirectory() {
        File storage = getExternalFilesDir(null);
        if (storage == null) storage = getFilesDir();
        return new File(storage, "game");
    }

    private void copyTree(Uri tree, String documentId, File target) throws Exception {
        Path targetPath = target.getCanonicalFile().toPath();
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, documentId);
        try (Cursor cursor = getContentResolver().query(children,
                new String[]{DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                        DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                        DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
            if (cursor == null) throw new IllegalStateException("Selected folder cannot be listed");
            while (cursor.moveToNext()) {
                String id = cursor.getString(0);
                String name = cursor.getString(1);
                String mime = cursor.getString(2);
                if (name == null || name.isEmpty() || name.equals(".") || name.equals("..") ||
                        name.contains("/") || name.contains("\\")) continue;
                File destination = new File(target, name);
                Path destinationPath = destination.getCanonicalFile().toPath();
                if (!destinationPath.startsWith(targetPath) || Files.isSymbolicLink(destination.toPath()))
                    throw new IllegalArgumentException("Unsafe game-data path: " + name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    if (!destination.isDirectory() && !destination.mkdirs()) throw new IllegalStateException("Cannot create " + name);
                    copyTree(tree, id, destination);
                } else {
                    Uri file = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                    try (InputStream in = getContentResolver().openInputStream(file);
                         OutputStream out = new FileOutputStream(destination)) {
                        if (in == null) throw new IllegalStateException("Cannot read " + name);
                        byte[] buffer = new byte[256 * 1024];
                        for (int count; (count = in.read(buffer)) >= 0;) out.write(buffer, 0, count);
                    }
                }
            }
        }
    }

    private void restoreImportedExe() {
        File candidate = new File(getGameDirectory(), EXE_NAME);
        if (candidate.isFile()) {
            exeFile = candidate;
            refreshRunEnabled();
            status.setText("Original DX9 executable and game folder are in app-private storage.");
        }
    }

    private void runGuest() {
        if (busy || !surfaceReady || exeFile == null || !exeFile.isFile()) return;
        busy = true;
        run.setEnabled(false);
        choose.setEnabled(false);
        status.setText("Calling the original executable through the native startup harness…");
        worker.execute(() -> {
            final int result;
            final String detail;
            try {
                File userdata = new File(getFilesDir(), "userdata");
                if (!userdata.isDirectory() && !userdata.mkdirs())
                    throw new IllegalStateException("Cannot create app-private userdata directory");
                result = nativeRun(exeFile.getAbsolutePath(), userdata.getCanonicalPath());
                detail = nativeLastError();
            }
            catch (Throwable error) {
                runOnUiThread(() -> { busy = false; status.setText("Native startup failed: " + error); refreshRunEnabled(); });
                return;
            }
            runOnUiThread(() -> {
                status.setText("Native startup returned " + result + (detail == null || detail.isEmpty() ? "" : "\n" + detail));
                busy = false;
                refreshRunEnabled();
            });
        });
    }

    @Override protected void onDestroy() {
        nativeSetSurface(null);
        worker.shutdownNow();
        super.onDestroy();
    }
}

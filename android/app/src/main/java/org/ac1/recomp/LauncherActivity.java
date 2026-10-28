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
import android.widget.ScrollView;
import android.widget.TextView;
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

    static { System.loadLibrary(LIB_NAME); }

    private static native int nativeRun(String exePath, String userdataPath);
    private static native String nativeLastError();

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
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.addView(status);
        content.addView(choose);
        content.addView(run);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(content);
        setContentView(scroll);
        restoreImportedExe();
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
                    run.setEnabled(true);
                    choose.setEnabled(true);
                    status.setText("Original executable and game folder copied. Tap Run original startup to execute the lifted code.");
                });
            } catch (Exception error) {
                runOnUiThread(() -> { choose.setEnabled(true); run.setEnabled(false); status.setText("Import failed: " + error.getMessage()); });
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
            run.setEnabled(true);
            status.setText("Original DX9 executable and game folder are in app-private storage.");
        }
    }

    private void runGuest() {
        if (exeFile == null || !exeFile.isFile()) return;
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
                runOnUiThread(() -> { status.setText("Native startup failed: " + error); run.setEnabled(true); choose.setEnabled(true); });
                return;
            }
            runOnUiThread(() -> {
                status.setText("Native startup returned " + result + (detail == null || detail.isEmpty() ? "" : "\n" + detail));
                run.setEnabled(true);
                choose.setEnabled(true);
            });
        });
    }

    @Override protected void onDestroy() {
        worker.shutdownNow();
        super.onDestroy();
    }
}

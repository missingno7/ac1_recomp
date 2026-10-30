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
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.InputDevice;
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
    // Lifted C call frames need more native stack than Android's default 1 MiB.
    // This is independent of the emulated x86 stack managed by the runtime.
    private final ExecutorService worker = Executors.newSingleThreadExecutor(
        task -> new Thread(null, task, "AC1 startup", 64L * 1024L * 1024L));
    private TextView status;
    private Button choose;
    private Button run;
    private File exeFile;
    private SurfaceView gameSurface;
    private boolean surfaceReady;
    private boolean busy;
    private boolean autoRun;
    private boolean skipIntros;
    private boolean traceImports;
    private String inputScript;

    static { System.loadLibrary(LIB_NAME); }

    private static native int nativeRun(String exePath, String userdataPath, String scriptPath,
                                       boolean skipIntros, boolean traceImports);
    private static native String nativeLastError();
    private static native boolean nativeSetSurface(Surface surface);
    private static native void nativeKey(int dik, boolean down);
    private static native void nativeMouse(int dx, int dy, int wheel, int buttons);
    private static native void nativeResetInput();
    private static native void nativeAudioPause(boolean paused);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        autoRun = getIntent().getBooleanExtra("run", false);
        skipIntros = getIntent().getBooleanExtra("skip_intros", false);
        traceImports = getIntent().getBooleanExtra("trace_imports", false);
        inputScript = getIntent().getStringExtra("input_script");
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
        gameSurface.setFocusableInTouchMode(true);
        gameSurface.setOnTouchListener((v, event) -> {
            if (!busy) return false;
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                nativeKey(0x1c, true); return true;
            }
            if (event.getActionMasked() == MotionEvent.ACTION_UP || event.getActionMasked() == MotionEvent.ACTION_CANCEL) {
                nativeKey(0x1c, false); return true;
            }
            return true;
        });
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
        maybeAutoRun();
    }

    private void updateSurface(Surface surface) {
        surfaceReady = surface != null && surface.isValid() && nativeSetSurface(surface);
        refreshRunEnabled();
        maybeAutoRun();
    }

    private void maybeAutoRun() {
        if (autoRun && surfaceReady && exeFile != null && exeFile.isFile() && !busy) {
            autoRun = false;
            runGuest();
        }
    }

    private static int dik(int key) {
        final int[] letters = {0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c};
        if (key >= KeyEvent.KEYCODE_A && key <= KeyEvent.KEYCODE_Z) return letters[key-KeyEvent.KEYCODE_A];
        if (key >= KeyEvent.KEYCODE_1 && key <= KeyEvent.KEYCODE_9) return 2+key-KeyEvent.KEYCODE_1;
        switch (key) {
            case KeyEvent.KEYCODE_0: return 0x0b;
            case KeyEvent.KEYCODE_ENTER: case KeyEvent.KEYCODE_BUTTON_A: case KeyEvent.KEYCODE_BUTTON_START: return 0x1c;
            case KeyEvent.KEYCODE_ESCAPE: case KeyEvent.KEYCODE_BUTTON_B: case KeyEvent.KEYCODE_BUTTON_SELECT: return 1;
            case KeyEvent.KEYCODE_SPACE: case KeyEvent.KEYCODE_BUTTON_X: return 0x39;
            case KeyEvent.KEYCODE_BUTTON_Y: return 0x12;
            case KeyEvent.KEYCODE_DPAD_UP: return 0xc8;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 0xd0;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 0xcb;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 0xcd;
            case KeyEvent.KEYCODE_SHIFT_LEFT: case KeyEvent.KEYCODE_BUTTON_L1: return 0x2a;
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return 0x36;
            case KeyEvent.KEYCODE_CTRL_LEFT: case KeyEvent.KEYCODE_BUTTON_R1: return 0x1d;
            case KeyEvent.KEYCODE_CTRL_RIGHT: return 0x9d;
            case KeyEvent.KEYCODE_ALT_LEFT: return 0x38;
            case KeyEvent.KEYCODE_ALT_RIGHT: return 0xb8;
            case KeyEvent.KEYCODE_TAB: return 0x0f;
            case KeyEvent.KEYCODE_DEL: return 0x0e;
            default: return -1;
        }
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        int code = dik(event.getKeyCode());
        if (busy && code >= 0 && (event.getAction() == KeyEvent.ACTION_DOWN || event.getAction() == KeyEvent.ACTION_UP)) {
            nativeKey(code, event.getAction() == KeyEvent.ACTION_DOWN);
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override public boolean onGenericMotionEvent(MotionEvent event) {
        if (busy && (event.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
            float x = event.getAxisValue(MotionEvent.AXIS_X), y = event.getAxisValue(MotionEvent.AXIS_Y);
            nativeKey(0x1e, x < -0.3f); nativeKey(0x20, x > 0.3f);
            nativeKey(0x11, y < -0.3f); nativeKey(0x1f, y > 0.3f);
            nativeMouse((int)(event.getAxisValue(MotionEvent.AXIS_Z)*20),
                    (int)(event.getAxisValue(MotionEvent.AXIS_RZ)*20), 0, 0);
            return true;
        }
        if (busy && (event.getSource() & InputDevice.SOURCE_MOUSE) == InputDevice.SOURCE_MOUSE) {
            nativeMouse((int)event.getAxisValue(MotionEvent.AXIS_RELATIVE_X),
                    (int)event.getAxisValue(MotionEvent.AXIS_RELATIVE_Y),
                    (int)(event.getAxisValue(MotionEvent.AXIS_VSCROLL)*120), event.getButtonState());
            return true;
        }
        return super.onGenericMotionEvent(event);
    }

    @Override protected void onPause() {
        nativeAudioPause(true);
        nativeResetInput();
        super.onPause();
    }

    @Override protected void onResume() {
        super.onResume();
        nativeAudioPause(false);
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
        status.setText("Tap the surface for Enter; keyboard/gamepad enabled. Calling the original executable through the native startup harness…");
        worker.execute(() -> {
            final int result;
            final String detail;
            try {
                File userdata = new File(getFilesDir(), "userdata");
                if (!userdata.isDirectory() && !userdata.mkdirs())
                    throw new IllegalStateException("Cannot create app-private userdata directory");
                String scriptPath = null;
                if (inputScript != null && !inputScript.isEmpty()) {
                    if (!inputScript.matches("[A-Za-z0-9_-]+\\.txt"))
                        throw new IllegalArgumentException("input_script must be a filename in input_scripts");
                    File script = new File(new File(getGameDirectory().getParentFile(), "input_scripts"), inputScript);
                    if (!script.isFile()) throw new IllegalArgumentException("Missing input script: " + script);
                    scriptPath = script.getCanonicalPath();
                }
                result = nativeRun(exeFile.getAbsolutePath(), userdata.getCanonicalPath(), scriptPath,
                                   skipIntros, traceImports);
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

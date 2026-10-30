package org.ac1.recomp;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.graphics.Rect;
import android.os.Build;
import android.util.DisplayMetrics;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import java.nio.file.Files;
import java.nio.file.Path;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.InputDevice;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
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
    private Ac1ControllerView controller;
    private Ac1PhysicalPad physicalPad;
    private boolean gameMode, resumed = true, focused = true;
    private boolean surfaceReady;
    private boolean busy;
    private boolean autoRun;
    private boolean skipIntros;
    private boolean traceImports;
    private String inputScript;
    private String environment;

    static { System.loadLibrary(LIB_NAME); }

    private static native int nativeRun(String exePath, String userdataPath, String scriptPath,
                                       boolean skipIntros, boolean traceImports);
    private static native String nativeLastError();
    private static native boolean nativeSetSurface(Surface surface);
    private static native void nativeKey(int dik, boolean down);
    private static native void nativeMouse(int dx, int dy, int wheel, int buttons);
    private static native void nativeResetInput();
    private static native void nativePad(boolean connected, int buttons, int lt, int rt,
                                         int lx, int ly, int rx, int ry);
    private static native void nativeAudioPause(boolean paused);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        autoRun = getIntent().getBooleanExtra("run", false);
        skipIntros = getIntent().getBooleanExtra("skip_intros", false);
        traceImports = getIntent().getBooleanExtra("trace_imports", false);
        inputScript = getIntent().getStringExtra("input_script");
        environment = getIntent().getStringExtra("env");
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
        controller = new Ac1ControllerView(this, LauncherActivity::nativePad);
        physicalPad = new Ac1PhysicalPad(this, LauncherActivity::nativePad, this::refreshController);
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
        FrameLayout stage = new FrameLayout(this);
        stage.addView(gameSurface);
        stage.addView(controller);
        content.addView(stage, new LinearLayout.LayoutParams(
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
            case KeyEvent.KEYCODE_ENTER: return 0x1c;
            case KeyEvent.KEYCODE_ESCAPE: return 1;
            case KeyEvent.KEYCODE_SPACE: return 0x39;
            case KeyEvent.KEYCODE_DPAD_UP: return 0xc8;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 0xd0;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 0xcb;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 0xcd;
            case KeyEvent.KEYCODE_SHIFT_LEFT: return 0x2a;
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return 0x36;
            case KeyEvent.KEYCODE_CTRL_LEFT: return 0x1d;
            case KeyEvent.KEYCODE_CTRL_RIGHT: return 0x9d;
            case KeyEvent.KEYCODE_ALT_LEFT: return 0x38;
            case KeyEvent.KEYCODE_ALT_RIGHT: return 0xb8;
            case KeyEvent.KEYCODE_TAB: return 0x0f;
            case KeyEvent.KEYCODE_DEL: return 0x0e;
            default: return -1;
        }
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if (physicalPad != null && physicalPad.key(event)) return true;
        int code = dik(event.getKeyCode());
        if (gameMode && resumed && focused && (event.getSource() & InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD && code >= 0 && (event.getAction() == KeyEvent.ACTION_DOWN || event.getAction() == KeyEvent.ACTION_UP)) {
            nativeKey(code, event.getAction() == KeyEvent.ACTION_DOWN);
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override public boolean onGenericMotionEvent(MotionEvent event) {
        if (physicalPad != null && physicalPad.motion(event)) return true;
        if (gameMode && resumed && focused && (event.getSource() & InputDevice.SOURCE_MOUSE) == InputDevice.SOURCE_MOUSE) {
            nativeMouse((int)event.getAxisValue(MotionEvent.AXIS_RELATIVE_X),
                    (int)event.getAxisValue(MotionEvent.AXIS_RELATIVE_Y),
                    (int)(event.getAxisValue(MotionEvent.AXIS_VSCROLL)*120), event.getButtonState());
            return true;
        }
        return super.onGenericMotionEvent(event);
    }

    @Override protected void onPause() {
        resumed = false;
        refreshController();
        nativeAudioPause(true);
        nativeResetInput();
        super.onPause();
    }

    @Override protected void onResume() {
        super.onResume();
        resumed = true;
        refreshController();
        nativeAudioPause(false);
    }

    private void refreshController() {
        if (controller == null || physicalPad == null) return;
        boolean play = gameMode && resumed && focused;
        // Neutralize the old source before publishing the selected source.
        controller.setActive(false);
        nativePad(false,0,0,0,0,0,0,0);
        physicalPad.setRunning(play);
        controller.setActive(play && !physicalPad.connected());
    }

    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        focused = focus;
        refreshController();
        if (!focus) nativeResetInput();
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

    /** While the guest runs, the surface owns the whole display: launcher views and system bars hide. */
    /** Diagnostic runtime settings from `--es env "AC1_X=1,DXVK_Y=2"`; only AC1_ and DXVK_ keys. */
    private static void applyEnvironment(String settings) throws ErrnoException {
        if (settings == null || settings.isEmpty()) return;
        for (String setting : settings.split(",")) {
            int split = setting.indexOf('=');
            String key = split > 0 ? setting.substring(0, split) : "";
            if (!key.matches("(AC1|DXVK)_[A-Z0-9_]+"))
                throw new IllegalArgumentException("env accepts AC1_/DXVK_ KEY=VALUE pairs: " + setting);
            Os.setenv(key, setting.substring(split + 1), true);
        }
    }

    @SuppressWarnings("deprecation") // API 29 has no WindowMetrics.
    private String physicalDisplaySize() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Rect bounds = getWindowManager().getMaximumWindowMetrics().getBounds();
            return bounds.width() + "x" + bounds.height();
        }
        DisplayMetrics metrics = new DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(metrics);
        return metrics.widthPixels + "x" + metrics.heightPixels;
    }

    @SuppressWarnings("deprecation") // API 29 has no WindowInsetsController.
    private void setGameMode(boolean game) {
        gameMode = game;
        refreshController();
        int launcher = game ? View.GONE : View.VISIBLE;
        status.setVisibility(launcher);
        choose.setVisibility(launcher);
        run.setVisibility(launcher);
        Window window = getWindow();
        if (game) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        WindowManager.LayoutParams attributes = window.getAttributes();
        attributes.layoutInDisplayCutoutMode = game
            ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_DEFAULT;
        window.setAttributes(attributes);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(!game);
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                if (game) {
                    controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                    controller.hide(WindowInsets.Type.systemBars());
                } else {
                    controller.show(WindowInsets.Type.systemBars());
                }
            }
        } else {
            window.getDecorView().setSystemUiVisibility(game
                ? View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                  | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                  | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                : View.SYSTEM_UI_FLAG_VISIBLE);
        }
    }

    private void runGuest() {
        if (busy || !surfaceReady || exeFile == null || !exeFile.isFile()) return;
        busy = true;
        run.setEnabled(false);
        choose.setEnabled(false);
        status.setText("AC1 controller enabled. Calling the original executable through the native startup harness…");
        setGameMode(true);
        // The surface resizes asynchronously after the launcher views hide.
        final String displaySize = physicalDisplaySize();
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
                Os.setenv("AC1_DISPLAY_SIZE", displaySize, true);
                applyEnvironment(environment);
                result = nativeRun(exeFile.getAbsolutePath(), userdata.getCanonicalPath(), scriptPath,
                                   skipIntros, traceImports);
                detail = nativeLastError();
            }
            catch (Throwable error) {
                runOnUiThread(() -> { busy = false; setGameMode(false); status.setText("Native startup failed: " + error); refreshRunEnabled(); });
                return;
            }
            runOnUiThread(() -> {
                setGameMode(false);
                status.setText("Native startup returned " + result + (detail == null || detail.isEmpty() ? "" : "\n" + detail));
                busy = false;
                refreshRunEnabled();
            });
        });
    }

    @Override protected void onDestroy() {
        if (physicalPad != null) physicalPad.close();
        if (controller != null) controller.setActive(false);
        nativeResetInput();
        nativeSetSurface(null);
        worker.shutdownNow();
        super.onDestroy();
    }
}

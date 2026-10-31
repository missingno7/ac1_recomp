#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "ac1_runtime.h"
#include "ac1_input_script.h"
#include "ac1_gamepad.h"
#include "ac1_audio_mixer.h"

#define AC1_LOG_TAG "AC1Recomp"
static _Thread_local char bridge_error[512];
static atomic_int guest_running;
static pthread_mutex_t window_mutex = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *current_window;

#include "vulkan_driver.h"

void ac1_android_surface_size(int32_t *width, int32_t *height) {
    pthread_mutex_lock(&window_mutex);
    *width = current_window ? ANativeWindow_getWidth(current_window) : 1280;
    *height = current_window ? ANativeWindow_getHeight(current_window) : 720;
    pthread_mutex_unlock(&window_mutex);
}

JNIEXPORT void JNICALL
Java_org_ac1_recomp_LauncherActivity_nativeKey(JNIEnv *env, jclass type, jint dik, jboolean down) {
    (void)env; (void)type;
    ac1_input_physical_key((unsigned)dik, down);
}
JNIEXPORT void JNICALL
Java_org_ac1_recomp_LauncherActivity_nativeMouse(JNIEnv *env, jclass type, jint dx, jint dy, jint wheel, jint buttons) {
    (void)env; (void)type;
    ac1_input_physical_mouse(dx, dy, wheel, (unsigned)buttons);
}
JNIEXPORT void JNICALL
Java_org_ac1_recomp_LauncherActivity_nativeResetInput(JNIEnv *env, jclass type) {
    (void)env; (void)type; ac1_input_physical_reset(); ac1_pad_reset();
}
JNIEXPORT void JNICALL
Java_org_ac1_recomp_LauncherActivity_nativeAudioPause(JNIEnv *env, jclass type, jboolean paused) {
    (void)env; (void)type; ac1_audio_device_pause(paused);
}

extern void *Direct3DCreate9(uint32_t sdk_version);
extern void dxvkAndroidSetWindow(ANativeWindow *window);
extern void *dxvkAndroidGetWindowHandle(void);
extern void dxvkAndroidSetRenderSize(uint32_t width, uint32_t height);

/* Render at the display aspect: AC1_RENDER_SIZE=WxH, else display height
 * capped at 720 lines (GPU cost) with the matching even width. */
static void select_render_size(uint32_t *width, uint32_t *height) {
    int32_t screen_width, screen_height;
    const char *display = getenv("AC1_DISPLAY_SIZE"); /* physical display, set by the launcher */
    if (!display || sscanf(display, "%dx%d", &screen_width, &screen_height) != 2 ||
        screen_width <= 0 || screen_height <= 0)
        ac1_android_surface_size(&screen_width, &screen_height);
    if (screen_width < screen_height) { int32_t t = screen_width; screen_width = screen_height; screen_height = t; }
    const char *setting = getenv("AC1_RENDER_SIZE");
    unsigned w = 0, h = 0;
    if (setting && sscanf(setting, "%ux%u", &w, &h) == 2 && w >= 320 && h >= 240 && w <= 8192 && h <= 8192) {
        *width = w; *height = h; return;
    }
    h = screen_height > 720 ? 720u : (unsigned)screen_height;
    w = (unsigned)(((uint64_t)screen_width * h / (unsigned)screen_height) & ~UINT64_C(1));
    *width = w; *height = h;
}

/* A fresh profile has no [Graphics] section and AC1 then picks 640x480.
 * Seed the display size once; the game rewrites the file with its choice. */
static void seed_graphics_settings(const char *userdata, uint32_t width, uint32_t height) {
    static const char *const parts[] = {"roaming", "Ubisoft", "Assassin's Creed"};
    char path[4096];
    size_t length = (size_t)snprintf(path, sizeof(path), "%s", userdata);
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]) && length < sizeof(path); ++i) {
        length += (size_t)snprintf(path + length, sizeof(path) - length, "/%s", parts[i]);
        if (mkdir(path, 0700) != 0 && errno != EEXIST) return;
    }
    if (length + 16u >= sizeof(path)) return;
    strcat(path, "/Assassin.ini");
    char existing[8192] = {0};
    FILE *file = fopen(path, "rb");
    if (file) { fread(existing, 1, sizeof(existing) - 1u, file); fclose(file); }
    if (strstr(existing, "[Graphics]")) return;
    file = fopen(path, "wb");
    if (!file) return;
    fprintf(file, "[Graphics]\r\nFullscreen=1\r\nDisplayWidth=%u\r\nDisplayHeight=%u\r\nRefreshRate=60\r\n%s",
            width, height, existing);
    fclose(file);
    __android_log_print(ANDROID_LOG_INFO, AC1_LOG_TAG, "Seeded %ux%u graphics settings", width, height);
}

void *ac1_android_d3d9_factory(void *context, uint32_t sdk_version) {
    (void)context;
    pthread_mutex_lock(&window_mutex);
    int have_window = current_window != NULL;
    pthread_mutex_unlock(&window_mutex);
    if (!have_window) return NULL;
    return Direct3DCreate9(sdk_version);
}

uintptr_t ac1_android_d3d9_guest_hwnd(void *context, uint32_t guest_hwnd) {
    (void)context;
    if (!guest_hwnd) return 0;
    return (uintptr_t)dxvkAndroidGetWindowHandle();
}

JNIEXPORT jboolean JNICALL
Java_org_ac1_recomp_LauncherActivity_nativeSetSurface(JNIEnv *env, jclass type,
                                                       jobject surface) {
    (void)type;
    ANativeWindow *next = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    if (surface && !next) return JNI_FALSE;
    pthread_mutex_lock(&window_mutex);
    ANativeWindow *previous = current_window;
    if (next == previous) {
        pthread_mutex_unlock(&window_mutex);
        if (next) ANativeWindow_release(next);
        return JNI_TRUE;
    }
    current_window = next;
    /* DXVK acquires its own native-window reference and owns the synthetic
       HWND published to D3D9. Update it before releasing our previous ref. */
    dxvkAndroidSetWindow(next);
    pthread_mutex_unlock(&window_mutex);
    if (previous) ANativeWindow_release(previous);
    return JNI_TRUE;
}

static jint run_original(JNIEnv *env, jstring exe_path, jstring userdata_path,
                         jstring script_path, jboolean skip_intros, jboolean trace_imports) {
    if (exe_path == NULL || userdata_path == NULL) {
        snprintf(bridge_error, sizeof(bridge_error), "Executable or userdata path is missing");
        return -1;
    }
    const char *path = (*env)->GetStringUTFChars(env, exe_path, NULL);
    if (path == NULL) return -2;
    const char *userdata = (*env)->GetStringUTFChars(env, userdata_path, NULL);
    if (userdata == NULL) {
        (*env)->ReleaseStringUTFChars(env, exe_path, path);
        return -2;
    }
    const char *script = script_path ? (*env)->GetStringUTFChars(env, script_path, NULL) : NULL;
    if (script_path && !script) {
        (*env)->ReleaseStringUTFChars(env, userdata_path, userdata);
        (*env)->ReleaseStringUTFChars(env, exe_path, path);
        return -2;
    }
    if (script && *script) setenv("AC1_INPUT_SCRIPT", script, 1);
    else unsetenv("AC1_INPUT_SCRIPT");
    unsetenv("AC1_INPUT_RECORD");
    setenv("AC1_D3D9_FORCE_WINDOWED", "1", 1);
    /* Like the host gameplay launchers: worker loops legitimately exceed the
     * startup-diagnostic dispatch budget once the device runs at speed. */
    setenv("AC1_DISPATCH_LIMIT", "0", 1);
    uint32_t render_width, render_height;
    select_render_size(&render_width, &render_height);
    dxvkAndroidSetRenderSize(render_width, render_height);
    seed_graphics_settings(userdata, render_width, render_height);
    __android_log_print(ANDROID_LOG_INFO, AC1_LOG_TAG, "Calling original startup for %s", path);
    int result;
    if (!ac1_set_userdata_root(userdata)) {
        snprintf(bridge_error, sizeof(bridge_error), "Invalid app-private userdata directory: %.400s", userdata);
        result = -3;
    } else if (setenv("DXVK_SHADER_CACHE_PATH", userdata, 1) != 0 ||
               setenv("MESA_SHADER_CACHE_DIR", userdata, 1) != 0 ||
               setenv("DXVK_WSI_DRIVER", "Android", 1) != 0) {
        snprintf(bridge_error, sizeof(bridge_error), "Cannot configure app-private DXVK state cache: %s", strerror(errno));
        result = -3;
    } else if (setenv("AC1_TRACE_IMPORTS", trace_imports ? "1" : "0", 1) != 0) {
        snprintf(bridge_error, sizeof(bridge_error), "Cannot configure import tracing: %s", strerror(errno));
        result = -3;
    } else if ((skip_intros ? setenv("AC1_HIDE_FILES",
                   "videos\\UBI_LOGO.bik;videos\\Ubisoft_Presents.bik;"
                   "videos\\Warning_Disclaimer.bik;videos\\DemoIntro.bik", 1)
                           : unsetenv("AC1_HIDE_FILES")) != 0) {
        snprintf(bridge_error, sizeof(bridge_error), "Cannot configure intro visibility: %s", strerror(errno));
        result = -3;
    } else {
        char log_path[32768];
        int log_fd = -1, saved_out = -1, saved_err = -1;
        if (snprintf(log_path, sizeof(log_path), "%s/startup.log", userdata) < (int)sizeof(log_path))
            log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (log_fd >= 0) {
            fflush(stdout); fflush(stderr);
            saved_out = dup(STDOUT_FILENO); saved_err = dup(STDERR_FILENO);
            if (saved_out >= 0 && saved_err >= 0) {
                dup2(log_fd, STDOUT_FILENO); dup2(log_fd, STDERR_FILENO);
            }
            close(log_fd);
        }
        bridge_error[0] = '\0';
        if (!configure_vulkan_driver()) {
            result = -4;
            fprintf(stderr, "AC1 Vulkan initialization failed: %s\n", bridge_error);
        } else {
            result = ac1_run(path);
            const char *detail = ac1_last_error();
            if (detail) snprintf(bridge_error, sizeof(bridge_error), "%s", detail);
            else bridge_error[0] = '\0';
        }
        fflush(stdout); fflush(stderr);
        if (saved_out >= 0) { dup2(saved_out, STDOUT_FILENO); close(saved_out); }
        if (saved_err >= 0) { dup2(saved_err, STDERR_FILENO); close(saved_err); }
    }
    __android_log_print(ANDROID_LOG_INFO, AC1_LOG_TAG, "Original startup returned %d", result);
    (*env)->ReleaseStringUTFChars(env, userdata_path, userdata);
    (*env)->ReleaseStringUTFChars(env, exe_path, path);
    if (script) (*env)->ReleaseStringUTFChars(env, script_path, script);
    return (jint)result;
}

JNIEXPORT jint JNICALL
Java_org_ac1_recomp_LauncherActivity_nativeRun(JNIEnv *env, jclass type,
                                               jstring exe_path, jstring userdata_path,
                                               jstring script_path, jboolean skip_intros,
                                               jboolean trace_imports) {
    (void)type;
    /* The runtime is process-global; a second guest (for example from a
     * recreated activity) would share and corrupt the running one's state. */
    if (atomic_exchange(&guest_running, 1)) {
        snprintf(bridge_error, sizeof(bridge_error),
                 "AC1 is already running in this process; a second guest was refused");
        return -5;
    }
    jint result = run_original(env, exe_path, userdata_path, script_path, skip_intros,
                               trace_imports);
    atomic_store(&guest_running, 0);
    return result;
}

JNIEXPORT jstring JNICALL
Java_org_ac1_recomp_LauncherActivity_nativeLastError(JNIEnv *env, jclass type) {
    (void)type;
    const char *detail = bridge_error;
    __android_log_print(ANDROID_LOG_INFO, AC1_LOG_TAG, "Startup boundary: %s",
                        *detail ? detail : "none");
    return (*env)->NewStringUTF(env, detail);
}

JNIEXPORT void JNICALL
Java_org_ac1_recomp_LauncherActivity_nativePad(JNIEnv *env, jclass type, jboolean connected,
        jint buttons, jint lt, jint rt, jint lx, jint ly, jint rx, jint ry) {
    (void)env; (void)type;
    ac1_pad pad = {(uint16_t)buttons, (uint8_t)lt, (uint8_t)rt,
                   (int16_t)lx, (int16_t)ly, (int16_t)rx, (int16_t)ry};
    ac1_pad_submit(0, connected, &pad);
}

JNIEXPORT jint JNICALL
Java_org_ac1_recomp_LauncherActivity_nativeCommitText(JNIEnv *env, jclass type, jstring text) {
    (void)type;
    if(!text)return -1;
    jsize length=(*env)->GetStringLength(env,text);
    const jchar *units=(*env)->GetStringChars(env,text,NULL);
    if(!units)return -2;
    int result=ac1_input_commit_text((const uint16_t *)units,(size_t)length);
    (*env)->ReleaseStringChars(env,text,units);return result;
}
JNIEXPORT jint JNICALL
Java_org_ac1_recomp_LauncherActivity_nativeTextKey(JNIEnv *env,jclass type,jint vk) {
    (void)env;(void)type;return ac1_input_text_key((unsigned)vk);
}
JNIEXPORT jboolean JNICALL
Java_org_ac1_recomp_LauncherActivity_nativeKeyText(JNIEnv *env,jclass type,jint dik,jboolean down,jint cp) {
    (void)env;(void)type;
    ac1_input_physical_key_text((unsigned)dik,down,(uint32_t)cp);
    return ac1_input_text_supported((uint32_t)cp)?JNI_TRUE:JNI_FALSE;
}

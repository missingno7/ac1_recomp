#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "ac1_runtime.h"
#include "ac1_input_script.h"

#define AC1_LOG_TAG "AC1Recomp"
static _Thread_local char bridge_error[512];
static pthread_mutex_t window_mutex = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *current_window;

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
    (void)env; (void)type; ac1_input_physical_reset();
}

extern void *Direct3DCreate9(uint32_t sdk_version);
extern void dxvkAndroidSetWindow(ANativeWindow *window);
extern void *dxvkAndroidGetWindowHandle(void);

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

JNIEXPORT jint JNICALL
Java_org_ac1_recomp_LauncherActivity_nativeRun(JNIEnv *env, jclass type,
                                               jstring exe_path, jstring userdata_path,
                                               jstring script_path) {
    (void)type;
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
    setenv("AC1_TRACE_IMPORTS", "1", 1);
    setenv("AC1_D3D9_FORCE_WINDOWED", "1", 1);
    __android_log_print(ANDROID_LOG_INFO, AC1_LOG_TAG, "Calling original startup for %s", path);
    int result;
    if (!ac1_set_userdata_root(userdata)) {
        snprintf(bridge_error, sizeof(bridge_error), "Invalid app-private userdata directory: %.400s", userdata);
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
        result = ac1_run(path);
        const char *detail = ac1_last_error();
        if (detail) snprintf(bridge_error, sizeof(bridge_error), "%s", detail);
        else bridge_error[0] = '\0';
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

JNIEXPORT jstring JNICALL
Java_org_ac1_recomp_LauncherActivity_nativeLastError(JNIEnv *env, jclass type) {
    (void)type;
    const char *detail = bridge_error;
    __android_log_print(ANDROID_LOG_INFO, AC1_LOG_TAG, "Startup boundary: %s",
                        *detail ? detail : "none");
    return (*env)->NewStringUTF(env, detail);
}

#include <jni.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "ac1_runtime.h"

#define AC1_LOG_TAG "AC1Recomp"
static _Thread_local char bridge_error[512];
static pthread_mutex_t window_mutex = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *current_window;

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
                                               jstring exe_path, jstring userdata_path) {
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
    __android_log_print(ANDROID_LOG_INFO, AC1_LOG_TAG, "Calling original startup for %s", path);
    int result;
    if (!ac1_set_userdata_root(userdata)) {
        snprintf(bridge_error, sizeof(bridge_error), "Invalid app-private userdata directory: %.400s", userdata);
        result = -3;
    } else {
        bridge_error[0] = '\0';
        result = ac1_run(path);
        const char *detail = ac1_last_error();
        if (detail) snprintf(bridge_error, sizeof(bridge_error), "%s", detail);
        else bridge_error[0] = '\0';
    }
    __android_log_print(ANDROID_LOG_INFO, AC1_LOG_TAG, "Original startup returned %d", result);
    (*env)->ReleaseStringUTFChars(env, userdata_path, userdata);
    (*env)->ReleaseStringUTFChars(env, exe_path, path);
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

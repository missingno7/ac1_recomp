#ifndef AC1_RUNTIME_H
#define AC1_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

/* Execute the installed PE32 Assassin's Creed executable through the native
 * lifted-function dispatcher. Returns 0 after a natural guest entry return,
 * 8 when execution stops at an unsupported guest boundary, and another
 * nonzero code for launch/verification/runtime failures. The path is borrowed
 * for this call and must be readable by the calling worker thread. */
int ac1_run(const char *exe_path);

/* Set an absolute, writable app-owned directory for Windows special folders.
 * Android launchers should pass a child of their private files directory.
 * The host build defaults to the workspace's work/userdata directory. */
int ac1_set_userdata_root(const char *path);

/* Diagnostic text from the last run on this thread. It remains valid until
 * the next ac1_run call on the same thread. */
const char *ac1_last_error(void);

#ifdef __cplusplus
}
#endif
#endif

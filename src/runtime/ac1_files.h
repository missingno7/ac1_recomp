#ifndef AC1_FILES_H
#define AC1_FILES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ac1_files ac1_files;

typedef struct ac1_files_overlapped_snapshot {
    uint32_t internal;
    uint32_t internal_high;
    uint32_t valid;
} ac1_files_overlapped_snapshot;

enum {
    AC1_FILES_NOT_HANDLED = 0,
    AC1_FILES_HANDLED = 1,
    AC1_FILES_UNSUPPORTED = 2
};

/* Create a read-only asset service rooted at the directory containing the
 * guest executable. The service never exposes a host handle to guest code. */
ac1_files *ac1_files_create(const char *game_root);

/* Variant for runtimes whose host asset directory differs from the guest’s
 * absolute Windows install path. Absolute guest paths are accepted only under
 * this configured logical root; relative paths remain rooted at host_game_root.
 * Pass NULL for guest_windows_root to disable absolute guest paths. */
ac1_files *ac1_files_create_with_guest_root(const char *host_game_root,
                                            const char *guest_windows_root);

/* Close every outstanding host file before destroying the service. */
void ac1_files_destroy(ac1_files *files);

/* Cancel and drain all native overlapped reads while guest memory backing
 * their buffers is still mapped. Call after guest workers stop, before
 * releasing guest address-space mappings. File handles remain open. */
void ac1_files_cancel_pending(ac1_files *files);

/* True while a native overlapped read still needs result polling. */
int ac1_files_has_pending(const ac1_files *files);

/* Narrow read-only CreateFileA implementation for observed AC1 game assets.
 * `guest_path` must be a safe relative path beneath the game directory, or an
 * absolute Windows path beneath the configured logical guest root.
 * Only GENERIC_READ, OPEN_EXISTING, no security/template handles, and the
 * observed archive flags (0x10000000) or Gear stream flags (0x70000001) are
 * accepted. Returns
 * AC1_FILES_HANDLED for a Win32 result (including a
 * native open failure), AC1_FILES_UNSUPPORTED for an unimplemented non-write
 * form that the runtime should diagnose, or AC1_FILES_NOT_HANDLED if the
 * service/output pointers are unavailable. Read-only policy rejects write
 * requests with ERROR_ACCESS_DENIED. `error_out` is a Win32 error when handled. */
int ac1_files_open(ac1_files *files, const char *guest_path,
                   uint32_t desired_access, uint32_t share_mode,
                   int security_attributes_present,
                   uint32_t creation_disposition, uint32_t flags_and_attributes,
                   uint32_t template_handle, uint32_t *guest_handle_out,
                   uint32_t *error_out);

/* Close only handles allocated by this service. Returns AC1_FILES_HANDLED when
 * token belongs to this service (including a host close failure), otherwise
 * AC1_FILES_NOT_HANDLED for an unrelated token.
 */
int ac1_files_close(ac1_files *files, uint32_t guest_handle,
                    uint32_t *error_out);

/* GetFileSizeEx subset. Returns the exact 64-bit size for a service-owned
 * handle; the runtime marshals it to the guest LARGE_INTEGER. */
int ac1_files_get_size(ac1_files *files, uint32_t guest_handle,
                       uint64_t *size_out, uint32_t *error_out);

/* Synchronous ReadFile subset used by the archive helpers. `buffer` and the
 * output pointers are host views of validated guest memory, never guest
 * addresses. EOF is a successful read with zero bytes. */
int ac1_files_read(ac1_files *files, uint32_t guest_handle, void *buffer,
                   uint32_t requested_bytes, uint32_t *bytes_read_out,
                   uint32_t *error_out);

/* Narrow overlapped ReadFile subset. `guest_overlapped_address` identifies a
 * stable guest OVERLAPPED; offsets are supplied separately from its guest
 * fields, and the module owns a native OVERLAPPED until same-key reuse or
 * close/drain. `host_event` is the native HANDLE resolved from guest hEvent;
 * POSIX hosts ignore it.
 * A zero error means immediate completion; 997 means native I/O is pending. */
int ac1_files_read_overlapped(ac1_files *files, uint32_t guest_handle,
                              void *buffer, uint32_t requested_bytes,
                              uint32_t guest_overlapped_address,
                              uint32_t offset_low, uint32_t offset_high,
                              void *host_event,
                              uint32_t *error_out,
                              ac1_files_overlapped_snapshot *snapshot_out);

/* GetOverlappedResult subset. The observed caller polls with wait=0. A pending
 * native operation returns HANDLED/ERROR_IO_INCOMPLETE and remains registered;
 * terminal completion returns the count/status without consuming the operation
 * key, matching native repeated-query behavior. A later same-key ReadFile may
 * reuse terminal state; pending state must finish first. */
int ac1_files_get_overlapped_result(ac1_files *files, uint32_t guest_handle,
                                    uint32_t guest_overlapped_address,
                                    uint32_t wait,
                                    uint32_t *bytes_transferred_out,
                                    uint32_t *error_out,
                                    ac1_files_overlapped_snapshot *snapshot_out);

/* SetFilePointer semantics for a signed low/high displacement. When
 * `has_distance_high` is zero, distance_low is sign-extended as a LONG; when
 * nonzero, distance_high:distance_low is a signed 64-bit displacement and
 * position_high_out is required. `move_method` is FILE_BEGIN/CURRENT/END
 * (0/1/2). Outputs are valid only when error_out is zero. */
int ac1_files_seek(ac1_files *files, uint32_t guest_handle,
                   uint32_t distance_low, int has_distance_high,
                   uint32_t distance_high, uint32_t move_method,
                   uint32_t *position_low_out, uint32_t *position_high_out,
                   uint32_t *error_out);

#ifdef __cplusplus
}
#endif

#endif

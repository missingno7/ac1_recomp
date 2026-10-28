#ifndef AC1_FILES_H
#define AC1_FILES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ac1_files ac1_files;

enum {
    AC1_FILES_NOT_HANDLED = 0,
    AC1_FILES_HANDLED = 1,
    AC1_FILES_UNSUPPORTED = 2
};

/* Create a read-only asset service rooted at the directory containing the
 * guest executable. The service never exposes a host handle to guest code. */
ac1_files *ac1_files_create(const char *game_root);

/* Close every outstanding host file before destroying the service. */
void ac1_files_destroy(ac1_files *files);

/* Narrow CreateFileA implementation for the observed AC1 archive probe.
 * `guest_path` must be a single relative filename. Only GENERIC_READ,
 * OPEN_EXISTING, no security/template handles, and FILE_FLAG_RANDOM_ACCESS
 * are accepted. Returns AC1_FILES_HANDLED for a Win32 result (including a
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

/* Synchronous ReadFile subset used by the archive helpers. `buffer` and the
 * output pointers are host views of validated guest memory, never guest
 * addresses. EOF is a successful read with zero bytes. */
int ac1_files_read(ac1_files *files, uint32_t guest_handle, void *buffer,
                   uint32_t requested_bytes, uint32_t *bytes_read_out,
                   uint32_t *error_out);

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

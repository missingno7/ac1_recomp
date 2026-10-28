#ifndef DUKEMP_PE32_LOADER_H
#define DUKEMP_PE32_LOADER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define PE32_API __declspec(dllexport)
#else
#define PE32_API __attribute__((visibility("default")))
#endif

typedef struct {
    uint8_t *image;
    uint32_t image_size;
    uint32_t preferred_base;
    uint32_t load_base;
    uint32_t entry_rva;
    uint32_t export_rva;
    uint32_t export_size;
    uint32_t import_rva;
    uint32_t import_size;
    uint32_t relocation_rva;
    uint32_t relocation_size;
    uint32_t delay_import_rva;
    uint32_t delay_import_size;
    uint32_t characteristics;
} pe32_module;

/* Resolver returns nonzero and writes a guest VA/token on success. name is
 * NULL for an ordinal import; ordinal is zero for a named import. */
typedef int (*pe32_import_resolver)(void *context, const char *dll_name,
                                    const char *name, uint16_t ordinal,
                                    uint32_t *guest_target,
                                    char *error, size_t error_capacity);

PE32_API int pe32_load(const uint8_t *file, size_t file_size, uint32_t load_base,
                       pe32_module *module, char *error, size_t error_capacity);
PE32_API void pe32_unload(pe32_module *module);

/* Return 1 on success, 0 on a missing symbol/error. Forwarded exports are
 * rejected explicitly; this minimal loader does not follow forwarders. */
PE32_API int pe32_export_by_name(const pe32_module *module, const char *name,
                                 uint32_t *guest_address, char *error,
                                 size_t error_capacity);
PE32_API int pe32_export_by_ordinal(const pe32_module *module, uint16_t ordinal,
                                    uint32_t *guest_address, char *error,
                                    size_t error_capacity);

/* Resolve every normal import before changing any IAT entry. Unresolved APIs
 * and delay imports fail loudly. */
PE32_API int pe32_link_imports(pe32_module *module, pe32_import_resolver resolver,
                               void *context, char *error, size_t error_capacity);

#ifdef __cplusplus
}
#endif
#endif

#include "pe32_loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PE32_MACHINE_I386 0x014cu
#define PE32_MAGIC 0x010bu
#define PE32_RELOCS_STRIPPED 0x0001u
#define PE32_DIRECTORY_EXPORT 0u
#define PE32_DIRECTORY_IMPORT 1u
#define PE32_DIRECTORY_RELOC 5u
#define PE32_DIRECTORY_DELAY_IMPORT 13u
#define PE32_IMPORT_ORDINAL 0x80000000u
#define PE32_MAX_SECTIONS 96u
#define PE32_MAX_IAT_PATCHES 1048576u

typedef struct {
    uint32_t iat_rva;
    uint32_t guest_target;
} iat_patch;

static void set_error(char *error, size_t capacity, const char *format, ...)
{
    va_list args;
    if (!error || capacity == 0)
        return;
    va_start(args, format);
    (void)vsnprintf(error, capacity, format, args);
    va_end(args);
    error[capacity - 1] = '\0';
}

static int fail(char *error, size_t capacity, const char *format, ...)
{
    va_list args;
    if (error && capacity != 0) {
        va_start(args, format);
        (void)vsnprintf(error, capacity, format, args);
        va_end(args);
        error[capacity - 1] = '\0';
    }
    return 0;
}

static int file_range(size_t size, uint64_t offset, uint64_t length)
{
    return offset <= size && length <= (uint64_t)size - offset;
}

static int image_range(uint32_t size, uint64_t rva, uint64_t length)
{
    return rva <= size && length <= (uint64_t)size - rva;
}

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static int get_directory(const uint8_t *optional, uint16_t optional_size,
                         uint32_t directory_count, uint32_t index,
                         uint32_t *rva, uint32_t *size,
                         char *error, size_t error_capacity)
{
    uint32_t offset;
    if (index >= directory_count) {
        *rva = 0;
        *size = 0;
        return 1;
    }
    offset = 96u + index * 8u;
    if (offset > optional_size || 8u > (uint32_t)optional_size - offset)
        return fail(error, error_capacity, "optional header truncates data directory %u", index);
    *rva = read_u32(optional + offset);
    *size = read_u32(optional + offset + 4u);
    if ((*rva == 0) != (*size == 0))
        return fail(error, error_capacity, "data directory %u has only RVA or size", index);
    return 1;
}

static int c_string_at(const pe32_module *module, uint32_t rva,
                       const char **value, char *error, size_t error_capacity)
{
    const uint8_t *start;
    const uint8_t *end;
    if (!image_range(module->image_size, rva, 1))
        return fail(error, error_capacity, "string RVA 0x%08x is outside image", rva);
    start = module->image + rva;
    end = (const uint8_t *)memchr(start, 0, module->image_size - rva);
    if (!end)
        return fail(error, error_capacity, "string at RVA 0x%08x is unterminated", rva);
    *value = (const char *)start;
    return 1;
}

static int validate_module_address(uint32_t base, uint32_t image_size,
                                   char *error, size_t error_capacity)
{
    if (image_size == 0 || image_size > UINT32_MAX ||
        (uint64_t)base + image_size > UINT64_C(0x100000000))
        return fail(error, error_capacity,
                    "module address range wraps: base=0x%08x size=0x%08x",
                    base, image_size);
    return 1;
}

static int apply_relocations(pe32_module *module, uint32_t reloc_rva,
                             uint32_t reloc_size, int relocations_stripped,
                             char *error, size_t error_capacity)
{
    uint64_t cursor = 0;
    int64_t delta = (int64_t)(uint64_t)module->load_base -
                    (int64_t)(uint64_t)module->preferred_base;
    if (relocations_stripped && reloc_size != 0)
        return fail(error, error_capacity,
                    "image marks relocations stripped but has a relocation directory");
    if (reloc_size == 0 || relocations_stripped) {
        if (delta == 0)
            return 1;
        return fail(error, error_capacity,
                    "cannot rebase image 0x%08x: relocations are stripped or absent",
                    module->preferred_base);
    }
    if (!image_range(module->image_size, reloc_rva, reloc_size))
        return fail(error, error_capacity, "relocation directory is outside image");
    while (cursor < reloc_size) {
        uint32_t page_rva, block_size;
        uint32_t entry_count, i;
        const uint8_t *block;
        if ((uint64_t)reloc_size - cursor < 8)
            return fail(error, error_capacity, "truncated relocation block header");
        block = module->image + reloc_rva + (size_t)cursor;
        page_rva = read_u32(block);
        block_size = read_u32(block + 4);
        if (block_size < 8 || (block_size & 1u) != 0 ||
            block_size > (uint64_t)reloc_size - cursor)
            return fail(error, error_capacity,
                        "invalid relocation block at directory offset 0x%08x",
                        (uint32_t)cursor);
        if ((page_rva & 0xfffu) != 0 || page_rva >= module->image_size)
            return fail(error, error_capacity, "relocation page RVA is outside image");
        entry_count = (block_size - 8u) / 2u;
        for (i = 0; i < entry_count; ++i) {
            uint16_t entry = read_u16(block + 8u + (size_t)i * 2u);
            uint16_t type = entry >> 12;
            uint32_t target_rva = page_rva + (entry & 0x0fffu);
            uint32_t value;
            if (type == 0) /* IMAGE_REL_BASED_ABSOLUTE padding */
                continue;
            if (type != 3) /* IMAGE_REL_BASED_HIGHLOW */
                return fail(error, error_capacity,
                            "unsupported i386 relocation type %u at RVA 0x%08x",
                            type, target_rva);
            if (target_rva < page_rva ||
                !image_range(module->image_size, target_rva, 4))
                return fail(error, error_capacity,
                            "HIGHLOW relocation target is outside image");
            value = read_u32(module->image + target_rva);
            if (delta != 0)
                write_u32(module->image + target_rva,
                          (uint32_t)((uint64_t)value + (uint64_t)delta));
        }
        cursor += block_size;
    }
    return 1;
}

int pe32_load(const uint8_t *file, size_t file_size, uint32_t load_base,
              pe32_module *module, char *error, size_t error_capacity)
{
    uint32_t pe_offset, image_size, header_size, preferred_base, entry_rva;
    uint32_t export_rva, export_size, import_rva, import_size;
    uint32_t reloc_rva, reloc_size, delay_rva, delay_size;
    uint32_t section_alignment, file_alignment, directory_count;
    uint16_t section_count, optional_size, characteristics;
    const uint8_t *coff, *optional, *section_table;
    uint64_t section_table_offset, section_table_size;
    uint32_t i;
    pe32_module pending;
    int relocations_stripped;

    if (module)
        memset(module, 0, sizeof(*module));
    memset(&pending, 0, sizeof(pending));
    if (!file || !module)
        return fail(error, error_capacity, "null PE input or output module");
    if (!file_range(file_size, 0, 64) || read_u16(file) != 0x5a4d)
        return fail(error, error_capacity, "invalid or truncated DOS header");
    pe_offset = read_u32(file + 0x3c);
    if (!file_range(file_size, pe_offset, 24) ||
        read_u32(file + pe_offset) != 0x00004550u)
        return fail(error, error_capacity, "invalid or truncated PE signature/COFF header");
    coff = file + pe_offset + 4u;
    if (read_u16(coff) != PE32_MACHINE_I386)
        return fail(error, error_capacity, "PE machine is not IMAGE_FILE_MACHINE_I386");
    section_count = read_u16(coff + 2);
    optional_size = read_u16(coff + 16);
    characteristics = read_u16(coff + 18);
    if (section_count == 0 || section_count > PE32_MAX_SECTIONS)
        return fail(error, error_capacity, "unsupported PE section count %u", section_count);
    if (optional_size < 96 ||
        !file_range(file_size, (uint64_t)pe_offset + 24u, optional_size))
        return fail(error, error_capacity, "invalid or truncated PE32 optional header");
    optional = coff + 20u;
    if (read_u16(optional) != PE32_MAGIC)
        return fail(error, error_capacity, "optional header is not PE32");
    entry_rva = read_u32(optional + 16);
    preferred_base = read_u32(optional + 28);
    section_alignment = read_u32(optional + 32);
    file_alignment = read_u32(optional + 36);
    image_size = read_u32(optional + 56);
    header_size = read_u32(optional + 60);
    directory_count = read_u32(optional + 92);
    if (section_alignment == 0 || file_alignment == 0 || image_size == 0 ||
        image_size > 0x7fffffffu || header_size == 0 || header_size > image_size)
        return fail(error, error_capacity, "invalid PE32 image/header alignment or size");
    if (load_base % section_alignment != preferred_base % section_alignment)
        return fail(error, error_capacity,
                    "load base 0x%08x violates section alignment 0x%08x",
                    load_base, section_alignment);
    if (!validate_module_address(preferred_base, image_size, error, error_capacity))
        return 0;
    if (!file_range(file_size, 0, header_size))
        return fail(error, error_capacity, "PE32 headers exceed file size");
    if (entry_rva >= image_size)
        return fail(error, error_capacity, "entry point RVA is outside image");
    if (!validate_module_address(load_base, image_size, error, error_capacity))
        return 0;

    if (!get_directory(optional, optional_size, directory_count,
                       PE32_DIRECTORY_EXPORT, &export_rva, &export_size,
                       error, error_capacity) ||
        !get_directory(optional, optional_size, directory_count,
                       PE32_DIRECTORY_IMPORT, &import_rva, &import_size,
                       error, error_capacity) ||
        !get_directory(optional, optional_size, directory_count,
                       PE32_DIRECTORY_RELOC, &reloc_rva, &reloc_size,
                       error, error_capacity) ||
        !get_directory(optional, optional_size, directory_count,
                       PE32_DIRECTORY_DELAY_IMPORT, &delay_rva, &delay_size,
                       error, error_capacity))
        return 0;
    if ((export_size && !image_range(image_size, export_rva, export_size)) ||
        (import_size && !image_range(image_size, import_rva, import_size)) ||
        (reloc_size && !image_range(image_size, reloc_rva, reloc_size)) ||
        (delay_size && !image_range(image_size, delay_rva, delay_size)))
        return fail(error, error_capacity, "PE32 data directory exceeds mapped image");

    section_table_offset = (uint64_t)pe_offset + 24u + optional_size;
    section_table_size = (uint64_t)section_count * 40u;
    if (!file_range(file_size, section_table_offset, section_table_size))
        return fail(error, error_capacity, "section table exceeds file size");
    if (section_table_offset + section_table_size > header_size)
        return fail(error, error_capacity, "section table exceeds SizeOfHeaders");
    section_table = file + (size_t)section_table_offset;

    pending.image = (uint8_t *)calloc(1, image_size);
    if (!pending.image)
        return fail(error, error_capacity, "could not allocate 0x%08x-byte PE image", image_size);
    pending.image_size = image_size;
    pending.preferred_base = preferred_base;
    pending.load_base = load_base;
    pending.entry_rva = entry_rva;
    pending.export_rva = export_rva;
    pending.export_size = export_size;
    pending.import_rva = import_rva;
    pending.import_size = import_size;
    pending.relocation_rva = reloc_rva;
    pending.relocation_size = reloc_size;
    pending.delay_import_rva = delay_rva;
    pending.delay_import_size = delay_size;
    pending.characteristics = characteristics;
    relocations_stripped = (characteristics & PE32_RELOCS_STRIPPED) != 0;

    memcpy(pending.image, file, header_size);
    for (i = 0; i < section_count; ++i) {
        const uint8_t *section = section_table + (size_t)i * 40u;
        uint32_t virtual_size = read_u32(section + 8);
        uint32_t virtual_address = read_u32(section + 12);
        uint32_t raw_size = read_u32(section + 16);
        uint32_t raw_offset = read_u32(section + 20);
        uint32_t mapped_size = virtual_size > raw_size ? virtual_size : raw_size;
        uint32_t j;
        if (mapped_size == 0)
            continue;
        if (virtual_address < header_size ||
            !image_range(image_size, virtual_address, mapped_size)) {
            pe32_unload(&pending);
            return fail(error, error_capacity,
                        "section %u virtual range exceeds image or overlaps headers", i);
        }
        if (raw_size && !file_range(file_size, raw_offset, raw_size)) {
            pe32_unload(&pending);
            return fail(error, error_capacity, "section %u raw data exceeds file", i);
        }
        if (raw_size && (raw_offset < header_size || raw_offset % file_alignment != 0)) {
            pe32_unload(&pending);
            return fail(error, error_capacity,
                        "section %u raw range overlaps headers or violates file alignment", i);
        }
        for (j = 0; j < i; ++j) {
            const uint8_t *previous = section_table + (size_t)j * 40u;
            uint32_t previous_va = read_u32(previous + 12);
            uint32_t previous_raw = read_u32(previous + 16);
            uint32_t previous_virtual = read_u32(previous + 8);
            uint32_t previous_span = previous_virtual > previous_raw ? previous_virtual : previous_raw;
            uint64_t current_end = (uint64_t)virtual_address + mapped_size;
            uint64_t previous_end = (uint64_t)previous_va + previous_span;
            if (previous_span && virtual_address < previous_end && previous_va < current_end) {
                pe32_unload(&pending);
                return fail(error, error_capacity, "PE32 sections %u and %u overlap", j, i);
            }
        }
        if (raw_size)
            memcpy(pending.image + virtual_address, file + raw_offset, raw_size);
    }

    if (entry_rva != 0) {
        int executable = 0;
        for (i = 0; i < section_count; ++i) {
            const uint8_t *section = section_table + (size_t)i * 40u;
            uint32_t va = read_u32(section + 12);
            uint32_t raw_size = read_u32(section + 16);
            if ((read_u32(section + 36) & 0x20000000u) &&
                entry_rva >= va && (uint64_t)entry_rva < (uint64_t)va + raw_size) {
                executable = 1;
                break;
            }
        }
        if (!executable) {
            pe32_unload(&pending);
            return fail(error, error_capacity, "entry point is not inside executable section");
        }
    }
    if (!apply_relocations(&pending, reloc_rva, reloc_size, relocations_stripped,
                           error, error_capacity)) {
        pe32_unload(&pending);
        return 0;
    }
    *module = pending;
    set_error(error, error_capacity, "");
    return 1;
}

void pe32_unload(pe32_module *module)
{
    if (!module)
        return;
    free(module->image);
    memset(module, 0, sizeof(*module));
}

static int export_function_at(const pe32_module *module, uint32_t index,
                              uint32_t *guest_address, char *error,
                              size_t error_capacity)
{
    uint32_t directory, function_count, functions_rva, function_rva;
    uint64_t address;
    if (!module || !module->image || !guest_address || module->export_size < 40 ||
        !image_range(module->image_size, module->export_rva, 40))
        return fail(error, error_capacity, "module has no valid export directory");
    directory = module->export_rva;
    function_count = read_u32(module->image + directory + 20);
    functions_rva = read_u32(module->image + directory + 28);
    if (index >= function_count || !image_range(module->image_size, functions_rva,
                                                 (uint64_t)function_count * 4u))
        return fail(error, error_capacity, "export function index %u is outside table", index);
    function_rva = read_u32(module->image + functions_rva + (size_t)index * 4u);
    if (function_rva == 0)
        return fail(error, error_capacity, "export function index %u is empty", index);
    if (function_rva >= module->export_rva &&
        (uint64_t)function_rva < (uint64_t)module->export_rva + module->export_size)
        return fail(error, error_capacity, "forwarded exports are unsupported");
    if (!image_range(module->image_size, function_rva, 1))
        return fail(error, error_capacity, "export function RVA 0x%08x is outside image", function_rva);
    address = (uint64_t)module->load_base + function_rva;
    if (address > UINT32_MAX)
        return fail(error, error_capacity, "export guest address wraps");
    *guest_address = (uint32_t)address;
    return 1;
}

int pe32_export_by_ordinal(const pe32_module *module, uint16_t ordinal,
                           uint32_t *guest_address, char *error,
                           size_t error_capacity)
{
    uint32_t base, count, index;
    if (!module || !module->image || !guest_address || module->export_size < 40 ||
        !image_range(module->image_size, module->export_rva, 40))
        return fail(error, error_capacity, "module has no valid export directory");
    base = read_u32(module->image + module->export_rva + 16);
    count = read_u32(module->image + module->export_rva + 20);
    if ((uint32_t)ordinal < base || (uint32_t)ordinal - base >= count)
        return fail(error, error_capacity, "export ordinal %u is not defined", ordinal);
    index = (uint32_t)ordinal - base;
    return export_function_at(module, index, guest_address, error, error_capacity);
}

int pe32_export_by_name(const pe32_module *module, const char *name,
                        uint32_t *guest_address, char *error,
                        size_t error_capacity)
{
    uint32_t name_count, names_rva, ordinals_rva, i;
    if (!module || !module->image || !name || !guest_address || module->export_size < 40 ||
        !image_range(module->image_size, module->export_rva, 40))
        return fail(error, error_capacity, "invalid module, export name, or export directory");
    name_count = read_u32(module->image + module->export_rva + 24);
    names_rva = read_u32(module->image + module->export_rva + 32);
    ordinals_rva = read_u32(module->image + module->export_rva + 36);
    if (!image_range(module->image_size, names_rva, (uint64_t)name_count * 4u) ||
        !image_range(module->image_size, ordinals_rva, (uint64_t)name_count * 2u))
        return fail(error, error_capacity, "export name/ordinal table exceeds image");
    for (i = 0; i < name_count; ++i) {
        uint32_t name_rva = read_u32(module->image + names_rva + (size_t)i * 4u);
        const char *candidate;
        uint16_t function_index;
        if (!c_string_at(module, name_rva, &candidate, error, error_capacity))
            return 0;
        if (strcmp(candidate, name) != 0)
            continue;
        function_index = read_u16(module->image + ordinals_rva + (size_t)i * 2u);
        return export_function_at(module, function_index, guest_address,
                                  error, error_capacity);
    }
    return fail(error, error_capacity, "export name '%s' is not defined", name);
}

static int append_patch(iat_patch **patches, size_t *count, size_t *capacity,
                        uint32_t rva, uint32_t target,
                        char *error, size_t error_capacity)
{
    iat_patch *grown;
    size_t next_capacity;
    if (*count >= PE32_MAX_IAT_PATCHES)
        return fail(error, error_capacity, "import table exceeds %u IAT entries",
                    PE32_MAX_IAT_PATCHES);
    if (*count == *capacity) {
        next_capacity = *capacity ? *capacity * 2u : 64u;
        if (next_capacity > PE32_MAX_IAT_PATCHES)
            next_capacity = PE32_MAX_IAT_PATCHES;
        grown = (iat_patch *)realloc(*patches, next_capacity * sizeof(**patches));
        if (!grown)
            return fail(error, error_capacity, "could not allocate IAT patch list");
        *patches = grown;
        *capacity = next_capacity;
    }
    (*patches)[*count].iat_rva = rva;
    (*patches)[*count].guest_target = target;
    ++*count;
    return 1;
}

int pe32_link_imports(pe32_module *module, pe32_import_resolver resolver,
                      void *context, char *error, size_t error_capacity)
{
    iat_patch *patches = NULL;
    size_t patch_count = 0, patch_capacity = 0;
    uint64_t descriptor_offset;
    int terminated = 0;
    if (!module || !module->image || !resolver)
        return fail(error, error_capacity, "null module or import resolver");
    if (module->delay_import_size != 0)
        return fail(error, error_capacity, "delay imports are unsupported");
    if (module->import_size == 0) {
        set_error(error, error_capacity, "");
        return 1;
    }
    if (!image_range(module->image_size, module->import_rva, module->import_size))
        return fail(error, error_capacity, "import directory exceeds image");
    for (descriptor_offset = 0;
         descriptor_offset + 20u <= module->import_size;
         descriptor_offset += 20u) {
        const uint8_t *descriptor = module->image + module->import_rva + (size_t)descriptor_offset;
        uint32_t original_thunk = read_u32(descriptor);
        uint32_t name_rva = read_u32(descriptor + 12);
        uint32_t first_thunk = read_u32(descriptor + 16);
        const char *dll_name;
        uint32_t lookup_rva, thunk_index;
        if (original_thunk == 0 && read_u32(descriptor + 4) == 0 &&
            read_u32(descriptor + 8) == 0 && name_rva == 0 && first_thunk == 0) {
            terminated = 1;
            break;
        }
        if (name_rva == 0 || first_thunk == 0 ||
            !c_string_at(module, name_rva, &dll_name, error, error_capacity)) {
            if (name_rva == 0 || first_thunk == 0)
                fail(error, error_capacity, "invalid import descriptor at RVA 0x%08x",
                     module->import_rva + (uint32_t)descriptor_offset);
            free(patches);
            return 0;
        }
        lookup_rva = original_thunk ? original_thunk : first_thunk;
        for (thunk_index = 0; ; ++thunk_index) {
            uint64_t lookup_slot = (uint64_t)lookup_rva + (uint64_t)thunk_index * 4u;
            uint64_t iat_slot = (uint64_t)first_thunk + (uint64_t)thunk_index * 4u;
            uint32_t thunk, target = 0;
            const char *import_name = NULL;
            uint16_t ordinal = 0;
            char resolver_error[256] = {0};
            if (!image_range(module->image_size, lookup_slot, 4) ||
                !image_range(module->image_size, iat_slot, 4)) {
                fail(error, error_capacity, "import thunk array exceeds image for '%s'", dll_name);
                free(patches);
                return 0;
            }
            thunk = read_u32(module->image + (size_t)lookup_slot);
            if (thunk == 0)
                break;
            if (thunk & PE32_IMPORT_ORDINAL) {
                if (thunk & 0x7fff0000u) {
                    fail(error, error_capacity, "invalid ordinal import encoding in '%s'", dll_name);
                    free(patches);
                    return 0;
                }
                ordinal = (uint16_t)thunk;
            } else {
                const char *ignored_hint_name;
                if (!image_range(module->image_size, thunk, 3) ||
                    !c_string_at(module, thunk + 2u, &import_name,
                                 error, error_capacity)) {
                    free(patches);
                    return 0;
                }
                ignored_hint_name = import_name;
                (void)ignored_hint_name;
            }
            if (!resolver(context, dll_name, import_name, ordinal, &target,
                          resolver_error, sizeof(resolver_error)) || target == 0) {
                if (import_name)
                    fail(error, error_capacity, "unresolved import %s!%s%s%s",
                         dll_name, import_name,
                         resolver_error[0] ? ": " : "", resolver_error);
                else
                    fail(error, error_capacity, "unresolved import %s!#%u%s%s",
                         dll_name, ordinal, resolver_error[0] ? ": " : "",
                         resolver_error);
                free(patches);
                return 0;
            }
            if (!append_patch(&patches, &patch_count, &patch_capacity,
                              (uint32_t)iat_slot, target, error, error_capacity)) {
                free(patches);
                return 0;
            }
        }
    }
    if (!terminated) {
        free(patches);
        return fail(error, error_capacity, "import descriptor table lacks terminator");
    }
    {
        size_t i;
        for (i = 0; i < patch_count; ++i)
            write_u32(module->image + patches[i].iat_rva, patches[i].guest_target);
    }
    free(patches);
    set_error(error, error_capacity, "");
    return 1;
}

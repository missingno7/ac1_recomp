"""Compare the runtime's INI section reader with native Windows APIs.

Run with the project's Python environment on Windows; synthetic fixtures and
the small MinGW probe are written beneath ignored work/.
"""

from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / "src/runtime/ac1_runtime.c").read_text(encoding="utf-8")

def between(start, end):
    a = source.index(start)
    b = source.index(end, a)
    return source[a:b]

trim = between("static char *trim_profile_text(char *text)\n{",
               "static void trim_profile_trailing_blanks")
enumerate_sections = between(
    "static int profile_enumerate_sections(const char *path, char *output,",
    "static int handle_profile_import")

fixture = r'''#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

''' + trim + "\n" + enumerate_sections + r'''

static int compare_case(const char *label, const char *path, DWORD capacity)
{
    char ours[64];
    char native[64];
    char native_string[64];
    memset(ours, 0xcc, sizeof(ours));
    memset(native, 0xcc, sizeof(native));
    memset(native_string, 0xcc, sizeof(native_string));
    int opened = 0;
    int ours_count = profile_enumerate_sections(path, ours, capacity, &opened);
    WritePrivateProfileStringA(NULL, NULL, NULL, path); /* flush profile cache */
    DWORD native_count = GetPrivateProfileSectionNamesA(native, capacity, path);
    DWORD string_count = GetPrivateProfileStringA(NULL, NULL, "", native_string,
                                                   capacity, path);
    int same = ours_count == (int)native_count && ours_count == (int)string_count &&
               (!capacity || (memcmp(ours, native, capacity) == 0 &&
                              memcmp(ours, native_string, capacity) == 0));
    for (DWORD i = capacity; i < sizeof(ours); ++i)
        same &= (unsigned char)ours[i] == 0xcc;
    printf("%s cap=%lu opened=%d ours=%d sectionNames=%lu profileString=%lu bytes=", label,
           (unsigned long)capacity, opened, ours_count,
           (unsigned long)native_count, (unsigned long)string_count);
    for (DWORD i = 0; i < capacity && i < 16; ++i)
        printf("%02x", (unsigned char)ours[i]);
    printf(" / ");
    for (DWORD i = 0; i < capacity && i < 16; ++i)
        printf("%02x", (unsigned char)native[i]);
    printf(" / ");
    for (DWORD i = 0; i < capacity && i < 16; ++i)
        printf("%02x", (unsigned char)native_string[i]);
    printf(" %s\n", same ? "MATCH" : "DIFFER");
    return same;
}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    int ok = 1;
    const DWORD capacities[] = {0, 1, 2, 6, 7, 8, 12, 64};
    for (unsigned i = 0; i < sizeof(capacities) / sizeof(capacities[0]); ++i)
        ok &= compare_case("multi", argv[1], capacities[i]);
    ok &= compare_case("single-exact", argv[2], 7);
    ok &= compare_case("single-short", argv[2], 6);
    ok &= compare_case("empty", argv[3], 2);
    ok &= compare_case("empty-large", argv[3], 16);
    ok &= compare_case("missing", "Z:/definitely_missing_ac1_profile.ini", 16);
    return ok ? 0 : 1;
}
'''

work = root / "work"
c_path = work / "profile_section_oracle_fixture.c"
exe_path = work / "profile_section_oracle_fixture.exe"
c_path.write_text(fixture, encoding="utf-8")
gcc = Path(r"C:/msys64/mingw64/bin/gcc.exe")
subprocess.run([str(gcc), "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(c_path), "-o", str(exe_path)], check=True)

multi = work / "profile_section_multi.ini"
single = work / "profile_section_single.ini"
empty = work / "profile_section_empty.ini"
multi.write_bytes(b"[Alpha]\r\nk=1\r\n[Beta]\r\nk=2\r\n[alpha]\r\nk=3\r\n")
single.write_bytes(b"[Alpha]\r\nk=1\r\n")
empty.write_bytes(b"; no sections\r\nkey=value\r\n")
subprocess.run([str(exe_path), str(multi.resolve()), str(single.resolve()),
                str(empty.resolve())],
               check=True)

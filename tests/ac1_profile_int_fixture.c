#include "ac1_profile_int.h"

#include <stdio.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        return 1; \
    } \
} while (0)

typedef struct {
    const char *text;
    int found;
    uint32_t fallback;
    uint32_t expected;
} profile_int_case;

int main(void)
{
    static const profile_int_case cases[] = {
        {NULL, 0, 0x87654321u, 0x87654321u}, /* missing key */
        {"42", 0, 0x87654321u, 0x87654321u},
        {NULL, 1, 0x87654321u, 0x87654321u},
        {"", 1, 0x87654321u, 0x87654321u},
        {"   \t", 1, 0x87654321u, 0x87654321u},
        {"\"\"", 1, 0x87654321u, 0x87654321u},
        {"\"  \"", 1, 0x87654321u, 0u},
        {"42", 1, 99u, 42u},
        {"010", 1, 99u, 10u},
        {"08", 1, 99u, 8u},
        {"-2147483648", 1, 99u, 0x80000000u},
        {"-2147483649", 1, 99u, 0x7FFFFFFFu},
        {"2147483648", 1, 99u, 0x80000000u},
        {"4294967295", 1, 99u, 0xFFFFFFFFu},
        {"4294967296", 1, 99u, 0u},
        {"0x2A", 1, 99u, 42u},
        {"0X2A", 1, 99u, 0u},
        {"0xDEADbeef", 1, 99u, 0xDEADBEEFu},
        {"-0x2A", 1, 99u, 0xFFFFFFD6u},
        {"+17", 1, 99u, 17u},
        {"   42  \t", 1, 99u, 42u},
        {"\"42\"", 1, 99u, 42u},
        {"\" 42 \"", 1, 99u, 42u},
        {"'42'", 1, 99u, 42u},
        {"''", 1, 0x87654321u, 0x87654321u},
        {"'  '", 1, 99u, 0u},
        {"\"", 1, 99u, 0u},
        {"'", 1, 99u, 0u},
        {"42xyz", 1, 99u, 42u},
        {"xyz", 1, 99u, 0u},
        {"-0", 1, 99u, 0u},
        {"0o10", 1, 99u, 8u},
        {"0b10", 1, 99u, 2u},
        {"0x2Ag", 1, 99u, 42u},
        {"0o18", 1, 99u, 1u},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        uint32_t actual = ac1_profile_int_parse(cases[i].text, cases[i].found,
                                                cases[i].fallback);
        CHECK(actual == cases[i].expected);
    }
    printf("PASS: %zu GetPrivateProfileIntA parsing cases\n",
           sizeof(cases) / sizeof(cases[0]));
    return 0;
}

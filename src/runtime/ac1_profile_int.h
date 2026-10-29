#ifndef AC1_PROFILE_INT_H
#define AC1_PROFILE_INT_H

#include <stdint.h>
#include <stddef.h>

static inline int ac1_profile_int_is_space(unsigned char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
           ch == '\v' || ch == '\f';
}

static inline unsigned ac1_profile_int_digit(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return (unsigned)(ch - '0');
    if (ch >= 'a' && ch <= 'f') return (unsigned)(ch - 'a') + 10u;
    if (ch >= 'A' && ch <= 'F') return (unsigned)(ch - 'A') + 10u;
    return 16u;
}

/* GetPrivateProfileIntA returns the supplied default for absent or empty
 * values. Nonempty values parse a numeric prefix, defaulting to decimal; the
 * lowercase 0x/0o/0b prefixes select their explicit radix. Accumulation and
 * signed negation intentionally wrap to the API's 32-bit UINT result. */
static inline uint32_t ac1_profile_int_parse(const char *value, int found,
                                             uint32_t default_value)
{
    if (!found || !value) return default_value;

    const unsigned char *begin = (const unsigned char *)value;
    while (ac1_profile_int_is_space(*begin)) ++begin;
    const unsigned char *end = begin;
    while (*end) ++end;
    while (end > begin && ac1_profile_int_is_space(end[-1])) --end;

    if (end - begin >= 2 &&
        ((*begin == '"' && end[-1] == '"') ||
         (*begin == '\'' && end[-1] == '\''))) {
        ++begin;
        --end;
    }
    if (begin == end) return default_value;
    while (begin < end && ac1_profile_int_is_space(*begin)) ++begin;

    int negative = 0;
    if (*begin == '+' || *begin == '-') {
        negative = *begin == '-';
        ++begin;
    }

    unsigned base = 10u;
    if (end - begin >= 2 && begin[0] == '0') {
        if (begin[1] == 'x') { base = 16u; begin += 2; }
        else if (begin[1] == 'o') { base = 8u; begin += 2; }
        else if (begin[1] == 'b') { base = 2u; begin += 2; }
    }

    uint32_t result = 0u;
    while (begin < end) {
        unsigned digit = ac1_profile_int_digit(*begin);
        if (digit >= base) break;
        result = result * base + digit;
        ++begin;
    }
    return negative ? (uint32_t)(0u - result) : result;
}

#endif

/* C library functions a Tactility device app needs and Tactility does not
 * export to apps.
 *
 * An app's undefined symbols are resolved only against the tables Tactility's
 * kernel modules export (c-symbols-module and the rest); the loader refuses an
 * app that names anything else. The toolchain's own libc.a cannot fill the
 * gaps: it is not position-independent, so its code cannot call into the
 * firmware, and its string conversions would bring a second malloc with them.
 * So the few functions the engine and packages use that are missing are
 * defined here, built as part of the app, out of functions Tactility does
 * export. The build reports any other missing symbol by name
 * (TactilityBuilder.cpp).
 */

#if defined(DEKI_TACTILITY_TARGET) && defined(ESP_PLATFORM) && !defined(DEKI_EDITOR)

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <freertos/FreeRTOS.h>

/* 64-bit atomics (std::atomic<double>, std::atomic<int64_t>). The ESP32-S3 has
 * no 64-bit atomic instruction, so the compiler calls these, and Tactility
 * exports them only for the ESP32-P4. They are made the way ESP-IDF's own
 * newlib makes them on Xtensa: under a critical section, whose functions
 * Tactility does export. One lock for every 64-bit atomic in the app. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wbuiltin-declaration-mismatch"

static portMUX_TYPE s_Atomic64 = portMUX_INITIALIZER_UNLOCKED;

uint64_t __atomic_load_8(const volatile void* mem, int order)
{
    (void)order;
    vPortEnterCritical(&s_Atomic64);
    const uint64_t value = *(const volatile uint64_t*)mem;
    vPortExitCritical(&s_Atomic64);
    return value;
}

void __atomic_store_8(volatile void* mem, uint64_t value, int order)
{
    (void)order;
    vPortEnterCritical(&s_Atomic64);
    *(volatile uint64_t*)mem = value;
    vPortExitCritical(&s_Atomic64);
}

uint64_t __atomic_exchange_8(volatile void* mem, uint64_t value, int order)
{
    (void)order;
    vPortEnterCritical(&s_Atomic64);
    const uint64_t old = *(volatile uint64_t*)mem;
    *(volatile uint64_t*)mem = value;
    vPortExitCritical(&s_Atomic64);
    return old;
}

bool __atomic_compare_exchange_8(volatile void* mem, void* expected, uint64_t desired, bool weak, int success,
                                 int failure)
{
    (void)weak;
    (void)success;
    (void)failure;
    vPortEnterCritical(&s_Atomic64);
    const uint64_t current = *(volatile uint64_t*)mem;
    const bool equal = current == *(uint64_t*)expected;
    if (equal)
    {
        *(volatile uint64_t*)mem = desired;
    }
    else
    {
        *(uint64_t*)expected = current;
    }
    vPortExitCritical(&s_Atomic64);
    return equal;
}

#define DEKI_ATOMIC_FETCH_8(name, op)                                                                                  \
    uint64_t __atomic_fetch_##name##_8(volatile void* mem, uint64_t value, int order)                                  \
    {                                                                                                                  \
        (void)order;                                                                                                   \
        vPortEnterCritical(&s_Atomic64);                                                                               \
        const uint64_t old = *(volatile uint64_t*)mem;                                                                 \
        *(volatile uint64_t*)mem = old op value;                                                                       \
        vPortExitCritical(&s_Atomic64);                                                                                \
        return old;                                                                                                    \
    }

DEKI_ATOMIC_FETCH_8(add, +)
DEKI_ATOMIC_FETCH_8(sub, -)
DEKI_ATOMIC_FETCH_8(and, &)
DEKI_ATOMIC_FETCH_8(or, |)
DEKI_ATOMIC_FETCH_8(xor, ^)

#undef DEKI_ATOMIC_FETCH_8
#pragma GCC diagnostic pop

/* Half away from zero, as C specifies. floor() keeps the fraction exact, which
 * x + 0.5 would not (0.49999999999999994 + 0.5 rounds to 1). */
long lround(double x)
{
    const double a = x < 0 ? -x : x;
    double r = floor(a);
    if (a - r >= 0.5)
    {
        r += 1.0;
    }
    return x < 0 ? -(long)r : (long)r;
}

long lroundf(float x)
{
    const float a = x < 0 ? -x : x;
    float r = floorf(a);
    if (a - r >= 0.5f)
    {
        r += 1.0f;
    }
    return x < 0 ? -(long)r : (long)r;
}

/* The float next to `from` toward `to` (std::nextafter, deki-2d's cropped
 * text). Floats of one sign are ordered as their bit patterns are, so the
 * neighbour is one step in the bits: up when moving away from zero, down when
 * moving toward it. From zero it is the smallest subnormal of the side `to` is
 * on. */
float nextafterf(float from, float to)
{
    if (isnan(from) || isnan(to))
    {
        return from + to;
    }
    if (from == to)
    {
        return to;
    }
    union
    {
        float f;
        uint32_t u;
    } v = { from };
    if (from == 0.0f)
    {
        v.u = 1;
        return to > 0.0f ? v.f : -v.f;
    }
    if ((from < to) == (from > 0.0f))
    {
        ++v.u;
    }
    else
    {
        --v.u;
    }
    return v.f;
}

float strtof(const char* s, char** end)
{
    return (float)strtod(s, end);
}

long long strtoll(const char* s, char** end, int base)
{
    const char* p = s;
    while (isspace((unsigned char)*p))
    {
        ++p;
    }
    int negative = 0;
    if (*p == '+' || *p == '-')
    {
        negative = *p == '-';
        ++p;
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2]))
    {
        p += 2;
        base = 16;
    }
    else if (base == 0)
    {
        base = p[0] == '0' ? 8 : 10;
    }
    if (base < 2 || base > 36)
    {
        if (end)
        {
            *end = (char*)s;
        }
        errno = EINVAL;
        return 0;
    }

    /* Accumulated as a magnitude, so LLONG_MIN fits. */
    const unsigned long long limit = negative ? (unsigned long long)LLONG_MAX + 1ULL : (unsigned long long)LLONG_MAX;
    unsigned long long value = 0;
    int any = 0;
    int overflow = 0;
    for (;; ++p)
    {
        int digit;
        const unsigned char c = (unsigned char)*p;
        if (c >= '0' && c <= '9')
        {
            digit = c - '0';
        }
        else if (c >= 'a' && c <= 'z')
        {
            digit = c - 'a' + 10;
        }
        else if (c >= 'A' && c <= 'Z')
        {
            digit = c - 'A' + 10;
        }
        else
        {
            break;
        }
        if (digit >= base)
        {
            break;
        }
        any = 1;
        if (overflow || value > (limit - (unsigned long long)digit) / (unsigned long long)base)
        {
            overflow = 1;
            continue;
        }
        value = value * (unsigned long long)base + (unsigned long long)digit;
    }

    if (end)
    {
        *end = (char*)(any ? p : s);
    }
    if (overflow)
    {
        errno = ERANGE;
        return negative ? LLONG_MIN : LLONG_MAX;
    }
    return negative ? (long long)(0ULL - value) : (long long)value;
}

/* The length of s, looking at no more than max bytes (the Tiled package reads
 * fixed-size names out of its binary maps with it). */
size_t strnlen(const char* s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n] != '\0')
    {
        ++n;
    }
    return n;
}

/* Unsigned 64-bit division, which the compiler calls for a uint64_t `/` on a
 * 32-bit core; it lives in libgcc, and Tactility does not export it. Operands
 * that fit 32 bits take the core's own divide. The rest go bit by bit, with
 * shifts by one only: a `/`, or a shift by a variable amount, on 64-bit values
 * here could call into libgcc again. */
uint64_t __udivdi3(uint64_t n, uint64_t d)
{
    if ((n >> 32) == 0 && (d >> 32) == 0)
    {
        return d == 0 ? 0 : (uint32_t)n / (uint32_t)d;
    }
    if (d == 0)
    {
        return 0;
    }
    uint64_t q = 0;
    uint64_t r = 0;
    for (int i = 0; i < 64; ++i)
    {
        /* A remainder with its top bit set is past any divisor once shifted:
         * the bit that falls off is that carry. */
        const uint64_t carry = r >> 63;
        r = (r << 1) | (n >> 63);
        n <<= 1;
        q <<= 1;
        if (carry || r >= d)
        {
            r -= d;
            q |= 1;
        }
    }
    return q;
}

#endif

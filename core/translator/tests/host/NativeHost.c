#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef unsigned long size_t_alias;

long test_main(void);

void host_print_i64(const char* label, long value) { printf("%s=%ld\n", label, value); }
void host_print_f64(const char* label, double value) { printf("%s=%.9g\n", label, value); }
void host_print_f32(const char* label, float value) { printf("%s=%.7g\n", label, (double)value); }

int host_printf(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int written = vprintf(format, arguments);
    va_end(arguments);
    return written;
}

void host_qsort(void* base, unsigned long count, unsigned long size, int (*compare)(const void*, const void*)) {
    qsort(base, count, size, compare);
}

long host_thread_create(void* (*body)(void*), void* argument) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, body, argument) != 0) abort();
    return (long)thread;
}

void host_thread_join(long handle) { pthread_join((pthread_t)handle, NULL); }

long host_sum8(long a, long b, long c, long d, long e, long f, long g, long h) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;
}

double host_mix(int a, double b, float c, long d, double e) { return a * 1000.0 + b * 100.0 + c * 10.0 + d + e / 10.0; }

long host_clock_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000000000L + now.tv_nsec;
}

int main(void) {
    long result = test_main();
    printf("result=%ld\n", result);
    return 0;
}

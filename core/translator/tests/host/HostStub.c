typedef unsigned long size_t;
void host_print_i64(const char* label, long value) {}
void host_print_f64(const char* label, double value) {}
void host_print_f32(const char* label, float value) {}
int host_printf(const char* format, ...) { return 0; }
void host_qsort(void* base, size_t count, size_t size, int (*compare)(const void*, const void*)) {}
long host_thread_create(void* (*body)(void*), void* argument) { return 0; }
void host_thread_join(long handle) {}
long host_sum8(long a, long b, long c, long d, long e, long f, long g, long h) { return 0; }
double host_mix(int a, double b, float c, long d, double e) { return 0; }
void* memcpy(void* destination, const void* source, size_t size) { return destination; }
void* memset(void* destination, int value, size_t size) { return destination; }
long host_clock_ns(void) { return 0; }
float fmaf(float a, float b, float c) { return 0; }
int setjmp(long* buffer) { return 0; }
void longjmp(long* buffer, int value) {}

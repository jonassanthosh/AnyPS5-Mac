#include "Host.h"

static long Fib(long n) { return n < 2 ? n : Fib(n - 1) + Fib(n - 2); }

static long Classify(int k) {
    switch (k) {
    case 0: return 7;
    case 1: return Fib(k + 10);
    case 2: return -3;
    case 3: return k * 1000;
    case 4: return 42;
    case 5: return 0x7fffffff;
    case 6: return -1L << 40;
    case 7: return k ^ 0x55;
    default: return -99;
    }
}

static long Square(long x) { return x * x; }
static long Negate(long x) { return -x; }
static long Halve(long x) { return x / 2; }
static long (*const operations[])(long) = {Square, Negate, Halve};

static unsigned long Mix(unsigned long a, unsigned long b) {
    unsigned __int128 wide = (unsigned __int128)a * b;
    unsigned long hi = (unsigned long)(wide >> 64);
    return (hi ^ (unsigned long)wide) + (a >> 7) + (b << 13) + (a % 97) + (long)a / -13 + __builtin_popcountl(a);
}

long test_main(void) {
    long total = 0;
    for (int k = -2; k < 11; ++k) total += Classify(k);
    host_print_i64("switch", total);
    host_print_i64("fib25", Fib(25));
    long acc = 3;
    for (int i = 0; i < 30; ++i) acc = operations[i % 3](acc + i);
    host_print_i64("fnptr", acc);
    unsigned long hash = 0x9e3779b97f4a7c15ul;
    for (unsigned long i = 1; i < 2000; ++i) hash = Mix(hash, i * 0x100000001b3ul);
    host_print_i64("mix", (long)hash);
    int counts[3] = {0, 0, 0};
    for (int i = -50; i < 50; ++i) {
        signed char c = (signed char)(i * 7);
        unsigned short u = (unsigned short)(i * 3001);
        counts[0] += c < 0;
        counts[1] += (u >> 3) & 1;
        counts[2] += (i * i) % 5 == 0;
    }
    host_print_i64("narrow", counts[0] * 10000 + counts[1] * 100 + counts[2]);
    host_print_i64("sum8", host_sum8(1, 2, 3, 4, 5, 6, 7, 8));
    host_printf("printf %d %s %ld %x %.3f %c\n", -12, "str", 1234567890123L, 0xbeef, 2.5, 'Z');
    return total ^ acc;
}

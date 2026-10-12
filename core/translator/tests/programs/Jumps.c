#include "Host.h"

static long buffer[32];
static long depthReached;

static long Descend(long depth, long limit) {
    depthReached = depth;
    if (depth == limit) longjmp(buffer, (int)depth);
    return Descend(depth + 1, limit) + depth;
}

static long Recurse(long n, long* outer) {
    if (n == 0) {
        long inner[32];
        volatile long hits = 0;
        if (setjmp(inner) == 0) {
            hits = hits + 1;
            longjmp(inner, 7);
        }
        hits = hits + 10;
        return hits;
    }
    return Recurse(n - 1, outer) * 2;
}

long test_main(void) {
    volatile long total = 0;
    volatile long rounds = 0;
    int value = setjmp(buffer);
    total = total + value;
    rounds = rounds + 1;
    if (rounds <= 5) Descend(1, rounds * 3);
    host_print_i64("total", total);
    host_print_i64("rounds", rounds);
    host_print_i64("depth", depthReached);
    host_print_i64("recursive", Recurse(6, 0));
    return total * 100 + rounds;
}

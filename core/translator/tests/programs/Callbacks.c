#include "Host.h"

struct Item {
    int key;
    int payload;
};

static int CompareItems(const void* a, const void* b) {
    const struct Item* x = a;
    const struct Item* y = b;
    return (x->key > y->key) - (x->key < y->key);
}

static int CompareDescending(const void* a, const void* b) {
    return *(const long*)b > *(const long*)a ? 1 : *(const long*)b < *(const long*)a ? -1 : 0;
}

long test_main(void) {
    struct Item items[40];
    unsigned seed = 12345;
    for (int i = 0; i < 40; ++i) {
        seed = seed * 1103515245u + 12345u;
        items[i].key = (int)(seed >> 16) % 1000;
        items[i].payload = i;
    }
    host_qsort(items, 40, sizeof(items[0]), CompareItems);
    long checksum = 0;
    for (int i = 0; i < 40; ++i) checksum = checksum * 31 + items[i].key * 64 + items[i].payload;
    host_print_i64("sorted", checksum);
    long values[25];
    for (int i = 0; i < 25; ++i) values[i] = (i * 7919) % 101 - 50;
    host_qsort(values, 25, sizeof(values[0]), CompareDescending);
    host_print_i64("first", values[0]);
    host_print_i64("last", values[24]);
    struct Item copy[40];
    memcpy(copy, items, sizeof(items));
    memset(items, 0, sizeof(items));
    host_print_i64("copied", copy[39].key + items[39].key);
    return checksum & 0xffff;
}

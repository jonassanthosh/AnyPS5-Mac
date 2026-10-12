#include "Host.h"

#define N 96

static float A[N][N], B[N][N], C[N][N];

static void MatMul(void) {
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < N; ++k) sum += A[i][k] * B[k][j];
            C[i][j] = sum;
        }
}

static unsigned Crc32(const unsigned char* data, int length, unsigned crc) {
    crc = ~crc;
    for (int i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

struct Node {
    int value;
    int next;
};

static struct Node nodes[4096];

static long Chase(int steps) {
    long sum = 0;
    int at = 0;
    for (int i = 0; i < steps; ++i) {
        sum += nodes[at].value;
        at = nodes[at].next;
    }
    return sum;
}

long test_main(void) {
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            A[i][j] = (float)((i * 7 + j) % 13) * 0.5f;
            B[i][j] = (float)((i + j * 3) % 11) * 0.25f;
        }
    for (int i = 0; i < 4096; ++i) {
        nodes[i].value = i * 3;
        nodes[i].next = (i * 1597 + 11) % 4096;
    }
    static unsigned char buffer[65536];
    for (int i = 0; i < 65536; ++i) buffer[i] = (unsigned char)(i * 131 + 7);

    long start = host_clock_ns();
    for (int r = 0; r < 20; ++r) MatMul();
    long matmul = host_clock_ns();
    unsigned crc = 0;
    for (int r = 0; r < 40; ++r) crc = Crc32(buffer, 65536, crc);
    long crcDone = host_clock_ns();
    long chased = Chase(20000000);
    long end = host_clock_ns();

    host_print_f32("c00", C[0][0] + C[N - 1][N - 1]);
    host_print_i64("crc", crc);
    host_print_i64("chase", chased);
    host_printf("time matmul_ms=%ld crc_ms=%ld chase_ms=%ld\n", (matmul - start) / 1000000, (crcDone - matmul) / 1000000,
        (end - crcDone) / 1000000);
    return crc & 0xff;
}

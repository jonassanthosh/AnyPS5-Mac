#include "Host.h"

static double Poly(double x) { return ((0.5 * x - 1.25) * x + 3.0) * x - 0.75; }

static float Dot(const float* a, const float* b, int n) {
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) sum += a[i] * b[i];
    return sum;
}

static void Saxpy(float* y, const float* x, float a, int n) {
    for (int i = 0; i < n; ++i) y[i] = a * x[i] + y[i];
}

static double Sqrt(double x) { return __builtin_sqrt(x); }

long test_main(void) {
    float a[64], b[64];
    for (int i = 0; i < 64; ++i) {
        a[i] = (float)i * 0.25f - 3.0f;
        b[i] = 1.0f / (float)(i + 1);
    }
    host_print_f32("dot", Dot(a, b, 64));
    Saxpy(a, b, 2.5f, 64);
    host_print_f32("saxpy", Dot(a, a, 64));
    float fused = 0.0f;
    for (int i = 0; i < 64; ++i) fused = fmaf(a[i], b[i], fused);
    host_print_f32("fused", fused);
    double s = 0.0;
    for (int i = 0; i < 100; ++i) s += Poly(i * 0.01) + Sqrt((double)i);
    host_print_f64("poly", s);
    long truncated = 0;
    for (int i = -20; i < 20; ++i) truncated += (long)(i * 1.7) + (int)(float)(i * -2.3);
    host_print_i64("truncate", truncated);
    double m = host_mix(7, 0.5, 0.25f, 3, 9.0);
    host_print_f64("mix", m);
    int ordered = 0;
    for (int i = 0; i < 10; ++i) {
        double x = i - 4.5;
        ordered += (x < 0.0) + (x > 1.0) * 2 + (x == 0.5) * 4;
    }
    host_print_i64("compare", ordered);
    return (long)(s * 1000.0);
}

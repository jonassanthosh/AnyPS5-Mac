#include "Host.h"
#include <immintrin.h>

static unsigned long Mix(unsigned long hash, const void* data, int bytes) {
    const unsigned char* p = (const unsigned char*)data;
    for (int i = 0; i < bytes; ++i) hash = (hash ^ p[i]) * 0x100000001b3ul;
    return hash;
}

#define CHECK(name, value) do { __typeof__(value) v_ = (value); host_print_i64(name, (long)Mix(0xcbf29ce484222325ul, &v_, sizeof(v_))); } while (0)

long test_main(void) {
    volatile float seed = 1.5f;
    __m256 a = _mm256_setr_ps(seed, -2.25f, 3.0f, 4.5f, -5.0f, 6.75f, 0.0f / seed - 0.0f, 8.0f);
    __m256 b = _mm256_setr_ps(0.5f, 2.0f, -3.0f, 9.0f, -5.0f, 1.0f, 7.0f, -8.5f);
    __m128 x = _mm256_castps256_ps128(a);
    __m128 y = _mm256_castps256_ps128(b);
    __m256d da = _mm256_setr_pd(seed, -2.5, 3.25, 100.75);
    __m256d db = _mm256_setr_pd(0.5, 4.0, -3.25, 1e10);
    __m256i ia = _mm256_setr_epi32(1, -2, 3, 0x7fffffff, -5, 6, 70000, -8);
    __m256i ib = _mm256_setr_epi32(9, 8, -7, 6, 5, -4, 3, 2);
    __m128i xa = _mm256_castsi256_si128(ia);
    __m128i xb = _mm256_castsi256_si128(ib);
    CHECK("permilps", _mm256_permute_ps(a, 0x1b));
    CHECK("permilpsv", _mm256_permutevar_ps(a, ib));
    CHECK("permilpd", _mm256_permute_pd(da, 0x5));
    CHECK("insertps", _mm_insert_ps(x, y, 0x9a));
    CHECK("movshdup", _mm256_movehdup_ps(a));
    CHECK("movsldup", _mm_moveldup_ps(x));
    CHECK("movddup", _mm256_movedup_pd(da));
    CHECK("cmpps_lt", _mm256_cmp_ps(a, b, _CMP_LT_OQ));
    CHECK("cmpps_neq", _mm256_cmp_ps(a, b, _CMP_NEQ_UQ));
    CHECK("cmpps_unord", _mm256_cmp_ps(a, a, _CMP_UNORD_Q));
    CHECK("cmpss_le", _mm_cmp_ss(x, y, _CMP_LE_OS));
    CHECK("cmppd_ge", _mm256_cmp_pd(da, db, _CMP_GE_OQ));
    CHECK("extractps", _mm_extract_ps(x, 3));
    CHECK("pextrd", _mm_extract_epi32(xa, 3));
    CHECK("pextrq", _mm_extract_epi64(xa, 1));
    CHECK("pextrb", _mm_extract_epi8(xa, 13));
    CHECK("pinsrd", _mm_insert_epi32(xa, 0x12345678, 2));
    CHECK("pinsrb", _mm_insert_epi8(xa, 0x7f, 9));
    CHECK("blendvps", _mm256_blendv_ps(a, b, _mm256_castsi256_ps(ia)));
    CHECK("blendvpd", _mm256_blendv_pd(da, db, _mm256_castps_pd(b)));
    CHECK("pblendvb", _mm256_blendv_epi8(ia, ib, ia));
    CHECK("dpps", _mm256_dp_ps(a, b, 0xf3));
    CHECK("dpps2", _mm_dp_ps(x, y, 0x7c));
    CHECK("haddps", _mm256_hadd_ps(a, b));
    CHECK("haddpd", _mm256_hadd_pd(da, db));
    CHECK("addsub", _mm256_addsub_ps(a, b));
    CHECK("roundps", _mm256_round_ps(_mm256_mul_ps(a, b), _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC));
    CHECK("roundss", _mm_round_ss(x, y, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC));
    CHECK("sqrtps", _mm256_sqrt_ps(_mm256_mul_ps(b, b)));
    CHECK("cvtps2pd", _mm256_cvtps_pd(x));
    CHECK("cvtpd2ps", _mm256_cvtpd_ps(da));
    CHECK("cvttpd2dq", _mm256_cvttpd_epi32(da));
    CHECK("cvtps2dq", _mm256_cvtps_epi32(b));
    CHECK("cvtdq2pd", _mm256_cvtepi32_pd(xa));
    CHECK("cvtph", _mm256_cvtph_ps(_mm256_cvtps_ph(b, 0)));
    CHECK("shufpd", _mm256_shuffle_pd(da, db, 0x9));
    CHECK("pshuflw", _mm_shufflelo_epi16(xa, 0x4e));
    CHECK("pshufhw", _mm256_shufflehi_epi16(ia, 0x1b));
    CHECK("permps", _mm256_permutevar8x32_ps(a, _mm256_setr_epi32(7, 0, 6, 1, 5, 2, 4, 3)));
    CHECK("paddsw", _mm256_adds_epi16(ia, ib));
    CHECK("psubusb", _mm256_subs_epu8(ia, ib));
    CHECK("pavgb", _mm256_avg_epu8(ia, ib));
    CHECK("psadbw", _mm256_sad_epu8(ia, ib));
    CHECK("pmulhw", _mm256_mulhi_epi16(ia, ib));
    CHECK("pmuldq", _mm256_mul_epi32(ia, ib));
    CHECK("pminuw", _mm256_min_epu16(ia, ib));
    CHECK("pabsd", _mm256_abs_epi32(ia));
    CHECK("phminpos", _mm_minpos_epu16(xb));
    CHECK("psrld", _mm256_srl_epi32(ia, _mm_cvtsi32_si128(3)));
    CHECK("psrad", _mm256_sra_epi32(ia, _mm_cvtsi32_si128(40)));
    CHECK("pslldq", _mm256_bslli_epi128(ia, 5));
    CHECK("palignr", _mm256_alignr_epi8(ia, ib, 7));
    CHECK("pmovzxbq", _mm256_cvtepu8_epi64(xa));
    CHECK("ptest", _mm256_testz_si256(ia, ib) * 2 + _mm256_testc_si256(ia, ib));
    CHECK("bcast128", _mm256_broadcast_ps(&y));
    float masked[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK("maskload", _mm256_maskload_ps(masked, ia));
    _mm256_maskstore_ps(masked, ib, b);
    CHECK("maskstore", *(__m256*)masked);
    unsigned int big = 0x12345678u;
    CHECK("movbe", (unsigned)_loadbe_i32(&big));
    CHECK("bextr", _bextr_u64(0xfedcba9876543210ul, 12, 20));
    __m128i key = _mm_setr_epi32(0x2b7e1516, 0x28aed2a6, 0xabf71588, 0x09cf4f3c);
    __m128i state = _mm_setr_epi32(0x3243f6a8, 0x885a308d, 0x313198a2, 0xe0370734);
    CHECK("aesenc", _mm_aesenc_si128(state, key));
    CHECK("aesenclast", _mm_aesenclast_si128(state, key));
    CHECK("aesdec", _mm_aesdec_si128(state, key));
    CHECK("aesdeclast", _mm_aesdeclast_si128(state, key));
    CHECK("aesimc", _mm_aesimc_si128(state));
    CHECK("keygen", _mm_aeskeygenassist_si128(key, 0x1b));
    CHECK("pclmul", _mm_clmulepi64_si128(state, key, 0x11));
    _Alignas(16) unsigned long pair[2] = {1, 2};
    CHECK("cmpxchg16b", __sync_bool_compare_and_swap((__int128*)pair, (__int128)1 | ((__int128)2 << 64), (__int128)7));
    CHECK("cmpxchg16b_value", pair[0] + pair[1] * 3);
    unsigned csr = _mm_getcsr();
    _mm_setcsr(csr);
    CHECK("mxcsr", _mm_getcsr() & ~0x3fu);
    return 0;
}

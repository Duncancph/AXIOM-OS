/* Kernel random numbers for cryptography.
 * Entropy: RDSEED/RDRAND (when the CPU has them), CPU timing jitter, and the timing of
 * network packets and keyboard/mouse input. Output: ChaCha20 keystream; the key is
 * replaced after every request ("fast key erasure"), so past output can't be recovered. */
#include "kernel.h"
#include "../user/lib/crypto/crypto.h"

static sha256_ctx pool;
static uint8_t key[32];
static uint64_t counter, events;
static bool have_rdrand, have_rdseed, seeded;

static bool rdrand64(uint64_t *v) {
    for (int i = 0; i < 10; i++) { uint8_t ok; __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok)); if (ok) return true; }
    return false;
}
static bool rdseed64(uint64_t *v) {
    for (int i = 0; i < 100; i++) { uint8_t ok; __asm__ volatile("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok)); if (ok) return true; cpu_relax(); }
    return false;
}

void random_add(const void *data, size_t n) { sha256_update(&pool, data, n); events++; }
void random_event(uint64_t x) { uint64_t t[2] = { rdtsc(), x }; random_add(t, sizeof t); }

static void jitter(int samples) {   /* timing noise from the CPU, caches and memory */
    static volatile uint8_t scratch[4096];
    uint64_t last = rdtsc();
    for (int i = 0; i < samples; i++) {
        for (int k = 0; k < (int)(last & 31) + 1; k++) scratch[(last >> 3 ^ k * 67) & 4095] += (uint8_t)k;
        inb(0x80);
        uint64_t now = rdtsc(), d = now - last;
        sha256_update(&pool, &d, sizeof d);
        last = now;
    }
}

static void reseed(void) {
    uint8_t h[32];
    uint64_t v;
    for (int i = 0; i < 8; i++) {
        if (have_rdseed && rdseed64(&v)) sha256_update(&pool, &v, 8);
        if (have_rdrand && rdrand64(&v)) sha256_update(&pool, &v, 8);
    }
    jitter(256);
    sha256_ctx c = pool;
    sha256_update(&c, key, 32);
    sha256_final(&c, h);
    memcpy(key, h, 32);
    secure_zero(h, 32);
}

void random_init(void) {
    uint32_t a, b, c, d;
    cpuid(1, 0, &a, &b, &c, &d); have_rdrand = (c >> 30) & 1;
    cpuid(0, 0, &a, &b, &c, &d);
    if (a >= 7) { cpuid(7, 0, &a, &b, &c, &d); have_rdseed = (b >> 18) & 1; }
    sha256_init(&pool);
    struct rtc_time t; rtc_read(&t); random_add(&t, sizeof t);
    random_add(&bootinfo, sizeof bootinfo.mmap);
    jitter(20000);
    reseed();
    seeded = true;
    LOG_OK("Random numbers: %s%s + timing jitter\n", have_rdseed ? "RDSEED, " : "", have_rdrand ? "RDRAND" : "no hardware source");
}

void random_bytes(void *out, size_t n) {
    uint8_t *o = out, blk[64], nonce[12] = { 0 };
    if (!seeded) random_init();
    reseed();                                   /* fold in new entropy every time */
    while (n) {
        memcpy(nonce, &counter, 8); counter++;
        chacha20_block(key, 0, nonce, blk);
        size_t k = n < 64 ? n : 64;
        memcpy(o, blk, k); o += k; n -= k;
    }
    memcpy(nonce, &counter, 8); counter++;      /* replace the key */
    chacha20_block(key, 0, nonce, blk);
    memcpy(key, blk, 32);
    secure_zero(blk, sizeof blk);
}

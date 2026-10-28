/* Generated from pinned MIT pcrecomp guest_mem.c; see prepare_runtime.py. */
#define HDR   16u            /* [size][magic][class][-] before every heap block */
#define MAGIC 0x48454150u    /* 'HEAP' */
#define GRAN  0x10000u       /* the reservation unit */

#define SMALL_MAX  0x8000u
#define CARVE      0x100000u
#define NCLASS     64
#define LARGE      0xFFu

/* Allocator state is supplied by the active heap adapter. */

static uint32_t class_size(int c) {       /* c 0..: 32, 48, 64, 80, 96, 112, 128, 160, 192, ... */
    if (c < 7) return 32u + 16u * (uint32_t)c;
    int k = c - 7;                         /* 128 * 2^(k/4) * (1 + (k%4+1)/4) */
    uint32_t p = 128u << (k / 4);
    return p + p / 4u * (uint32_t)(k % 4 + 1);
}
static int class_of(uint32_t total) {
    for (int c = 0; c < NCLASS; c++) if (class_size(c) >= total) return c;
    return -1;
}

static uint32_t small_alloc(int c) {
    uint32_t va = g_free[c];
    if (va) { g_free[c] = *(uint32_t *)host(va + HDR); return va; }
    uint32_t sz = class_size(c);
    if (g_carve + sz > g_carve_end || !g_carve) {
        uint32_t r = vm_reserve_locked(0, CARVE);
        if (!r) return 0;
        g_carve = r, g_carve_end = r + CARVE;
    }
    va = g_carve;
    g_carve += sz;
    return va;
}

void *hle_heap_alloc(uint32_t n, int zero) {
    hle_mem_init();
    if (n > 0xFFFFFFFFu - GRAN - HDR) return NULL;
    uint32_t total = n + HDR, va;
    int c = total <= SMALL_MAX ? class_of(total) : -1;
    pthread_mutex_lock(&g_mem_lock);
    va = c >= 0 ? small_alloc(c) : vm_reserve_locked(0, total);
    pthread_mutex_unlock(&g_mem_lock);
    if (!va) return NULL;
    uint32_t *h = (uint32_t *)host(va);
    h[0] = n, h[1] = MAGIC, h[2] = c >= 0 ? (uint32_t)c : LARGE, h[3] = 0;
    if (zero) memset(h + 4, 0, n);
    return h + 4;
}

static uint32_t capacity(const uint32_t *h) {
    return h[2] == LARGE ? ((h[0] + HDR + GRAN - 1) & ~(GRAN - 1)) - HDR : class_size((int)h[2]) - HDR;
}

void hle_heap_free(void *u) {
    if (!u) return;
    if (!hle_heap_ok(u)) { fprintf(stderr, "[win32hle] free of a block this heap did not hand out: %p\n", u); return; }
    uint32_t *h = (uint32_t *)u - 4;
    uint32_t va = hle_va(h);
    h[1] = 0;
    pthread_mutex_lock(&g_mem_lock);
    if (h[2] == LARGE) vm_release_locked(va);
    else { *(uint32_t *)u = g_free[h[2]]; g_free[h[2]] = va; }
    pthread_mutex_unlock(&g_mem_lock);
}

void *hle_heap_realloc(void *u, uint32_t n, int zero) {
    if (!u) return hle_heap_alloc(n, zero);
    if (!hle_heap_ok(u)) return NULL;
    uint32_t *h = (uint32_t *)u - 4, old = h[0];
    if (n <= capacity(h)) {                /* in place (a large block's capacity shrinks with it: harmless) */
        if (zero && n > old) memset((uint8_t *)u + old, 0, n - old);
        h[0] = n;
        return u;
    }
    void *p = hle_heap_alloc(n, zero);
    if (!p) return NULL;
    memcpy(p, u, old < n ? old : n);
    hle_heap_free(u);
    return p;
}
int hle_heap_ok(const void *u) {
    return u && ((const uint32_t *)((const uint8_t *)u - HDR))[1] == MAGIC;
}
uint32_t hle_heap_size(const void *u) {
    return hle_heap_ok(u) ? ((const uint32_t *)((const uint8_t *)u - HDR))[0] : 0;
}

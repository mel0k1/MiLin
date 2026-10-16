// sha512-crypt: проверка паролей /etc/shadow без libc.
#ifndef MILIN_CRYPT_H
#define MILIN_CRYPT_H

typedef unsigned long long cw64;
typedef unsigned char cb8;

struct sha512_ctx {
    cw64 h[8];
    cw64 total;
    cb8 buf[128];
    cw64 buflen;
};

static const cw64 sha512_k[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static const cw64 sha512_iv[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
    0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

static cw64 c_ror64(cw64 x, int n) {
    return (x >> n) | (x << (64 - n));
}

static void sha512_block(struct sha512_ctx *c, const cb8 *p) {
    cw64 w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((cw64)p[i * 8] << 56) | ((cw64)p[i * 8 + 1] << 48) |
               ((cw64)p[i * 8 + 2] << 40) | ((cw64)p[i * 8 + 3] << 32) |
               ((cw64)p[i * 8 + 4] << 24) | ((cw64)p[i * 8 + 5] << 16) |
               ((cw64)p[i * 8 + 6] << 8) | (cw64)p[i * 8 + 7];
    for (int i = 16; i < 80; i++) {
        cw64 s0 = c_ror64(w[i - 15], 1) ^ c_ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        cw64 s1 = c_ror64(w[i - 2], 19) ^ c_ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    cw64 a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    cw64 e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 80; i++) {
        cw64 S1 = c_ror64(e, 14) ^ c_ror64(e, 18) ^ c_ror64(e, 41);
        cw64 ch = (e & f) ^ (~e & g);
        cw64 t1 = h + S1 + ch + sha512_k[i] + w[i];
        cw64 S0 = c_ror64(a, 28) ^ c_ror64(a, 34) ^ c_ror64(a, 39);
        cw64 maj = (a & b) ^ (a & cc) ^ (b & cc);
        cw64 t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha512_init(struct sha512_ctx *c) {
    for (int i = 0; i < 8; i++) c->h[i] = sha512_iv[i];
    c->total = 0;
    c->buflen = 0;
}

static void sha512_update(struct sha512_ctx *c, const void *data, cw64 n) {
    const cb8 *p = data;
    c->total += n;
    while (n > 0) {
        cw64 take = 128 - c->buflen;
        if (take > n) take = n;
        for (cw64 i = 0; i < take; i++) c->buf[c->buflen + i] = p[i];
        c->buflen += take;
        p += take;
        n -= take;
        if (c->buflen == 128) {
            sha512_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

static void sha512_finish(struct sha512_ctx *c, cb8 *out) {
    cw64 bits = c->total * 8;
    cw64 bl = c->buflen;
    cw64 k = (bl <= 119) ? (119 - bl) : (247 - bl);
    cb8 tail[136];
    tail[0] = 0x80;
    for (cw64 i = 0; i < k; i++) tail[1 + i] = 0;
    for (int i = 0; i < 8; i++) tail[1 + k + i] = (cb8)(bits >> (56 - i * 8));
    sha512_update(c, tail, 1 + k + 8);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            out[i * 8 + j] = (cb8)(c->h[i] >> (56 - j * 8));
}

static const int sha512_tmap[64] = {
    42, 21,  0,  1, 43, 22, 23,  2, 44, 45, 24,  3,  4, 46, 25, 26,
     5, 47, 48, 27,  6,  7, 49, 28, 29,  8, 50, 51, 30,  9, 10, 52,
    31, 32, 11, 53, 54, 33, 12, 13, 55, 34, 35, 14, 56, 57, 36, 15,
    16, 58, 37, 38, 17, 59, 60, 39, 18, 19, 61, 40, 41, 20, 62, 63
};

static const char sha512_b64t[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static void sha512_b64(const cb8 *d, char *out) {
    cb8 t[64];
    for (int i = 0; i < 64; i++) t[i] = d[sha512_tmap[i]];
    int o = 0;
    for (int i = 0; i + 2 < 64; i += 3) {
        cw64 w = (cw64)t[i] | ((cw64)t[i + 1] << 8) | ((cw64)t[i + 2] << 16);
        out[o++] = sha512_b64t[w & 63];
        out[o++] = sha512_b64t[(w >> 6) & 63];
        out[o++] = sha512_b64t[(w >> 12) & 63];
        out[o++] = sha512_b64t[(w >> 18) & 63];
    }
    out[o++] = sha512_b64t[t[63] & 63];
    out[o++] = sha512_b64t[t[63] >> 6];
    out[o] = 0;
}

static unsigned c_uclen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static void sha512_crypt(const char *pw, unsigned pw_len,
                         const char *salt, unsigned salt_len, char *out86) {
    struct sha512_ctx c;
    cb8 dig[64], cur[64];
    cb8 pbuf[64], dsb[16];

    sha512_init(&c);
    sha512_update(&c, pw, pw_len);
    sha512_update(&c, salt, salt_len);
    sha512_update(&c, pw, pw_len);
    sha512_finish(&c, dig);

    sha512_init(&c);
    sha512_update(&c, pw, pw_len);
    sha512_update(&c, salt, salt_len);
    unsigned cnt;
    for (cnt = pw_len; cnt > 64; cnt -= 64)
        sha512_update(&c, dig, 64);
    sha512_update(&c, dig, cnt);
    for (unsigned i = pw_len; i > 0; i >>= 1) {
        if (i & 1) sha512_update(&c, dig, 64);
        else sha512_update(&c, pw, pw_len);
    }
    sha512_finish(&c, cur);

    sha512_init(&c);
    for (unsigned i = 0; i < pw_len; i++)
        sha512_update(&c, pw, pw_len);
    sha512_finish(&c, dig);
    for (unsigned i = 0; i < pw_len; i++) pbuf[i] = dig[i % 64];

    sha512_init(&c);
    for (unsigned i = 0; i < 16 + cur[0]; i++)
        sha512_update(&c, salt, salt_len);
    sha512_finish(&c, dig);
    for (unsigned i = 0; i < salt_len; i++) dsb[i] = dig[i];

    for (int r = 0; r < 5000; r++) {
        cb8 nxt[64];
        sha512_init(&c);
        if (r & 1) sha512_update(&c, pbuf, pw_len);
        else sha512_update(&c, cur, 64);
        if (r % 3) sha512_update(&c, dsb, salt_len);
        if (r % 7) sha512_update(&c, pbuf, pw_len);
        if (r & 1) sha512_update(&c, cur, 64);
        else sha512_update(&c, pbuf, pw_len);
        sha512_finish(&c, nxt);
        for (int i = 0; i < 64; i++) cur[i] = nxt[i];
    }
    sha512_b64(cur, out86);
}

static int sha512_verify(const char *pw, const char *salt, const char *enc) {
    char out[88];
    sha512_crypt(pw, c_uclen(pw), salt, c_uclen(salt), out);
    for (int i = 0; i < 86; i++) {
        if (out[i] != enc[i]) return 0;
        if (!enc[i]) return 0;
    }
    return enc[86] == 0;
}

#endif

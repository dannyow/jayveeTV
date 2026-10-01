#include "png.h"
#include <stdio.h>
#include <string.h>

// ---- CRC32 / Adler32 --------------------------------------------------------
static uint32_t crcTable[256];
static void crcInit() {
  if (crcTable[1]) return;
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crcTable[n] = c;
  }
}
static uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
  crcInit();
  for (size_t i = 0; i < n; i++) c = crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c;
}
static uint32_t adler32(const uint8_t* p, size_t n) {
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < n; i++) { a = (a + p[i]) % 65521; b = (b + a) % 65521; }
  return (b << 16) | a;
}

// ---- Bit writer (deflate is LSB-first; Huffman codes go in MSB-first) --------
struct BitW {
  std::vector<uint8_t>& out; uint32_t acc = 0; int n = 0;
  explicit BitW(std::vector<uint8_t>& o) : out(o) {}
  void put(uint32_t v, int bits) {
    acc |= v << n; n += bits;
    while (n >= 8) { out.push_back((uint8_t)(acc & 0xFF)); acc >>= 8; n -= 8; }
  }
  void putCode(uint32_t code, int bits) {         // Huffman code: reverse the bits
    uint32_t r = 0; for (int i = 0; i < bits; i++) r |= ((code >> i) & 1) << (bits - 1 - i);
    put(r, bits);
  }
  void flush() { if (n > 0) { out.push_back((uint8_t)(acc & 0xFF)); acc = 0; n = 0; } }
};

// Fixed Huffman literal/length codes (RFC 1951 §3.2.6).
static void putLit(BitW& w, int sym) {
  if (sym < 144)      w.putCode(0x30 + sym, 8);
  else if (sym < 256) w.putCode(0x190 + (sym - 144), 9);
  else if (sym < 280) w.putCode(sym - 256, 7);
  else                w.putCode(0xC0 + (sym - 280), 8);
}
static const uint16_t LBASE[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t  LEXTRA[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t DBASE[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t  DEXTRA[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static void putMatch(BitW& w, int len, int dist) {
  int li = 28; while (li > 0 && LBASE[li] > len) li--;
  putLit(w, 257 + li); if (LEXTRA[li]) w.put(len - LBASE[li], LEXTRA[li]);
  int di = 29; while (di > 0 && DBASE[di] > dist) di--;
  w.putCode(di, 5);    if (DEXTRA[di]) w.put(dist - DBASE[di], DEXTRA[di]);
}

void zlibCompress(const uint8_t* d, size_t n, std::vector<uint8_t>& out) {
  out.push_back(0x78); out.push_back(0x01);               // CMF/FLG: deflate, 32K window, level 0-1
  BitW w(out);
  w.put(1, 1); w.put(1, 2);                               // BFINAL=1, BTYPE=01 (fixed Huffman)
  const int HBITS = 15, WIN = 32768, MAXLEN = 258, CHAIN = 24;
  std::vector<int32_t> head((size_t)1 << HBITS, -1), prev(n > 0 ? n : 1, -1);
  auto hash3 = [&](size_t i) { return (uint32_t)((d[i] << 10) ^ (d[i + 1] << 5) ^ d[i + 2]) * 2654435761u >> (32 - HBITS); };
  auto insert = [&](size_t i) { if (i + 2 < n) { uint32_t h = hash3(i); prev[i] = head[h]; head[h] = (int32_t)i; } };
  size_t i = 0;
  while (i < n) {
    int best = 0, bestDist = 0;
    if (i + 2 < n) {
      int32_t p = head[hash3(i)]; int chain = 0;
      size_t maxLen = n - i < (size_t)MAXLEN ? n - i : (size_t)MAXLEN;
      while (p >= 0 && i - (size_t)p <= (size_t)WIN && chain++ < CHAIN) {
        if (d[p + best] == d[i + best]) {
          int len = 0; while ((size_t)len < maxLen && d[p + len] == d[i + len]) len++;
          if (len > best) { best = len; bestDist = (int)(i - p); if (len >= (int)maxLen) break; }
        }
        p = prev[p];
      }
    }
    if (best >= 3) { putMatch(w, best, bestDist); for (int k = 0; k < best; k++) insert(i + k); i += best; }
    else { putLit(w, d[i]); insert(i); i++; }
  }
  putLit(w, 256); w.flush();
  uint32_t a = adler32(d, n);
  out.push_back(a >> 24); out.push_back(a >> 16); out.push_back(a >> 8); out.push_back(a);
}

static void be32(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x); }
static void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> hdr; be32(hdr, (uint32_t)data.size());
  fwrite(hdr.data(), 1, 4, f);
  std::vector<uint8_t> body(type, type + 4); body.insert(body.end(), data.begin(), data.end());
  fwrite(body.data(), 1, body.size(), f);
  std::vector<uint8_t> c; be32(c, crc32(body.data(), body.size()) ^ 0xFFFFFFFFu);
  fwrite(c.data(), 1, 4, f);
}

bool pngWrite(const char* path, const uint8_t* rgb, int w, int h) {
  // Filter type 1 (Sub) per scanline: the 2× horizontal pixel doubling of the
  // panel picture turns into runs of zeros, which LZ77 eats.
  std::vector<uint8_t> raw((size_t)h * (1 + (size_t)w * 3));
  for (int y = 0; y < h; y++) {
    uint8_t* r = &raw[(size_t)y * (1 + (size_t)w * 3)]; const uint8_t* s = rgb + (size_t)y * w * 3;
    r[0] = 1; r[1] = s[0]; r[2] = s[1]; r[3] = s[2];
    for (int i = 3; i < w * 3; i++) r[1 + i] = (uint8_t)(s[i] - s[i - 3]);
  }
  std::vector<uint8_t> z; z.reserve(raw.size() / 4); zlibCompress(raw.data(), raw.size(), z);
  FILE* f = fopen(path, "wb"); if (!f) return false;
  static const uint8_t SIG[8] = {137,80,78,71,13,10,26,10}; fwrite(SIG, 1, 8, f);
  std::vector<uint8_t> ihdr; be32(ihdr, w); be32(ihdr, h);
  ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);   // 8-bit RGB
  chunk(f, "IHDR", ihdr); chunk(f, "IDAT", z); chunk(f, "IEND", {});
  return fclose(f) == 0;
}

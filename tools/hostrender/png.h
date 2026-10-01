// Tiny dependency-free PNG writer: 8-bit RGB, zlib stream with fixed-Huffman
// deflate + greedy LZ77. Good enough for test-card frames (flat areas compress
// ~30×); snow compresses poorly, as it should.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vector>

// rgb: w*h*3 bytes, row-major. Returns false if the file could not be written.
bool pngWrite(const char* path, const uint8_t* rgb, int w, int h);
// Raw deflate/zlib helpers, exposed for tests.
void zlibCompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out);

#include "renderer/dxbc_patch.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace kc::dxbc {
namespace {
uint32_t read(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
void append(std::vector<uint8_t>& out, uint32_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + 4);
}
void store(std::vector<uint8_t>& out, size_t at, uint32_t v) {
    std::memcpy(out.data() + at, &v, 4);
}
uint32_t rotate(uint32_t x, unsigned s) {
    return (x << s) | (x >> (32 - s));
}

// Standard MD5 compression with DXBC's length/padding layout. Algorithm reference:
// GPUOpen-Archive/common-src-ShaderUtils, DX10/DXBCChecksum.cpp (CalculateDXBCChecksum).
// This implementation is independent; no source from that implementation is copied.
void checksum(std::vector<uint8_t>& blob) {
    std::vector<uint8_t> input(blob.begin() + 20, blob.end());
    const auto bits = uint32_t(input.size() * 8);
    const size_t tail = input.size() % 64;
    const size_t full = input.size() - tail;
    if (tail >= 56) {
        input.push_back(0x80);
        input.resize(full + 64, 0);
        append(input, bits);
        input.resize(full + 124, 0);
    } else {
        input.insert(input.begin() + full, 4, 0);
        std::memcpy(input.data() + full, &bits, 4);
        input.push_back(0x80);
        input.resize(full + 60, 0);
    }
    append(input, (bits >> 2) | 1);
    constexpr unsigned shifts[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};
    constexpr uint32_t constants[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (size_t pos = 0; pos < input.size(); pos += 64) {
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        for (unsigned i = 0; i < 64; ++i) {
            uint32_t f, g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = 7 * i % 16;
            }
            uint32_t next =
                b + rotate(a + f + constants[i] + read(input.data() + pos + g * 4), shifts[i / 16][i % 4]);
            a = d;
            d = c;
            c = b;
            b = next;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
    }
    std::memcpy(blob.data() + 4, state, sizeof(state));
}
} // namespace

bool patch_contact_depth_uv(const void* code, size_t size, std::vector<uint8_t>& patched) {
    patched.clear();
    const auto* b = static_cast<const uint8_t*>(code);
    if (!b || size < 32 || size > 1024 * 1024 || std::memcmp(b, "DXBC", 4) || read(b + 24) != size)
        return false;
    constexpr uint8_t hashes[2][16] = {
        {0x5e, 0x3a, 0x6a, 0xd3, 0x50, 0x9e, 0x43, 0x3d, 0x1d, 0x3c, 0x8d, 0xb2, 0x83, 0x92, 0x2c, 0x29},
        {0xe1, 0x08, 0xb1, 0x8f, 0xda, 0xe5, 0x60, 0x7b, 0xc3, 0x14, 0x4b, 0x28, 0xe6, 0xd0, 0xc1, 0xf4}};
    if (std::memcmp(b + 4, hashes[0], 16) && std::memcmp(b + 4, hashes[1], 16))
        return false;
    // Fail closed on a damaged shader instead of trusting its advertised checksum.
    std::vector<uint8_t> verified(b, b + size);
    checksum(verified);
    if (std::memcmp(verified.data() + 4, b + 4, 16))
        return false;
    uint32_t count = read(b + 28);
    if (!count || count > 64 || 32 + count * 4 > size)
        return false;
    std::vector<std::vector<uint8_t>> chunks;
    unsigned samples = 0, programs = 0;
    constexpr uint32_t sample[] = {0x8d000048, 0x800000c2, 0x00155543, 0x00100012, 5,          0x00100086, 6,
                                   0x00107e46, 0,          0x00106000, 0,          0x00004001, 0};
    constexpr uint32_t mul[] = {0x08000038, 0x00100052, 6, 0x00100a06, 6, 0x00208506, 13, 0};
    for (uint32_t j = 0; j < count; ++j) {
        size_t off = read(b + 32 + j * 4);
        if (off < 32 + count * 4 || off > size - 8)
            return false;
        size_t len = read(b + off + 4);
        if (len > size - off - 8)
            return false;
        const uint8_t* data = b + off + 8;
        if (!std::memcmp(b + off, "RDEF", 4) || !std::memcmp(b + off, "STAT", 4))
            continue;
        std::vector<uint8_t> chunk(b + off, b + off + 8);
        if (!std::memcmp(b + off, "SHEX", 4) || !std::memcmp(b + off, "SHDR", 4)) {
            if (++programs != 1 || len < 8 || len % 4 || read(data + 4) != len / 4)
                return false;
            std::vector<uint8_t> words(data, data + 8);
            bool declared = false;
            for (size_t i = 2; i < len / 4;) {
                uint32_t token = read(data + i * 4), op = token & 2047, n = (token >> 24) & 127;
                if (!n || i + n > len / 4)
                    return false;
                if (!declared && op == 90) {
                    for (auto v : {0x04000059u, 0x00208e46u, 13u, 1u})
                        append(words, v);
                    declared = true;
                }
                if (op == 72) {
                    if (n != std::size(sample) || std::memcmp(data + i * 4, sample, sizeof(sample)))
                        return false;
                    for (auto v : mul)
                        append(words, v);
                    ++samples;
                }
                words.insert(words.end(), data + i * 4, data + (i + n) * 4);
                i += n;
            }
            if (!declared)
                return false;
            store(words, 4, uint32_t(words.size() / 4));
            chunk.insert(chunk.end(), words.begin(), words.end());
            store(chunk, 4, uint32_t(words.size()));
        } else
            chunk.insert(chunk.end(), data, data + len);
        chunks.push_back(std::move(chunk));
    }
    if (samples != 1 || programs != 1)
        return false;
    std::vector<uint8_t> out(b, b + 32);
    store(out, 28, uint32_t(chunks.size()));
    out.resize(32 + 4 * chunks.size());
    for (size_t i = 0; i < chunks.size(); ++i) {
        store(out, 32 + i * 4, uint32_t(out.size()));
        out.insert(out.end(), chunks[i].begin(), chunks[i].end());
    }
    store(out, 24, uint32_t(out.size()));
    checksum(out);
    patched = std::move(out);
    return true;
}

bool patch_forward_depth_origins(const void* code, size_t size, std::vector<uint8_t>& patched) {
    patched.clear();
    const auto* b = static_cast<const uint8_t*>(code);
    if (!b || size < 32 || size > 1024 * 1024 || std::memcmp(b, "DXBC", 4) || read(b + 24) != size)
        return false;
    constexpr uint8_t hashes[2][16] = {
        {0x7f,0x6e,0x90,0x33,0x83,0xed,0x45,0xdb,0x5f,0xe3,0xd9,0x76,0x91,0x47,0x10,0xa2},
        {0xc1,0x9c,0x94,0x27,0x24,0x50,0xfa,0xc6,0x64,0x2d,0x7a,0xbf,0xde,0x00,0xd1,0xb3}};
    const bool opaque = !std::memcmp(b + 4, hashes[0], 16);
    if (!opaque && std::memcmp(b + 4, hashes[1], 16)) return false;
    std::vector<uint8_t> verified(b, b + size); checksum(verified);
    if (std::memcmp(verified.data() + 4, b + 4, 16)) return false;
    const uint32_t count = read(b + 28);
    if (!count || count > 64 || 32 + count * 4 > size) return false;
    // Exact origin instructions. The other PS_ScreenSize multiplies belong to
    // projected ray steps or tile indexing and must retain their original values.
    constexpr uint32_t origin[3][8] = {
        {0x08000038,0x00100032,9,0x00100596,5,0x00208046,0,35},
        {0x08000038,0x00100032,19,0x00100596,12,0x00208046,0,35},
        {0x08000038,0x00100062,12,0x00100656,12,0x00208106,0,35}};
    std::vector<std::vector<uint8_t>> chunks;
    unsigned programs = 0, edits[3]{};
    for (uint32_t j = 0; j < count; ++j) {
        const size_t off = read(b + 32 + j * 4);
        if (off < 32 + count * 4 || off > size - 8) return false;
        const size_t len = read(b + off + 4);
        if (len > size - off - 8) return false;
        const auto* data = b + off + 8;
        if (!std::memcmp(b + off,"RDEF",4) || !std::memcmp(b + off,"STAT",4)) continue;
        std::vector<uint8_t> chunk(b + off,b + off + 8);
        if (!std::memcmp(b + off,"SHEX",4) || !std::memcmp(b + off,"SHDR",4)) {
            if (++programs != 1 || len < 8 || len % 4 || read(data + 4) != len / 4) return false;
            std::vector<uint8_t> words(data,data + 8);
            bool declared = false;
            for (size_t i = 2; i < len / 4;) {
                const uint32_t token = read(data+i*4), op = token & 2047, n = (token >> 24) & 127;
                if (!n || i+n > len/4) return false;
                if (op == 89 && n >= 4 && read(data+(i+2)*4) == 13) return false;
                if (!declared && op == 90) {
                    for (auto v : {0x04000059u,0x00208e46u,13u,1u}) append(words,v);
                    declared = true;
                }
                const size_t start = words.size();
                words.insert(words.end(),data+i*4,data+(i+n)*4);
                for (unsigned k = opaque ? 0 : 1; k < (opaque ? 1u : 3u); ++k)
                    if (n == 8 && !std::memcmp(data+i*4,origin[k],sizeof(origin[k]))) {
                        store(words,start+6*4,13); store(words,start+7*4,0); ++edits[k];
                    }
                i += n;
            }
            if (!declared) return false;
            store(words,4,uint32_t(words.size()/4));
            chunk.insert(chunk.end(),words.begin(),words.end());store(chunk,4,uint32_t(words.size()));
        } else chunk.insert(chunk.end(),data,data+len);
        chunks.push_back(std::move(chunk));
    }
    if (programs != 1 || (opaque ? (edits[0]!=1 || edits[1] || edits[2]) : (edits[0] || edits[1]!=1 || edits[2]!=1))) return false;
    std::vector<uint8_t> out(b,b+32);store(out,28,uint32_t(chunks.size()));out.resize(32+4*chunks.size());
    for (size_t i=0;i<chunks.size();++i) {store(out,32+i*4,uint32_t(out.size()));out.insert(out.end(),chunks[i].begin(),chunks[i].end());}
    store(out,24,uint32_t(out.size()));checksum(out);patched=std::move(out);return true;
}
} // namespace kc::dxbc

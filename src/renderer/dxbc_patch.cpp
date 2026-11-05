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

using Hash = uint8_t[16];

// A well-formed container whose checksum matches its content.
bool verified(const uint8_t* b, size_t size) {
    if (!b || size < 32 || size > 1024 * 1024 || std::memcmp(b, "DXBC", 4) || read(b + 24) != size)
        return false;
    std::vector<uint8_t> copy(b, b + size);
    checksum(copy);
    return !std::memcmp(copy.data() + 4, b + 4, 16);
}

// Returns the index of the known shader checksum `code` carries, or -1. Fails closed on a
// malformed container or a damaged shader instead of trusting its advertised checksum.
int match_shader(const uint8_t* b, size_t size, const Hash* hashes, size_t count) {
    if (!b || size < 32)
        return -1;
    int match = -1;
    for (size_t i = 0; i < count && match < 0; ++i)
        if (!std::memcmp(b + 4, hashes[i], 16))
            match = int(i);
    return match >= 0 && verified(b, size) ? match : -1;
}

// Rebuilds a container with exactly one shader program. RDEF and STAT are dropped,
// and (with declare_b13) a b13 constant buffer declaration is inserted before dcl_temps. `edit`
// receives every original instruction (op, word count) and appends its replacement to `words`;
// it returns false to reject the shader. The output checksum is recomputed.
template <class Edit>
bool rewrite_program(const uint8_t* b, size_t size, Edit&& edit, std::vector<uint8_t>& patched,
                     bool declare_b13 = true) {
    patched.clear();
    const uint32_t count = read(b + 28);
    if (!count || count > 64 || 32 + count * 4 > size)
        return false;
    std::vector<std::vector<uint8_t>> chunks;
    unsigned programs = 0;
    for (uint32_t j = 0; j < count; ++j) {
        const size_t off = read(b + 32 + j * 4);
        if (off < 32 + count * 4 || off > size - 8)
            return false;
        const size_t len = read(b + off + 4);
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
            bool declared = !declare_b13;
            for (size_t i = 2; i < len / 4;) {
                const uint32_t token = read(data + i * 4), op = token & 2047, n = (token >> 24) & 127;
                if (!n || i + n > len / 4)
                    return false;
                if (!declared && op == 90) {
                    for (auto v : {0x04000059u, 0x00208e46u, 13u, 1u})
                        append(words, v);
                    declared = true;
                }
                if (!edit(data + i * 4, op, n, words))
                    return false;
                i += n;
            }
            if (!declared)
                return false;
            store(words, 4, uint32_t(words.size() / 4));
            chunk.insert(chunk.end(), words.begin(), words.end());
            store(chunk, 4, uint32_t(words.size()));
        } else {
            chunk.insert(chunk.end(), data, data + len);
        }
        chunks.push_back(std::move(chunk));
    }
    if (programs != 1)
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
} // namespace

bool patch_contact_depth_uv(const void* code, size_t size, std::vector<uint8_t>& patched) {
    patched.clear();
    const auto* b = static_cast<const uint8_t*>(code);
    constexpr Hash hashes[] = {
        {0x5e, 0x3a, 0x6a, 0xd3, 0x50, 0x9e, 0x43, 0x3d, 0x1d, 0x3c, 0x8d, 0xb2, 0x83, 0x92, 0x2c, 0x29},
        {0xe1, 0x08, 0xb1, 0x8f, 0xda, 0xe5, 0x60, 0x7b, 0xc3, 0x14, 0x4b, 0x28, 0xe6, 0xd0, 0xc1, 0xf4}};
    if (match_shader(b, size, hashes, std::size(hashes)) < 0)
        return false;
    constexpr uint32_t sample[] = {0x8d000048, 0x800000c2, 0x00155543, 0x00100012, 5,          0x00100086, 6,
                                   0x00107e46, 0,          0x00106000, 0,          0x00004001, 0};
    constexpr uint32_t mul[] = {0x08000038, 0x00100052, 6, 0x00100a06, 6, 0x00208506, 13, 0};
    unsigned samples = 0;
    auto edit = [&](const uint8_t* instr, uint32_t op, uint32_t n, std::vector<uint8_t>& words) {
        if (op == 72) {
            if (n != std::size(sample) || std::memcmp(instr, sample, sizeof(sample)))
                return false;
            for (auto v : mul)
                append(words, v);
            ++samples;
        }
        words.insert(words.end(), instr, instr + n * 4);
        return true;
    };
    std::vector<uint8_t> out;
    if (!rewrite_program(b, size, edit, out) || samples != 1)
        return false;
    patched = std::move(out);
    return true;
}

bool patch_forward_depth_origins(const void* code, size_t size, std::vector<uint8_t>& patched) {
    patched.clear();
    const auto* b = static_cast<const uint8_t*>(code);
    constexpr Hash hashes[] = {
        {0x7f, 0x6e, 0x90, 0x33, 0x83, 0xed, 0x45, 0xdb, 0x5f, 0xe3, 0xd9, 0x76, 0x91, 0x47, 0x10, 0xa2},
        {0xc1, 0x9c, 0x94, 0x27, 0x24, 0x50, 0xfa, 0xc6, 0x64, 0x2d, 0x7a, 0xbf, 0xde, 0x00, 0xd1, 0xb3}};
    const int match = match_shader(b, size, hashes, std::size(hashes));
    if (match < 0)
        return false;
    const bool opaque = match == 0;
    // Exact origin instructions. The other PS_ScreenSize multiplies belong to
    // projected ray steps or tile indexing and must retain their original values.
    constexpr uint32_t origin[3][8] = {
        {0x08000038, 0x00100032, 9, 0x00100596, 5, 0x00208046, 0, 35},
        {0x08000038, 0x00100032, 19, 0x00100596, 12, 0x00208046, 0, 35},
        {0x08000038, 0x00100062, 12, 0x00100656, 12, 0x00208106, 0, 35}};
    // The opaque variant has origin 0; the transparent variant has origins 1 and 2.
    const unsigned first = opaque ? 0 : 1, last = opaque ? 1 : 3;
    unsigned edits[3]{};
    auto edit = [&](const uint8_t* instr, uint32_t op, uint32_t n, std::vector<uint8_t>& words) {
        // b13 must be free for the allocation dimensions.
        if (op == 89 && n >= 4 && read(instr + 2 * 4) == 13)
            return false;
        const size_t start = words.size();
        words.insert(words.end(), instr, instr + n * 4);
        for (unsigned k = first; k < last; ++k)
            if (n == 8 && !std::memcmp(instr, origin[k], sizeof(origin[k]))) {
                // Redirect the PS_ScreenSize operand (cb0[35]) to cb13[0].
                store(words, start + 6 * 4, 13);
                store(words, start + 7 * 4, 0);
                ++edits[k];
            }
        return true;
    };
    std::vector<uint8_t> out;
    if (!rewrite_program(b, size, edit, out))
        return false;
    const bool expected = opaque ? edits[0] == 1 && !edits[1] && !edits[2]
                                 : !edits[0] && edits[1] == 1 && edits[2] == 1;
    if (!expected)
        return false;
    patched = std::move(out);
    return true;
}

namespace {
// One operand of an instruction: register type, component selection and immediate indices.
// Only the forms the water patch has to recognise are accepted; anything else fails parsing.
struct Operand {
    uint32_t type = 0, mode = 0, bits = 0;  // bits: mask (mode 0), swizzle (1) or component (2)
    bool neg = false;
    uint32_t index[2] = {};
    size_t words = 0;
    uint32_t imm = 0;  // immediate32 with one component
};
enum : uint32_t { kTemp = 0, kImmediate = 4, kResource = 7, kConstantBuffer = 8 };
bool parse_operand(const uint8_t* p, size_t avail, Operand& o) {
    if (avail < 1)
        return false;
    const uint32_t t = read(p);
    o = {};
    o.mode = (t >> 2) & 3;
    o.bits = (t >> 4) & 255;
    o.type = (t >> 12) & 255;
    const uint32_t dims = (t >> 20) & 3;
    size_t at = 1;
    if (t >> 31) {
        // Only a plain negate modifier.
        if (avail < 2 || (read(p + 4) & 0x80003fffu) != 0x41u)
            return false;
        o.neg = true;
        at = 2;
    }
    if (o.type == kImmediate) {
        if ((t & 3) != 1 || dims || at + 1 > avail)
            return false;
        o.imm = read(p + at * 4);
        o.words = at + 1;
        return true;
    }
    if (dims > 2 || at + dims > avail)
        return false;
    for (uint32_t d = 0; d < dims; ++d) {
        if ((t >> (22 + 3 * d)) & 7)  // relative addressing
            return false;
        o.index[d] = read(p + (at + d) * 4);
    }
    o.words = at + dims;
    return true;
}
// An ALU or sample instruction with one destination (operand 0) and only plain operands:
// the instructions a backward search for a register's last writer may step over.
bool single_destination(uint32_t op) {
    switch (op) {
    case 0: case 1: case 11: case 12: case 14: case 15: case 16: case 17: case 24: case 25: case 26: case 27:
    case 28: case 29: case 30: case 43: case 47: case 49: case 50: case 51: case 52: case 54: case 55: case 56:
    case 57: case 59: case 60: case 64: case 65: case 66: case 67: case 68: case 69: case 70: case 71: case 72:
    case 73: case 74: case 75: case 86:
        return true;
    default:
        return false;
    }
}
struct Instr {
    const uint8_t* p;
    uint32_t op, n;
    // Operands start after the opcode token and any extended opcode tokens.
    size_t first_operand() const {
        size_t at = 1;
        for (uint32_t t = read(p); (t >> 31) && at < n; ++at)
            t = read(p + at * 4);
        return at;
    }
    bool operands(Operand* out, size_t count) const {
        size_t at = first_operand();
        for (size_t i = 0; i < count; ++i) {
            if (at >= n || !parse_operand(p + at * 4, n - at, out[i]))
                return false;
            at += out[i].words;
        }
        return true;
    }
};
} // namespace

// WaterPS reads its reflection at  uv + offset * mask,  where the offset's y also carries the
// shift into the bottom-left sub-rect, 1 - (1 - y) * PS_HPosScale.y, and mask (0 or 1) drops
// the offset where it would land on something in front of the water. A dropped read lands at
// the unshifted uv. The reflection sample is moved to a new temp register holding
//   x = coordinate.x,  y = coordinate.y + (1 - mask) * (1 - PS_HPosScale.y)
// so the dropped reads are shifted too. Original registers are left untouched.
bool patch_water_refl_fallback(const void* code, size_t size, unsigned refl_slot, unsigned hpos_cb,
                               unsigned hpos_vec, std::vector<uint8_t>& patched) {
    patched.clear();
    const auto* b = static_cast<const uint8_t*>(code);
    if (!verified(b, size))
        return false;
    // The program's instructions.
    std::vector<Instr> code_;
    const uint32_t count = read(b + 28);
    if (!count || count > 64 || 32 + count * 4 > size)
        return false;
    for (uint32_t j = 0; j < count; ++j) {
        const size_t off = read(b + 32 + j * 4);
        if (off < 32 + count * 4 || off > size - 8)
            return false;
        const size_t len = read(b + off + 4);
        if (len > size - off - 8 || (std::memcmp(b + off, "SHEX", 4) && std::memcmp(b + off, "SHDR", 4)))
            continue;
        if (!code_.empty() || len < 8 || len % 4)
            return false;
        const uint8_t* data = b + off + 8;
        for (size_t i = 2; i < len / 4;) {
            const uint32_t token = read(data + i * 4), op = token & 2047, n = (token >> 24) & 127;
            if (!n || i + n > len / 4)
                return false;
            code_.push_back({data + i * 4, op, n});
            i += n;
        }
    }
    // dcl_temps: the new register's index.
    size_t temps = code_.size();
    for (size_t i = 0; i < code_.size(); ++i)
        if (code_[i].op == 104) {
            if (temps != code_.size() || code_[i].n != 2)
                return false;
            temps = i;
        }
    if (temps == code_.size())
        return false;
    const uint32_t reg = read(code_[temps].p + 4);
    // The first (and only) sample of the reflection texture.
    size_t s = code_.size();
    Operand so[3];
    for (size_t i = 0; i < code_.size(); ++i) {
        if (code_[i].op < 69 || code_[i].op > 74 || !code_[i].operands(so, 3) || so[2].type != kResource ||
            so[2].index[0] != refl_slot)
            continue;
        if (s != code_.size())
            return false;
        s = i;
    }
    if (s == code_.size() || s == 0 || !code_[s].operands(so, 3))
        return false;
    const Operand& coord = so[1];
    if (coord.type != kTemp || coord.mode != 1 || coord.neg || coord.words != 2)
        return false;
    const uint32_t cx = coord.bits & 3, cy = (coord.bits >> 2) & 3;
    // The index of the last writer of temp r component c before `before`, stepping over
    // single-destination instructions only; npos if anything else is in the way.
    constexpr size_t npos = size_t(-1);
    auto last_writer = [&](uint32_t r, uint32_t c, size_t before) -> size_t {
        for (size_t i = before; i-- > temps + 1;) {
            Operand dst;
            if (!single_destination(code_[i].op) || !code_[i].operands(&dst, 1))
                return npos;
            if (dst.type == kTemp && dst.mode == 0 && dst.index[0] == r && (dst.bits & (1u << c)))
                return i;
        }
        return npos;
    };
    // The coordinate: mad D, A, M.mmmm, uv.
    const size_t w = last_writer(coord.index[0], cy, s);
    if (w == npos || last_writer(coord.index[0], cx, s) != w)
        return false;
    Operand wo[4];
    if (code_[w].op != 50 || (read(code_[w].p) & 0x80002000u) || !code_[w].operands(wo, 4))
        return false;
    const Operand &d = wo[0], &a = wo[1], &m = wo[2];
    if (d.mode != 0 || a.type != kTemp || a.mode != 1 || a.neg || m.type != kTemp || m.mode != 1 || m.neg ||
        wo[3].type != kTemp || wo[3].neg)
        return false;
    const uint32_t mc = m.bits & 3, ay = (a.bits >> (2 * cy)) & 3;
    if (m.bits != mc * 0x55u)
        return false;
    // A.ay = 1 - B * PS_HPosScale.y: the shift is inside the masked term.
    const size_t shift = last_writer(a.index[0], ay, w);
    Operand ho[4];
    if (shift == npos || code_[shift].op != 50 || (read(code_[shift].p) & 0x80002000u) ||
        !code_[shift].operands(ho, 4) || ho[0].bits != (1u << ay) || ho[1].type != kTemp || !ho[1].neg ||
        ho[2].type != kConstantBuffer || ho[2].mode != 2 || ho[2].bits != 1 || ho[2].neg ||
        ho[2].index[0] != hpos_cb || ho[2].index[1] != hpos_vec || ho[3].type != kImmediate ||
        ho[3].imm != 0x3f800000u)
        return false;
    // M.mc = (...) & 1.0: 0 or 1.
    const size_t mask = last_writer(m.index[0], mc, w);
    Operand mo[3];
    if (mask == npos || code_[mask].op != 1 || (read(code_[mask].p) & 0x80002000u) ||
        !code_[mask].operands(mo, 3) || mo[0].bits != (1u << mc) || mo[2].type != kImmediate ||
        mo[2].imm != 0x3f800000u)
        return false;

    const uint32_t D = coord.index[0], M = m.index[0];
    // Before the mad (it may overwrite the mask):
    //   add rN.x, -M.mc, l(1.0)          1 - mask
    const uint32_t before_mad[] = {0x08000000, 0x00100012, reg, 0x8010000au | (mc << 4), 0x41, M, 0x00004001,
                                   0x3f800000};
    // Before the sample:
    //   add rN.y, -cb[hpos].y, l(1.0)    1 - PS_HPosScale.y
    //   mad rN.y, rN.x, rN.y, D.cy       coordinate.y + (1 - mask) * (1 - PS_HPosScale.y)
    //   mov rN.x, D.cx
    const uint32_t before_sample[] = {
        0x09000000, 0x00100022, reg, 0x8020801au, 0x41, hpos_cb, hpos_vec, 0x00004001, 0x3f800000,
        0x09000032, 0x00100022, reg, 0x0010000a, reg, 0x0010001a, reg, 0x0010000au | (cy << 4), D,
        0x05000036, 0x00100012, reg, 0x0010000au | (cx << 4), D};
    const size_t coord_at = so[0].words + code_[s].first_operand();
    size_t index = 0;
    auto edit = [&](const uint8_t* instr, uint32_t, uint32_t n, std::vector<uint8_t>& words) {
        const size_t i = index++;
        if (i == w)
            for (auto v : before_mad)
                append(words, v);
        if (i == s)
            for (auto v : before_sample)
                append(words, v);
        const size_t at = words.size();
        words.insert(words.end(), instr, instr + n * 4);
        if (i == temps)
            store(words, at + 4, reg + 1);
        if (i == s) {
            // The coordinate operand: rN.xyxx.
            store(words, at + coord_at * 4, (read(instr + coord_at * 4) & ~0xff0u) | (0x04u << 4));
            store(words, at + (coord_at + 1) * 4, reg);
        }
        return true;
    };
    std::vector<uint8_t> out;
    if (!rewrite_program(b, size, edit, out, false) || index != code_.size())
        return false;
    patched = std::move(out);
    return true;
}
} // namespace kc::dxbc

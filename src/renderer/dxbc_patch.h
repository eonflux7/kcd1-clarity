#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kc::dxbc {
// Exact shipping local-light shaders only. Adds b13.xy to the contact depth UV,
// after the original frustum test; all other executable tokens stay unchanged.
bool patch_contact_depth_uv(const void* code, size_t size, std::vector<uint8_t>& patched);
// Exact shipping fur shaders: use allocation dimensions only for depth origins derived
// from already-scaled screen UVs. Projected ray steps keep viewport dimensions.
bool patch_forward_depth_origins(const void* code, size_t size, std::vector<uint8_t>& patched);
} // namespace kc::dxbc

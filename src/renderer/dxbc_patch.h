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
// Water surface shaders (WaterPS, any permutation): the reflection reads whose wave offset is
// masked out also get the shift into the bottom-left sub-rect. refl_slot is the ReflSampler
// texture register, PS_HPosScale is cb<hpos_cb>[hpos_vec]. Fails closed on any other shape.
bool patch_water_refl_fallback(const void* code, size_t size, unsigned refl_slot, unsigned hpos_cb,
                               unsigned hpos_vec, std::vector<uint8_t>& patched);
} // namespace kc::dxbc

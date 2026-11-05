#pragma once
#include <d3d11.h>

#include <cstdint>

#include "renderer/resource_slots.h"
#include "renderer/subrect_fixes.h"

// Everything a shader bind needs, computed once at shader creation and stored on the shader object,
// so binding costs one GetPrivateData call and no lock or map lookup.
namespace kc::shader_tag {
struct Tag {
    uint32_t slots = resource_slots::all;              // SRV slots the bytecode declares
    subrect_fixes::Kind kind = subrect_fixes::Kind::None;  // VS: Bloom marks the Gaussian bloom VS
    ID3D11PixelShader* replacement = nullptr;          // owned by subrect_fixes for the process lifetime
};
inline constexpr GUID key{0xe217ca02, 0xacaf, 0x4e4d, {0xa2, 0xc3, 0x28, 0xc6, 0x4a, 0x8e, 0x13, 0x92}};

inline void set(ID3D11DeviceChild* shader, const Tag& tag) {
    shader->SetPrivateData(key, sizeof(tag), &tag);
}
// Untagged shaders (created before the hooks, or by us) keep the conservative defaults.
// Shaders bound with class instances may link extra resources, so all slots stay eligible.
inline Tag get(ID3D11DeviceChild* shader, UINT classes) {
    Tag tag;
    if (!shader) {
        tag.slots = 0;
        return tag;
    }
    UINT size = sizeof(tag);
    if (FAILED(shader->GetPrivateData(key, &size, &tag)) || size != sizeof(tag))
        tag = Tag{};
    if (classes)
        tag.slots = resource_slots::all;
    return tag;
}
}  // namespace kc::shader_tag

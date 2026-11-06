#include "core/config.h"

#include <cstdio>
#include <cwchar>
#include <string>

#include "core/log.h"

namespace kc {

static Config g_config;
static std::wstring g_path;

Config& config() { return g_config; }

float render_scale_for(int quality) {
    static const float kScale[] = {1.0f, 2.0f / 3.0f, 0.58f, 0.5f, 1.0f / 3.0f};
    return kScale[quality < 0 ? 0 : quality > 4 ? 4 : quality];
}

void config_load(const std::wstring& path) {
    g_path = path;
    const wchar_t* f = g_path.c_str();
    g_config.mode = GetPrivateProfileIntW(L"general", L"mode", g_config.mode, f) ? 1 : 0;
    g_config.toggle_key = GetPrivateProfileIntW(L"ui", L"toggle_key", g_config.toggle_key, f);
    g_config.debug_key = GetPrivateProfileIntW(L"ui", L"debug_key", g_config.debug_key, f);
    g_config.show_on_start = GetPrivateProfileIntW(L"ui", L"show_on_start", g_config.show_on_start, f) != 0;
    g_config.show_fps = GetPrivateProfileIntW(L"ui", L"show_fps", g_config.show_fps, f) != 0;
    g_config.dlss_quality = GetPrivateProfileIntW(L"dlss", L"quality", g_config.dlss_quality, f);
    g_config.dlss_preset = GetPrivateProfileIntW(L"dlss", L"preset", g_config.dlss_preset, f);
    g_config.mip_bias = GetPrivateProfileIntW(L"dlss", L"mip_bias", g_config.mip_bias, f) != 0;
    wchar_t buf[32];
    GetPrivateProfileStringW(L"dlss", L"mip_bias_offset", L"", buf, 32, f);
    if (buf[0])
        g_config.mip_bias_offset = wcstof(buf, nullptr);
    g_config.velocity16 = GetPrivateProfileIntW(L"dlss", L"velocity16", g_config.velocity16, f) != 0;
    g_config.coarse_shadow_noise = GetPrivateProfileIntW(L"dlss", L"coarse_shadow_noise", g_config.coarse_shadow_noise, f) != 0;
    g_config.temporal_shadow_noise =
        GetPrivateProfileIntW(L"dlss", L"temporal_shadow_noise", g_config.temporal_shadow_noise, f) != 0;
    g_config.edge_clamp = GetPrivateProfileIntW(L"dlss", L"edge_clamp", g_config.edge_clamp, f) != 0;
    g_config.ui_depth_alignment = GetPrivateProfileIntW(L"dlss", L"ui_depth_alignment", g_config.ui_depth_alignment, f) != 0;
    g_config.d3d_debug = GetPrivateProfileIntW(L"debug", L"d3d_debug", g_config.d3d_debug, f) != 0;
    g_config.skip_ngx = GetPrivateProfileIntW(L"debug", L"skip_ngx", g_config.skip_ngx, f) != 0;
    g_config.engine_after = GetPrivateProfileIntW(L"debug", L"engine_after", g_config.engine_after, f) != 0;
    g_config.force_halton = GetPrivateProfileIntW(L"debug", L"force_halton", g_config.force_halton, f) != 0;
    g_config.state_swap = GetPrivateProfileIntW(L"debug", L"state_swap", g_config.state_swap, f) != 0;
    logf("config: %ls: mode %d, skip_ngx %d, engine_after %d, force_halton %d, state_swap %d, quality %d", f, g_config.mode,
         g_config.skip_ngx, g_config.engine_after, g_config.force_halton, g_config.state_swap, g_config.dlss_quality);
    config_save();
}

void config_save() {
    if (g_path.empty())
        return;
    const wchar_t* f = g_path.c_str();
    WritePrivateProfileStringW(L"general", L"mode", std::to_wstring(g_config.mode).c_str(), f);
    WritePrivateProfileStringW(L"ui", L"toggle_key", std::to_wstring(g_config.toggle_key).c_str(), f);
    WritePrivateProfileStringW(L"ui", L"debug_key", std::to_wstring(g_config.debug_key).c_str(), f);
    WritePrivateProfileStringW(L"ui", L"show_on_start", g_config.show_on_start ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"ui", L"show_fps", g_config.show_fps ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"dlss", L"quality", std::to_wstring(g_config.dlss_quality).c_str(), f);
    WritePrivateProfileStringW(L"dlss", L"preset", std::to_wstring(g_config.dlss_preset).c_str(), f);
    WritePrivateProfileStringW(L"dlss", L"mip_bias", g_config.mip_bias ? L"1" : L"0", f);
    wchar_t buf[32];
    swprintf(buf, 32, L"%.2f", g_config.mip_bias_offset);
    WritePrivateProfileStringW(L"dlss", L"mip_bias_offset", buf, f);
    WritePrivateProfileStringW(L"dlss", L"velocity16", g_config.velocity16 ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"dlss", L"coarse_shadow_noise", g_config.coarse_shadow_noise ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"dlss", L"temporal_shadow_noise", g_config.temporal_shadow_noise ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"dlss", L"edge_clamp", g_config.edge_clamp ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"dlss", L"ui_depth_alignment", g_config.ui_depth_alignment ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"debug", L"d3d_debug", g_config.d3d_debug ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"debug", L"skip_ngx", g_config.skip_ngx ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"debug", L"engine_after", g_config.engine_after ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"debug", L"force_halton", g_config.force_halton ? L"1" : L"0", f);
    WritePrivateProfileStringW(L"debug", L"state_swap", g_config.state_swap ? L"1" : L"0", f);
}

}  // namespace kc

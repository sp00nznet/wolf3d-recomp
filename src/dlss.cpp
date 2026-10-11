/*
 * dlss.cpp - the 3D view through NVIDIA DLSS (optional; see docs/dlss.md).
 *
 * Built only when the DLSS SDK is in external/dlss-sdk (tools/get_dlss.py):
 * the SDK is NVIDIA's, under the NVIDIA RTX SDKs licence, and is never part
 * of this repository. This file is this project's own code (MIT) calling it.
 *
 * hires.c renders the view at DLSS's render resolution with a sub-pixel jitter,
 * plus depth, motion vectors and a mask of pixels whose motion it can't know
 * (sprites); this uploads them to a Direct3D 11 device of its own, runs DLSS
 * Super Resolution (or DLAA, at the output resolution), and reads the result
 * back. Its own device, no swap chain: the same path works in a window and
 * headless, and the presenter stays as it is. C++ only because the NGX
 * headers use default arguments; the interface is C.
 */
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static NVSDK_NGX_Parameter *params;
static NVSDK_NGX_Handle *feature;
static int state;                       /* 0 not tried, 1 ready, -1 unavailable */
static int f_rw, f_rh, f_ow, f_oh, f_mode = -1;
static ID3D11Texture2D *t_color, *t_depth, *t_mv, *t_bias, *t_out, *t_read;

/* Modes as --dlss names them: dlaa renders at the output size (anti-aliasing
 * only); the others render smaller and upscale. */
static const NVSDK_NGX_PerfQuality_Value k_quality[] = {
    NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality,
    NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
    NVSDK_NGX_PerfQuality_Value_UltraPerformance };

static void release(IUnknown **p) { if (*p) { (*p)->Release(); *p = NULL; } }
#define REL(p) release((IUnknown **)&(p))

extern "C" int dlss_available(void)
{
    if (state) return state > 0;
    state = -1;
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl, 2, D3D11_SDK_VERSION,
                                 &dev, NULL, &ctx))) {
        fprintf(stderr, "[dlss] no Direct3D 11 device\n");
        return 0;
    }
    wchar_t logs[MAX_PATH];
    GetTempPathW(MAX_PATH, logs);
    wcscat_s(logs, L"wolf3d-ngx");
    CreateDirectoryW(logs, NULL);
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID("6f1c3e5a-2b7d-4e8f-9a01-57f3d0c2b9e4",
        NVSDK_NGX_ENGINE_TYPE_CUSTOM, "0.1", logs, dev);
    if (NVSDK_NGX_FAILED(r)) {
        fprintf(stderr, "[dlss] NGX init failed (0x%08X): not an RTX GPU, or the driver is too old\n", (unsigned)r);
        return 0;
    }
    int ok = 0;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_GetCapabilityParameters(&params)) ||
        NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_Available, &ok)) || !ok) {
        fprintf(stderr, "[dlss] DLSS Super Resolution is not available on this GPU/driver\n");
        return 0;
    }
    state = 1;
    fprintf(stderr, "[dlss] DLSS ready\n");
    return 1;
}

/* The render size DLSS wants for an output of ow x oh in this mode. */
extern "C" int dlss_render_size(int ow, int oh, int mode, int *rw, int *rh)
{
    if (!dlss_available()) return 0;
    if (mode == 0) { *rw = ow; *rh = oh; return 1; }
    unsigned w = 0, h = 0, a, b, c, d;
    float sharp;
    if (NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(params, ow, oh, k_quality[mode], &w, &h, &a, &b, &c, &d, &sharp))
        || !w || !h) return 0;
    *rw = (int)w; *rh = (int)h;
    return 1;
}

static ID3D11Texture2D *tex(int w, int h, DXGI_FORMAT f, UINT bind, D3D11_USAGE use = D3D11_USAGE_DEFAULT, UINT cpu = 0)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = f;
    td.SampleDesc.Count = 1; td.Usage = use; td.BindFlags = bind; td.CPUAccessFlags = cpu;
    ID3D11Texture2D *t = NULL;
    dev->CreateTexture2D(&td, NULL, &t);
    return t;
}

static int make_feature(int rw, int rh, int ow, int oh, int mode)
{
    if (feature) { NVSDK_NGX_D3D11_ReleaseFeature(feature); feature = NULL; }
    REL(t_color); REL(t_depth); REL(t_mv); REL(t_bias); REL(t_out); REL(t_read);
    t_color = tex(rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    t_depth = tex(rw, rh, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    t_mv    = tex(rw, rh, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE);
    t_bias  = tex(rw, rh, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    t_out   = tex(ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    t_read  = tex(ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    if (!t_color || !t_depth || !t_mv || !t_bias || !t_out || !t_read) return 0;
    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth = rw; cp.Feature.InHeight = rh;
    cp.Feature.InTargetWidth = ow; cp.Feature.InTargetHeight = oh;
    cp.Feature.InPerfQualityValue = k_quality[mode];
    /* motion vectors at the render size; depth is 1 near, 0 far; the view is
     * plain LDR palette colours, so let DLSS find its own exposure */
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted
                            | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &feature, params, &cp);
    if (NVSDK_NGX_FAILED(r)) { fprintf(stderr, "[dlss] create failed (0x%08X)\n", (unsigned)r); return 0; }
    f_rw = rw; f_rh = rh; f_ow = ow; f_oh = oh; f_mode = mode;
    static const char *names[] = { "DLAA", "quality", "balanced", "performance", "ultra performance" };
    fprintf(stderr, "[dlss] %s: %dx%d -> %dx%d\n", names[mode], rw, rh, ow, oh);
    return 1;
}

static uint16_t half(float f)
{
    uint32_t x; memcpy(&x, &f, 4);
    uint32_t s = (x >> 16) & 0x8000, e = (x >> 23) & 0xFF, m = x & 0x7FFFFF;
    if (e < 113) return (uint16_t)s;                         /* tiny: zero */
    if (e > 142) return (uint16_t)(s | 0x7BFF);              /* huge: clamp */
    return (uint16_t)(s | (e - 112) << 10 | (m + 0x1000) >> 13);
}

/* color: 0xAARRGGBB, rw x rh; depth: 1 near .. 0 far; mv: x,y pairs in render
 * pixels, current -> previous; bias: 255 where the current frame should win.
 * out: 0xAARRGGBB, ow x oh. Returns 0 on any failure (the caller keeps its
 * own frame). */
extern "C" int dlss_eval(const uint32_t *color, const float *depth, const float *mv, const uint8_t *bias,
                         int rw, int rh, float jx, float jy, int reset, uint32_t *out, int ow, int oh, int mode)
{
    if (!dlss_available()) return 0;
    if ((rw != f_rw || rh != f_rh || ow != f_ow || oh != f_oh || mode != f_mode) && !make_feature(rw, rh, ow, oh, mode)) {
        state = -1;                       /* don't retry every frame */
        return 0;
    }
    static std::vector<uint32_t> rgba;
    static std::vector<uint16_t> mvh;
    rgba.resize((size_t)rw * rh); mvh.resize((size_t)rw * rh * 2);
    for (size_t i = 0; i < rgba.size(); i++) {               /* ARGB words -> RGBA bytes */
        uint32_t p = color[i];
        rgba[i] = 0xFF000000u | (p & 0xFF) << 16 | (p & 0xFF00) | (p >> 16 & 0xFF);
    }
    for (size_t i = 0; i < mvh.size(); i++) mvh[i] = half(mv[i]);
    ctx->UpdateSubresource(t_color, 0, NULL, rgba.data(), rw * 4, 0);
    ctx->UpdateSubresource(t_depth, 0, NULL, depth, rw * 4, 0);
    ctx->UpdateSubresource(t_mv, 0, NULL, mvh.data(), rw * 4, 0);
    ctx->UpdateSubresource(t_bias, 0, NULL, bias, rw, 0);

    NVSDK_NGX_D3D11_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = t_color;
    ep.Feature.pInOutput = t_out;
    ep.pInDepth = t_depth;
    ep.pInMotionVectors = t_mv;
    ep.pInBiasCurrentColorMask = t_bias;
    ep.InJitterOffsetX = jx;
    ep.InJitterOffsetY = jy;
    ep.InRenderSubrectDimensions.Width = rw;
    ep.InRenderSubrectDimensions.Height = rh;
    ep.InReset = reset;
    ep.InMVScaleX = ep.InMVScaleY = 1.0f;
    NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, feature, params, &ep);
    if (NVSDK_NGX_FAILED(r)) {
        static int n;
        if (n++ < 3) fprintf(stderr, "[dlss] evaluate failed (0x%08X)\n", (unsigned)r);
        return 0;
    }
    ctx->CopyResource(t_read, t_out);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(t_read, 0, D3D11_MAP_READ, 0, &m))) return 0;
    for (int y = 0; y < oh; y++) {
        const uint32_t *s = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
        uint32_t *o = out + (size_t)y * ow;
        for (int x = 0; x < ow; x++) {
            uint32_t p = s[x];
            o[x] = 0xFF000000u | (p & 0xFF) << 16 | (p & 0xFF00) | (p >> 16 & 0xFF);
        }
    }
    ctx->Unmap(t_read, 0);
    return 1;
}

extern "C" void dlss_shutdown(void)
{
    if (feature) NVSDK_NGX_D3D11_ReleaseFeature(feature);
    if (state > 0) NVSDK_NGX_D3D11_Shutdown1(dev);
    feature = NULL; state = 0;
}

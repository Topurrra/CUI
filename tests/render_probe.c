/* Rendering probe for Windows: measures what a window costs with OpenGL (CUI's own GL renderer),
 * with Direct3D 11 (a minimal prototype, not a renderer), or with plain GDI. Not a test and not part
 * of the default build (cmake -DCUI_BUILD_RENDER_PROBE=ON, target cui_render_probe).
 *
 *   cui_render_probe --mode gl|d3d|gdi [--load] [--frames N] [--idle MS] [--buffers 2|3]
 *                    [--latency 1|2] [--pace flush] [--blob FILE] [--size WxH]
 *
 * Bare (no --load): the window, the device or context and the swap chain, one cleared frame per
 * refresh. With --load: the vertex and index load of ed's scrolling frame (12 800 vertices of
 * cui_vertex, 12 draw calls with scissor, a 1024 x 1024 R8 atlas, CUI's own fragment shader),
 * uploaded and drawn every frame. gl uses cui_gl_render; d3d uses the HLSL port below.
 * Prints one line: time to the first presented frame, memory idle and after the run, per-frame
 * CPU time (without the wait for the display), process CPU per second, DWM frame counters.
 * --blob FILE: d3d writes the compiled shaders to FILE (first run) or reads them (later runs), so
 * d3dcompiler is not loaded, as with bytecode embedded in the exe. */
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "cui/cui.h"
#include "cui/cui_render.h"

static LARGE_INTEGER rp_freq;
static double rp_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart * 1000.0 / (double)rp_freq.QuadPart; }

enum { RP_W = 1200, RP_H = 800, RP_QUADS = 3200, RP_CMDS = 12, RP_ATLAS = 1024 };

/* ---------------------------------------------------------------- the load */
static cui_vertex *rp_verts;
static uint32_t *rp_idx;
static cui_draw_cmd rp_cmds[RP_CMDS];
static int rp_nv, rp_ni;
static unsigned char *rp_atlas;
static unsigned rp_seed = 12345;
static unsigned rp_rand(void) { rp_seed = rp_seed * 1664525u + 1013904223u; return rp_seed >> 8; }

static void rp_make_load(int w, int h) {
    rp_nv = RP_QUADS * 4; rp_ni = RP_QUADS * 6;
    rp_verts = calloc((size_t)rp_nv, sizeof *rp_verts);
    rp_idx = malloc(sizeof(uint32_t) * (size_t)rp_ni);
    rp_atlas = malloc((size_t)RP_ATLAS * RP_ATLAS);
    for (int i = 0; i < RP_ATLAS * RP_ATLAS; i++) rp_atlas[i] = (unsigned char)(rp_rand() & 255);
    for (int q = 0; q < RP_QUADS; q++) {
        float x = (float)(rp_rand() % (unsigned)(w - 16)), y = (float)(rp_rand() % (unsigned)(h - 20));
        float u = (float)(rp_rand() % (RP_ATLAS - 16)) / RP_ATLAS, v = (float)(rp_rand() % (RP_ATLAS - 16)) / RP_ATLAS;
        float qw = 8, qh = 16;
        int rr = (q % 40) == 0;   /* a few rounded rects among the glyphs */
        for (int k = 0; k < 4; k++) {
            cui_vertex *p = &rp_verts[q * 4 + k];
            int dx = k & 1, dy = k >> 1;
            p->x = x + dx * qw; p->y = y + dy * qh;
            p->u = u + dx * 8.0f / RP_ATLAS; p->v = v + dy * 16.0f / RP_ATLAS;
            p->lx = (dx - 0.5f) * qw; p->ly = (dy - 0.5f) * qh; p->hw = qw / 2; p->hh = qh / 2;
            p->r_tl = p->r_tr = p->r_br = p->r_bl = 3; p->border = 1; p->mode = rr ? 1.0f : 0.0f;
            p->color.r = 220; p->color.g = 220; p->color.b = 230; p->color.a = 255;
            p->border_color.r = 80; p->border_color.g = 80; p->border_color.b = 200; p->border_color.a = 255;
        }
        uint32_t b = (uint32_t)q * 4;
        uint32_t t[6] = { b, b + 1, b + 2, b + 1, b + 3, b + 2 };
        memcpy(rp_idx + q * 6, t, sizeof t);
    }
    int per = rp_ni / RP_CMDS;
    for (int c = 0; c < RP_CMDS; c++) {
        rp_cmds[c].clip.x = (float)(c * 7); rp_cmds[c].clip.y = (float)(c * 5);
        rp_cmds[c].clip.w = (float)(w - c * 14); rp_cmds[c].clip.h = (float)(h - c * 10);
        rp_cmds[c].texture = 0;
        rp_cmds[c].index_offset = (uint32_t)(c * per);
        rp_cmds[c].index_count = (uint32_t)(c == RP_CMDS - 1 ? rp_ni - c * per : per);
    }
}

/* ------------------------------------------------------------ measurements */
static double rp_cpu_ms(void) {
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return (double)(((ULARGE_INTEGER){ .u = { k.dwLowDateTime, k.dwHighDateTime } }).QuadPart + ((ULARGE_INTEGER){ .u = { u.dwLowDateTime, u.dwHighDateTime } }).QuadPart) / 10000.0;
}
static void rp_mem(double *ws, double *priv) {
    PROCESS_MEMORY_COUNTERS_EX m;
    memset(&m, 0, sizeof m);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof m);
    *ws = (double)m.WorkingSetSize / 1048576.0;
    *priv = (double)m.PrivateUsage / 1048576.0;
}
static int rp_cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

typedef HRESULT (WINAPI *PFN_timing)(HWND, DWM_TIMING_INFO *);
typedef HRESULT (WINAPI *PFN_dwmflush)(void);
static PFN_timing rp_timing;
static PFN_dwmflush rp_dwmflush;

/* ----------------------------------------------------------------- window */
static LRESULT CALLBACK rp_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}
static HWND rp_window(int w, int h, bool owndc) {
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.style = owndc ? CS_OWNDC : 0; wc.lpfnWndProc = rp_proc;
    wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"CUIRenderProbe";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassExW(&wc);
    RECT r = { 0, 0, w, h };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    return CreateWindowExW(0, wc.lpszClassName, L"cui render probe", WS_OVERLAPPEDWINDOW, 100, 100, r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
}
static void rp_pump(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

/* -------------------------------------------------------------------- GL */
typedef HGLRC (WINAPI *PFN_wglCCA)(HDC, HGLRC, const int *);
typedef BOOL (WINAPI *PFN_wglSI)(int);
static HMODULE rp_gl32;
static void *rp_glproc(const char *n) {
    void *p = (void *)wglGetProcAddress(n);
    if (!p || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) p = (void *)GetProcAddress(rp_gl32, n);
    return p;
}
static HGLRC rp_gl_context(HDC dc) {
    PIXELFORMATDESCRIPTOR pfd;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd; pfd.nVersion = 1; pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cAlphaBits = 8;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) return NULL;
    HGLRC legacy = wglCreateContext(dc);
    wglMakeCurrent(dc, legacy);
    PFN_wglCCA create = (PFN_wglCCA)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
    int attrs[] = { 0x2091, 3, 0x2092, 3, 0x9126, 1, 0 };
    HGLRC ctx = create ? create(dc, NULL, attrs) : NULL;
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(legacy);
    if (!ctx || !wglMakeCurrent(dc, ctx)) return NULL;
    return ctx;
}

/* ------------------------------------------------------------------ D3D11 */
static const char rp_hlsl[] =
    "cbuffer cb : register(b0) { float2 u_viewport; float2 pad; };\n"
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float2 loc : TEXCOORD1; float2 hlf : TEXCOORD2;\n"
    "  float4 radii : TEXCOORD3; float2 prm : TEXCOORD4; float4 color : TEXCOORD5; float4 bcolor : TEXCOORD6; };\n"
    "struct PSIn { float4 sv : SV_Position; float2 uv : TEXCOORD0; float2 loc : TEXCOORD1; nointerpolation float2 hlf : TEXCOORD2;\n"
    "  nointerpolation float4 radii : TEXCOORD3; nointerpolation float2 prm : TEXCOORD4; float4 color : TEXCOORD5;\n"
    "  nointerpolation float4 bcolor : TEXCOORD6; };\n"
    "PSIn vs_main(VSIn i) {\n"
    "  PSIn o;\n"
    "  o.sv = float4(i.pos.x / u_viewport.x * 2.0 - 1.0, 1.0 - i.pos.y / u_viewport.y * 2.0, 0.0, 1.0);\n"
    "  o.uv = i.uv; o.loc = i.loc; o.hlf = i.hlf; o.radii = i.radii; o.prm = i.prm; o.color = i.color; o.bcolor = i.bcolor;\n"
    "  return o;\n"
    "}\n"
    "float sd_rrect(float2 p, float2 b, float4 r) {\n"
    "  float rr = p.x < 0.0 ? (p.y < 0.0 ? r.x : r.w) : (p.y < 0.0 ? r.y : r.z);\n"
    "  rr = min(rr, min(b.x, b.y));\n"
    "  float2 q = abs(p) - b + rr;\n"
    "  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - rr;\n"
    "}\n"
    "float erf_(float x) {\n"
    "  float s = sign(x); x = abs(x);\n"
    "  float t = 1.0 / (1.0 + 0.3275911 * x);\n"
    "  float y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-x * x);\n"
    "  return s * y;\n"
    "}\n"
    "float bayer8(float2 p) {\n"
    "  float2 f = floor(p);\n"
    "  int2 q = int2(f - 8.0 * floor(f / 8.0));\n"
    "  int a = q.x ^ q.y, y = q.y;\n"
    "  int m = ((a & 1) << 5) | ((y & 1) << 4) | ((a & 2) << 2) | ((y & 2) << 1) | ((a & 4) >> 1) | ((y & 4) >> 2);\n"
    "  return (float(m) + 0.5) / 64.0;\n"
    "}\n"
    "float4 ps_main(PSIn i) : SV_Target {\n"
    "  int mode = int(i.prm.y + 0.5);\n"
    "  float4 c = i.color;\n"
    "  float4 t = tex.SampleLevel(smp, i.uv, 0);\n"
    "  if (mode == 4) return float4(c.rgb * c.a, c.a);\n"
    "  if (mode == 0) { float a = c.a * t.r; return float4(c.rgb * a, a); }\n"
    "  float d = sd_rrect(i.loc, i.hlf, i.radii);\n"
    "  if (mode == 1) {\n"
    "    float outer = clamp(0.5 - d, 0.0, 1.0);\n"
    "    float bw = i.prm.x;\n"
    "    float inner = bw > 0.0 ? clamp(0.5 - (d + bw), 0.0, 1.0) : outer;\n"
    "    float fa = c.a * inner, ba = i.bcolor.a * (outer - inner);\n"
    "    return float4(c.rgb * fa + i.bcolor.rgb * ba, fa + ba);\n"
    "  } else if (mode == 2) {\n"
    "    float s = max(i.prm.x, 0.01);\n"
    "    float a = c.a * 0.5 * (1.0 - erf_(d / (s * 1.41421356)));\n"
    "    return float4(c.rgb * a, a);\n"
    "  } else if (mode == 5) {\n"
    "    float tt = bayer8(i.uv);\n"
    "    float k = tt >= i.prm.x && tt < c.a ? i.bcolor.a * clamp(0.5 - d, 0.0, 1.0) : 0.0;\n"
    "    return float4(i.bcolor.rgb * k, k);\n"
    "  }\n"
    "  float cov = clamp(0.5 - d, 0.0, 1.0);\n"
    "  float k = c.a * cov;\n"
    "  return float4(t.rgb * t.a * c.rgb * k, t.a * k);\n"
    "}\n";

typedef HRESULT (WINAPI *PFN_D3DCompile_)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static bool rp_blob_io(const char *path, const char *entry, const char *target, ID3DBlob **out, void **raw, size_t *rawn) {
    /* with a blob file: read "entry" bytecode from it (name.entry); else compile and, when a path is given, write it */
    char name[600];
    snprintf(name, sizeof name, "%s.%s", path ? path : "", entry);
    if (path) {
        FILE *f = fopen(name, "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            *raw = malloc((size_t)n); *rawn = fread(*raw, 1, (size_t)n, f); fclose(f);
            return true;
        }
    }
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    PFN_D3DCompile_ comp = dll ? (PFN_D3DCompile_)(void *)GetProcAddress(dll, "D3DCompile") : NULL;
    if (!comp) return false;
    ID3DBlob *err = NULL;
    HRESULT hr = comp(rp_hlsl, sizeof rp_hlsl - 1, "probe.hlsl", NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr)) { fprintf(stderr, "hlsl: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return false; }
    *raw = ID3D10Blob_GetBufferPointer(*out); *rawn = ID3D10Blob_GetBufferSize(*out);
    if (path) { FILE *f = fopen(name, "wb"); if (f) { fwrite(*raw, 1, *rawn, f); fclose(f); } }
    return true;
}

static const GUID rp_iid_sc2 = { 0xa8be2ac4, 0x199f, 0x4946, { 0xb3, 0x31, 0x79, 0x59, 0x9f, 0xb9, 0x8d, 0xe7 } };   /* IDXGISwapChain2 */

typedef struct {
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGISwapChain2 *sc;
    ID3D11RenderTargetView *rtv;
    HANDLE wait;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11InputLayout *il;
    ID3D11Buffer *vb, *ib, *cb;
    ID3D11ShaderResourceView *srv;
    ID3D11SamplerState *smp;
    ID3D11BlendState *bs;
    ID3D11RasterizerState *rs;
    double compile_ms;
} rp_d3d;

#define RP_CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { fprintf(stderr, "failed 0x%08lx at line %d: %s\n", (unsigned long)hr_, __LINE__, #x); exit(2); } } while (0)

static void rp_d3d_init(rp_d3d *d, HWND hwnd, int buffers, int latency, bool load, const char *blob) {
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    RP_CHECK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, fl, 3, D3D11_SDK_VERSION, &d->dev, &got, &d->ctx));
    IDXGIDevice *dxd; RP_CHECK(ID3D11Device_QueryInterface(d->dev, &IID_IDXGIDevice, (void **)&dxd));
    IDXGIAdapter *ad; RP_CHECK(IDXGIDevice_GetAdapter(dxd, &ad));
    IDXGIFactory2 *fac; RP_CHECK(IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&fac));
    DXGI_SWAP_CHAIN_DESC1 sd;
    memset(&sd, 0, sizeof sd);
    sd.Width = RP_W; sd.Height = RP_H; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = (UINT)buffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    IDXGISwapChain1 *sc1;
    RP_CHECK(IDXGIFactory2_CreateSwapChainForHwnd(fac, (IUnknown *)d->dev, hwnd, &sd, NULL, NULL, &sc1));
    IDXGIFactory2_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_ALT_ENTER);
    RP_CHECK(IDXGISwapChain1_QueryInterface(sc1, &rp_iid_sc2, (void **)&d->sc));
    IDXGISwapChain1_Release(sc1);
    IDXGISwapChain2_SetMaximumFrameLatency(d->sc, (UINT)latency);
    d->wait = IDXGISwapChain2_GetFrameLatencyWaitableObject(d->sc);
    IDXGIFactory2_Release(fac); IDXGIAdapter_Release(ad); IDXGIDevice_Release(dxd);
    ID3D11Texture2D *bb;
    RP_CHECK(IDXGISwapChain2_GetBuffer(d->sc, 0, &IID_ID3D11Texture2D, (void **)&bb));
    RP_CHECK(ID3D11Device_CreateRenderTargetView(d->dev, (ID3D11Resource *)bb, NULL, &d->rtv));
    ID3D11Texture2D_Release(bb);
    if (!load) return;
    double t0 = rp_now();
    ID3DBlob *vb = NULL, *pb = NULL; void *vraw = NULL, *praw = NULL; size_t vn = 0, pn = 0;
    if (!rp_blob_io(blob, "vs_main", "vs_4_0", &vb, &vraw, &vn) || !rp_blob_io(blob, "ps_main", "ps_4_0", &pb, &praw, &pn)) exit(3);
    d->compile_ms = rp_now() - t0;
    RP_CHECK(ID3D11Device_CreateVertexShader(d->dev, vraw, vn, NULL, &d->vs));
    RP_CHECK(ID3D11Device_CreatePixelShader(d->dev, praw, pn, NULL, &d->ps));
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 5, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 56, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 6, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 60, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    RP_CHECK(ID3D11Device_CreateInputLayout(d->dev, il, 8, vraw, vn, &d->il));
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof bd);
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = (UINT)(sizeof(cui_vertex) * (size_t)rp_nv); bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    RP_CHECK(ID3D11Device_CreateBuffer(d->dev, &bd, NULL, &d->vb));
    bd.ByteWidth = (UINT)(sizeof(uint32_t) * (size_t)rp_ni); bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    RP_CHECK(ID3D11Device_CreateBuffer(d->dev, &bd, NULL, &d->ib));
    bd.ByteWidth = 16; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    RP_CHECK(ID3D11Device_CreateBuffer(d->dev, &bd, NULL, &d->cb));
    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof td);
    td.Width = td.Height = RP_ATLAS; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sr = { rp_atlas, RP_ATLAS, 0 };
    ID3D11Texture2D *tex;
    RP_CHECK(ID3D11Device_CreateTexture2D(d->dev, &td, &sr, &tex));
    RP_CHECK(ID3D11Device_CreateShaderResourceView(d->dev, (ID3D11Resource *)tex, NULL, &d->srv));
    ID3D11Texture2D_Release(tex);
    D3D11_SAMPLER_DESC sm;
    memset(&sm, 0, sizeof sm);
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    RP_CHECK(ID3D11Device_CreateSamplerState(d->dev, &sm, &d->smp));
    D3D11_BLEND_DESC bl;
    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlend = bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    RP_CHECK(ID3D11Device_CreateBlendState(d->dev, &bl, &d->bs));
    D3D11_RASTERIZER_DESC rs;
    memset(&rs, 0, sizeof rs);
    rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.ScissorEnable = TRUE; rs.DepthClipEnable = TRUE;
    RP_CHECK(ID3D11Device_CreateRasterizerState(d->dev, &rs, &d->rs));
}

static void rp_d3d_frame(rp_d3d *d, bool load, int frame) {
    float clear[4] = { 0.1f, 0.1f, 0.12f, 1.0f };
    D3D11_VIEWPORT vp = { 0, 0, RP_W, RP_H, 0, 1 };
    ID3D11DeviceContext_OMSetRenderTargets(d->ctx, 1, &d->rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(d->ctx, 1, &vp);
    ID3D11DeviceContext_ClearRenderTargetView(d->ctx, d->rtv, clear);
    if (!load) return;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11DeviceContext_Map(d->ctx, (ID3D11Resource *)d->vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    cui_vertex *dst = m.pData;
    float dy = (float)(frame % 16);
    for (int i = 0; i < rp_nv; i++) { dst[i] = rp_verts[i]; dst[i].y -= dy; }   /* the frame's own vertices, as CUI would build them */
    ID3D11DeviceContext_Unmap(d->ctx, (ID3D11Resource *)d->vb, 0);
    ID3D11DeviceContext_Map(d->ctx, (ID3D11Resource *)d->ib, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    memcpy(m.pData, rp_idx, sizeof(uint32_t) * (size_t)rp_ni);
    ID3D11DeviceContext_Unmap(d->ctx, (ID3D11Resource *)d->ib, 0);
    ID3D11DeviceContext_Map(d->ctx, (ID3D11Resource *)d->cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    ((float *)m.pData)[0] = RP_W; ((float *)m.pData)[1] = RP_H;
    ID3D11DeviceContext_Unmap(d->ctx, (ID3D11Resource *)d->cb, 0);
    UINT stride = sizeof(cui_vertex), off = 0;
    ID3D11DeviceContext_IASetInputLayout(d->ctx, d->il);
    ID3D11DeviceContext_IASetPrimitiveTopology(d->ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(d->ctx, 0, 1, &d->vb, &stride, &off);
    ID3D11DeviceContext_IASetIndexBuffer(d->ctx, d->ib, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_VSSetShader(d->ctx, d->vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(d->ctx, 0, 1, &d->cb);
    ID3D11DeviceContext_PSSetShader(d->ctx, d->ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(d->ctx, 0, 1, &d->srv);
    ID3D11DeviceContext_PSSetSamplers(d->ctx, 0, 1, &d->smp);
    ID3D11DeviceContext_OMSetBlendState(d->ctx, d->bs, NULL, 0xffffffffu);
    ID3D11DeviceContext_RSSetState(d->ctx, d->rs);
    for (int c = 0; c < RP_CMDS; c++) {
        D3D11_RECT r = { (LONG)floorf(rp_cmds[c].clip.x), (LONG)floorf(rp_cmds[c].clip.y),
                         (LONG)ceilf(rp_cmds[c].clip.x + rp_cmds[c].clip.w), (LONG)ceilf(rp_cmds[c].clip.y + rp_cmds[c].clip.h) };
        ID3D11DeviceContext_RSSetScissorRects(d->ctx, 1, &r);
        ID3D11DeviceContext_DrawIndexed(d->ctx, rp_cmds[c].index_count, rp_cmds[c].index_offset, 0);
    }
}

/* --------------------------------------------------------------- GL texture */
static cui_texture rp_gl_atlas(cui_gl_renderer *gl) {
    cui_renderer_iface it = cui_gl_iface(gl);
    return it.texture_create(it.user, RP_ATLAS, RP_ATLAS, CUI_TEXTURE_R8, rp_atlas);
}

int main(int argc, char **argv) {
    const char *mode = "d3d", *blob = NULL, *pace = NULL;
    bool load = false;
    int frames = 300, idle_ms = 2000, buffers = 2, latency = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--load")) load = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--idle") && i + 1 < argc) idle_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--buffers") && i + 1 < argc) buffers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--latency") && i + 1 < argc) latency = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--blob") && i + 1 < argc) blob = argv[++i];
        else if (!strcmp(argv[i], "--pace") && i + 1 < argc) pace = argv[++i];
    }
    QueryPerformanceFrequency(&rp_freq);
    double t_main = rp_now();
    SetProcessDPIAware();
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        rp_timing = (PFN_timing)(void *)GetProcAddress(dwm, "DwmGetCompositionTimingInfo");
        rp_dwmflush = (PFN_dwmflush)(void *)GetProcAddress(dwm, "DwmFlush");
    }
    bool isgl = !strcmp(mode, "gl"), isd3d = !strcmp(mode, "d3d");
    if (load) rp_make_load(RP_W, RP_H);
    HWND hwnd = rp_window(RP_W, RP_H, isgl);
    HDC dc = GetDC(hwnd);
    rp_d3d d3;
    memset(&d3, 0, sizeof d3);
    cui_gl_renderer *gl = NULL;
    cui_texture atlas = 0;
    HGLRC glrc = NULL;
    if (isgl) {
        rp_gl32 = LoadLibraryW(L"opengl32.dll");
        glrc = rp_gl_context(dc);
        if (!glrc) { fprintf(stderr, "no GL 3.3\n"); return 2; }
        PFN_wglSI si = (PFN_wglSI)(void *)wglGetProcAddress("wglSwapIntervalEXT");
        if (si) si(pace ? 0 : 1);
        if (load) { gl = cui_gl_create(rp_glproc); if (!gl) return 2; atlas = rp_gl_atlas(gl); }
    } else if (isd3d) {
        rp_d3d_init(&d3, hwnd, buffers, latency, load, blob);
    }
    DWM_TIMING_INFO t0;
    memset(&t0, 0, sizeof t0); t0.cbSize = sizeof t0;
    if (rp_timing) rp_timing(NULL, &t0);

    cui_vertex *glv = load ? malloc(sizeof(cui_vertex) * (size_t)rp_nv) : NULL;
    double *cpu = malloc(sizeof(double) * (size_t)(frames + 1));
    int ncpu = 0;
    double run_start = 0, cpu0 = 0, first_ms = 0;
    bool idled = false;
    double idle_ws = 0, idle_priv = 0, first_since_create = 0;
    for (int f = 0; f <= frames; f++) {
        /* frame 0 is the first presented frame; then the idle pause; then the measured run */
        if (isd3d && f > 0) WaitForSingleObject(d3.wait, 1000);
        double a = rp_now();
        rp_pump();
        if (rp_timing) { DWM_TIMING_INFO tp; memset(&tp, 0, sizeof tp); tp.cbSize = sizeof tp; rp_timing(NULL, &tp); }   /* the counters only advance while asked */
        if (isgl) {
            if (load) {
                float dy = (float)(f % 16);
                for (int i = 0; i < rp_nv; i++) { glv[i] = rp_verts[i]; glv[i].y -= dy; }
                cui_draw_cmd cmds[RP_CMDS];
                memcpy(cmds, rp_cmds, sizeof cmds);
                for (int c = 0; c < RP_CMDS; c++) cmds[c].texture = atlas;
                cui_draw_data dd;
                memset(&dd, 0, sizeof dd);
                dd.vertices = glv; dd.vertex_count = rp_nv; dd.indices = rp_idx; dd.index_count = rp_ni;
                dd.commands = cmds; dd.command_count = RP_CMDS; dd.width_px = RP_W; dd.height_px = RP_H;
                dd.clear_color.r = 25; dd.clear_color.g = 25; dd.clear_color.b = 30; dd.clear_color.a = 255; dd.font_atlas = atlas;
                cui_gl_render(gl, &dd, true);
            } else {
                typedef void (__stdcall *PFN_clear)(unsigned);
                typedef void (__stdcall *PFN_cc)(float, float, float, float);
                ((PFN_cc)rp_glproc("glClearColor"))(0.1f, 0.1f, 0.12f, 1);
                ((PFN_clear)rp_glproc("glClear"))(0x4000);
            }
            if (f == 0) ShowWindow(hwnd, SW_SHOW);
            SwapBuffers(dc);
            if (pace && rp_dwmflush) rp_dwmflush();   /* swap interval 0, paced by the compositor */
        } else if (isd3d) {
            rp_d3d_frame(&d3, load, f);
            if (f == 0) ShowWindow(hwnd, SW_SHOW);
            IDXGISwapChain2_Present(d3.sc, 1, 0);
        } else {
            RECT r = { 0, 0, RP_W, RP_H };
            HBRUSH b = CreateSolidBrush(RGB(25 + f % 8, 25, 30));
            FillRect(dc, &r, b);
            DeleteObject(b);
            if (f == 0) ShowWindow(hwnd, SW_SHOW);
            if (rp_dwmflush) rp_dwmflush();   /* gdi has no swap wait: pace it the same way */
        }
        double b2 = rp_now();
        if (f == 0) {
            first_ms = b2 - t_main;
            {
                FILETIME c_, e_, k_, u_, n_;
                GetProcessTimes(GetCurrentProcess(), &c_, &e_, &k_, &u_);
                GetSystemTimeAsFileTime(&n_);
                first_since_create = (double)(((ULARGE_INTEGER){ .u = { n_.dwLowDateTime, n_.dwHighDateTime } }).QuadPart - ((ULARGE_INTEGER){ .u = { c_.dwLowDateTime, c_.dwHighDateTime } }).QuadPart) / 10000.0;
            }
            Sleep(idle_ms);   /* nothing is drawn: this is the idle figure */
            rp_pump();
            rp_mem(&idle_ws, &idle_priv);
            idled = true;
            run_start = rp_now();
            cpu0 = rp_cpu_ms();
        } else {
            cpu[ncpu++] = b2 - a;
        }
    }
    (void)idled;
    double run_ms = rp_now() - run_start, cpu_used = rp_cpu_ms() - cpu0;
    double ws, priv;
    rp_mem(&ws, &priv);
    DWM_TIMING_INFO t1;
    memset(&t1, 0, sizeof t1); t1.cbSize = sizeof t1;
    if (rp_timing) rp_timing(NULL, &t1);
    qsort(cpu, (size_t)ncpu, sizeof *cpu, rp_cmp);
    printf("mode=%s%s%s first_frame_ms=%.1f (since process start=%.1f) shader_compile_ms=%.1f idle_ws_mb=%.1f idle_private_mb=%.1f run_ws_mb=%.1f run_private_mb=%.1f "
           "fps=%.1f frame_cpu_ms p50=%.2f p95=%.2f max=%.2f process_cpu_ms_per_s=%.0f dwm rate=%.2fHz refreshes=%llu composed=%llu dropped=%llu missed=%llu\n",
           mode, load ? "+load" : "", blob ? "+blob" : "", first_ms, first_since_create, d3.compile_ms, idle_ws, idle_priv, ws, priv,
           ncpu * 1000.0 / run_ms, cpu[ncpu / 2], cpu[(int)(ncpu * 0.95)], cpu[ncpu - 1], cpu_used * 1000.0 / run_ms,
           t1.rateRefresh.uiDenominator ? (double)t1.rateRefresh.uiNumerator / t1.rateRefresh.uiDenominator : 0.0,
           (unsigned long long)(t1.cRefresh - t0.cRefresh), (unsigned long long)(t1.cFrame - t0.cFrame),
           (unsigned long long)(t1.cFramesDropped - t0.cFramesDropped), (unsigned long long)(t1.cFramesMissed - t0.cFramesMissed));
    return 0;
}

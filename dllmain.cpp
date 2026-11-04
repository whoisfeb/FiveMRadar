// =====================================================================
//  FiveMRadar3D.asi  -  Radar 3D miring, bentuk & posisi tetap bawaan game
//  GTA SA 1.0 US (SA-MP 0.3.7).  Build: Release | x86.  Wajib: MinHook.
//
//  Proyeksi 3D dan zoom disamakan dengan Radar3D.lua (v1.8):
//   - Proyeksi: X = u/(1+v*sin(tilt)), Y = v*cos(tilt)/(1+v*sin(tilt)),
//     u,v = jarak dunia / D  (D = zoom dunia, 40..1000)
//   - Zoom dinamis di kendaraan: D * (1.35 + min(speed/180,1)), dihaluskan 0.06/frame
//   - tilt 0..40 derajat
//
//  Tilt dan zoom diatur lewat konstanta di bagian KONFIGURASI (tanpa hotkey).
//
//  Cara kerja:
//   1. Hook CHud::DrawRadar : ukur radar bawaan; seluruh radar bawaan
//                             (peta, cincin, mask) digambar ke texture
//                             (resolusi = layar x SUPERSAMPLE, dengan mipmap).
//   2. Hook CRadar::TransformRadarPointToScreenSpace : dipanggil setiap blip.
//                             Pada panggilan pertama, render target kembali ke
//                             layar dan peta digambar sebagai cakram bulat
//                             miring di posisi radar bawaan. Posisi tiap blip
//                             dihitung ulang dengan proyeksi yang sama, icon
//                             tetap digambar game (tegak, ukuran dikecilkan
//                             oleh BLIP_SCALE).
//   3. Zoom: jangkauan radar game (CRadar::m_radarRange) ditulis ABSOLUT
//        = D * m. (6 titik tulis di DrawMap dialihkan ke stub.)
//   4. Hook CRadar::DrawMap : penanda fase peta.
//   5. Hook EndScene        : ambil device.
//
//  Perbaikan v2:
//   - Radius cakram tidak lagi diambil dari lebar saja (min(lebar, tinggi*1.25)).
//   - Posisi cakram + bar health/armor dijepit supaya selalu masuk layar.
//   - Ukuran radar sumber (X/Y/lebar/tinggi di memori game) HANYA diubah selama
//     CHud::DrawRadar berjalan, lalu dikembalikan ke nilai asli game. Dengan
//     begitu Pause Menu / HUD lain tidak ikut terpengaruh.
//
//  v3:
//   - BLIP_SCALE: semua icon/blip radar bisa diperkecil. Ukuran layar internal
//     game (RsGlobal.maximumWidth/Height) diperkecil sementara hanya selama fase
//     blip, lalu dipulihkan.
//
//  v10: SEMUA FITUR GPS DIHAPUS (sementara). Radar 3D tetap sama; blip, zoom,
//       skala icon, dan layout tidak berubah.
//
//  Log: radar3d.log di folder GTA SA.
// =====================================================================
#include "pch.h"
#include <windows.h>
#include <d3d9.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MinHook.h"
#pragma comment(lib, "d3d9.lib")

// ------------------------- KONFIGURASI -------------------------------
// ====== ATUR DI SINI (tidak ada hotkey) ======
static const float g_pitchDeg = 33.0f;   // tilt 0..40 derajat
static const float g_zoomWorld = 150.0f;  // zoom dunia 40..1000 (lebih besar = lebih tajam)
// =============================================
static const float SUPERSAMPLE = 2.0f;    // 1.0 = mati, 2.0 = tajam, 3.0 = paling tajam (berat)
static const bool  USE_MIPMAP = true;    // hilangkan gerigi di sisi jauh

static const bool  DYNAMIC_ZOOM = false;  // false = zoom TETAP (di dalam/luar kendaraan sama); true = zoom naik saat di kendaraan
static const float SPAN_MAX = 10.0f;       // batas pelebaran area sumber (jangkauan sumber / D)

static const float RADAR_SCALE = 1.20f;   // 1.0 = ukuran radar bawaan; >1 lebih besar
static const float OFFSET_X = 0.0f;       // geser radar (piksel) dari posisi bawaan
static const float OFFSET_Y = 0.0f;
static const bool  SHOW_BARS = false;     // bar health/armor di bawah radar (false = hilang)
static const bool  SHOW_FRAME = false;    // bingkai bulat gelap di sekeliling radar (false = hilang)

static const float BLIP_SCALE = 0.5f;    // ukuran icon/blip radar: 1.0 = normal, 0.5 = setengah

static const float FAR_DARKEN = 0.18f;    // sisi jauh sedikit lebih gelap (0..1)
static const float SRC_CROP = 0.95f;      // bagian tengah sumber yang dipakai (0..1)
static const float CLIP_MARGIN = 40.0f;   // margin scissor di sekitar radar (piksel)
static const float EDGE_PAD = 6.0f;       // jarak minimum cakram/bar ke tepi layar (piksel)

// true  = ukuran radar sumber diperbesar (menulis X, Y, LEBAR, TINGGI) hanya selama DrawRadar
// false = tidak menulis memori sama sekali
static const bool  ENLARGE_SOURCE = true;
static const float SRC_HEIGHT_VU = 300.0f; // tinggi radar sumber (virtual 640x448; standar 76)
static const float SRC_WIDTH_VU = 150.0f;  // lebar radar sumber  (virtual 640x448; standar 94)
static const float SRC_X_VU = 175.0f;      // posisi X radar sumber (virtual 640); standar 40.
// digeser ke kanan supaya tidak menimpa cincin hitam bawaan di kiri bawah
// jangan terlalu besar: tinggi <= ~380, lebar <= ~220

// ------------------------- ALAMAT GTA SA 1.0 US ----------------------
static const DWORD ADDR_RADAR_X = 0x858A10;
static const DWORD ADDR_RADAR_Y = 0x866B70;
static const DWORD ADDR_RADAR_W = 0x866B78;
static const DWORD ADDR_RADAR_H = 0x866B74;
static const DWORD ADDR_DRAWRADAR = 0x58A330;    // CHud::DrawRadar
static const DWORD ADDR_DRAWMAP = 0x586B00;      // CRadar::DrawMap
static const DWORD ADDR_TRANSFORM = 0x583480;    // CRadar::TransformRadarPointToScreenSpace
static const DWORD ADDR_WORLD2RADAR = 0x583530;  // CRadar::TransformRealWorldPointToRadarSpace
static const DWORD ADDR_RADAR2WORLD = 0x5835A0;  // CRadar::TransformRadarPointToRealWorldSpace
static const DWORD ADDR_PLAYER0 = 0xB7CD98;
static const DWORD ADDR_SCREEN_W = 0xC17044;     // RsGlobal.maximumWidth  (dipakai skala icon)
static const DWORD ADDR_SCREEN_H = 0xC17048;     // RsGlobal.maximumHeight

// ------------------------- TIPE & GLOBAL -----------------------------
struct Vec2 { float x, y; };

typedef HRESULT(__stdcall* EndScene_t)(IDirect3DDevice9*);
typedef HRESULT(__stdcall* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef void(__cdecl* VoidFn_t)();
typedef void(__cdecl* Transform_t)(Vec2*, Vec2*);

static EndScene_t  oEndScene = nullptr;
static Reset_t     oReset = nullptr;
static VoidFn_t    oDrawRadar = nullptr;
static VoidFn_t    oDrawMap = nullptr;
static Transform_t oTransform = nullptr;
static Transform_t oWorldToRadar = nullptr;
static Transform_t oRadarToWorld = nullptr;

static IDirect3DDevice9* g_dev = nullptr;
static IDirect3DTexture9* g_tex = nullptr;
static IDirect3DSurface9* g_texSurf = nullptr;
static IDirect3DSurface9* g_depth = nullptr;
static UINT  g_w = 0, g_h = 0;     // ukuran layar (back buffer)
static UINT  g_tw = 0, g_th = 0;   // ukuran texture (layar x supersample)
static float g_ss = 1.0f;          // faktor supersample efektif
static bool  g_mip = false;        // texture punya mipmap otomatis
static const bool g_enabled = true;
static bool  g_hooksOk = false;
static bool  g_inHud = false;      // di dalam CHud::DrawRadar (mode 3D aktif)
static bool  g_inMap = false;      // di dalam CRadar::DrawMap (fase peta)
static bool  g_projValid = false;  // parameter proyeksi frame ini sudah dihitung
static bool  g_inTex = false;      // sedang menggambar ke texture
static bool  g_scissorOn = false;  // scissor aktif di fase layar

// state render target yang disimpan selama fase texture
static IDirect3DSurface9* g_oldRT = nullptr;
static IDirect3DSurface9* g_oldDS = nullptr;
static D3DVIEWPORT9       g_oldVP;

// radar bawaan (diukur sekali, sebelum kita ubah apa pun)
static bool  g_defMeasured = false;
static float g_defCx = 0, g_defCy = 0, g_defHW = 0, g_defHH = 0;

// radar sumber (yang digambar ke texture; diukur tiap frame, dalam piksel layar 1x)
static float g_srcCx = 0, g_srcCy = 0;     // pusat radar sumber
static float g_halfW = 1, g_halfH = 1;     // setengah lebar/tinggi sumber

// parameter proyeksi untuk memosisikan blip
static float g_cx = 0, g_cy = 0, g_R = 1;  // pusat & jari-jari cakram di layar
static float g_cosP = 1, g_sinP = 0;       // sudut miring
static float g_m = 1.0f;                   // jangkauan sumber / D

// zoom absolut & zoom dinamis
static float g_absRange = 0.0f;            // 0 = pakai nilai bawaan game; diisi tiap frame
static float g_dynZoom = 0.0f;
static float g_speed = 0.0f;               // km/jam (dihaluskan)
static float g_dynX = 0.0f, g_dynY = 0.0f;
static DWORD g_dynT = 0;

struct VtxT { float x, y, z, rhw; DWORD color; float u, v; };
struct VtxC { float x, y, z, rhw; DWORD color; };
#define FVF_T (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
#define FVF_C (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static void Log(const char* msg)
{
    FILE* f = nullptr;
    if (fopen_s(&f, "radar3d.log", "a") == 0 && f) { fprintf(f, "%s\n", msg); fclose(f); }
}

// ------------------------- MEMORI GAME -------------------------------
static void PokeFloat(DWORD addr, float v)
{
    __try { *(volatile float*)addr = v; return; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    DWORD op;
    if (VirtualProtect((LPVOID)addr, sizeof(float), PAGE_EXECUTE_READWRITE, &op))
        *(volatile float*)addr = v;
}

static bool PeekFloat(DWORD addr, float& v)
{
    __try { v = *(volatile float*)addr; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool ReadPlayerStats(float& hp, float& armor)
{
    __try
    {
        DWORD ped = *(volatile DWORD*)ADDR_PLAYER0;
        if (!ped) return false;
        hp = *(volatile float*)(ped + 0x540);
        armor = *(volatile float*)(ped + 0x548);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Posisi pemain (x, y) dan apakah sedang di kendaraan.
static bool ReadPlayerPos(float& x, float& y, bool& inCar)
{
    __try
    {
        DWORD ped = *(volatile DWORD*)ADDR_PLAYER0;
        if (!ped) return false;
        DWORD mat = *(volatile DWORD*)(ped + 0x14);          // CMatrix*
        if (!mat) return false;
        x = *(volatile float*)(mat + 0x30);
        y = *(volatile float*)(mat + 0x34);
        inCar = (*(volatile DWORD*)(ped + 0x58C)) != 0;      // CPed::m_pVehicle
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Ukur radar sebagaimana game menggambarnya: pusat & setengah lebar/tinggi (piksel layar).
// Memakai fungsi transform ASLI game (oTransform, tanpa skala supersample).
static bool MeasureRadar(float& cx, float& cy, float& hw, float& hh)
{
    if (!oTransform) return false;
    Vec2 i0 = { 0.0f, 0.0f }, i1 = { 1.0f, 0.0f }, i2 = { 0.0f, 1.0f };
    Vec2 c = {}, ex = {}, ey = {};
    __try
    {
        oTransform(&c, &i0);
        oTransform(&ex, &i1);
        oTransform(&ey, &i2);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    cx = c.x; cy = c.y;
    hw = fabsf(ex.x - c.x);
    hh = fabsf(c.y - ey.y);
    return (hw > 8.0f && hh > 8.0f && hw < 2000.0f && hh < 2000.0f);
}

// ------------------------- PATCH JANGKAUAN RADAR ---------------------
// Jangkauan peta dikendalikan variabel game CRadar::m_radarRange. Tile peta dan posisi
// blip sama-sama membacanya, jadi cukup menulisnya.
// CRadar::DrawMap menulis variabel itu di 6 tempat; semuanya dialihkan ke stub.
// Jika g_absRange > 0, stub menulis nilai absolut itu (zoom dunia * m); jika 0,
// stub menulis nilai bawaan game. Semua byte diverifikasi dulu; kalau ada yang tidak
// cocok, tidak ada yang di-patch.
static DWORD g_rangeAddr = 0;
static float kC340 = 340.0f, kC350 = 350.0f, kC180 = 180.0f;

// menggantikan: fstp dword ptr [range]   (nilai ada di ST0)
// pushfd/popfd: instruksi asli tidak mengubah EFLAGS, jadi harus dipulihkan.
static __declspec(naked) void StubStore()
{
    __asm {
        pushfd
        push eax
        mov eax, g_rangeAddr
        cmp dword ptr[g_absRange], 0
        je keep
        fstp st(0)
        fld dword ptr[g_absRange]
        keep:
        fstp dword ptr[eax]
            pop eax
            popfd
            ret
    }
}
// menggantikan: mov dword ptr [range], imm32
#define DEFINE_CONST_STUB(NAME, VAR)                    \
static __declspec(naked) void NAME()                    \
{                                                       \
    __asm { fld dword ptr[VAR] }                        \
    __asm { jmp StubStore }                             \
}
DEFINE_CONST_STUB(StubC340, kC340)
DEFINE_CONST_STUB(StubC350, kC350)
DEFINE_CONST_STUB(StubC180, kC180)

static bool ReadBytes(DWORD addr, unsigned char* out, int n)
{
    __try { for (int i = 0; i < n; i++) out[i] = *(volatile unsigned char*)(addr + i); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void WriteCall(DWORD site, void* stub, int totalLen)
{
    DWORD op;
    VirtualProtect((LPVOID)site, totalLen, PAGE_EXECUTE_READWRITE, &op);
    unsigned char* p = (unsigned char*)site;
    p[0] = 0xE8;
    *(DWORD*)(p + 1) = (DWORD)stub - (site + 5);
    for (int i = 5; i < totalLen; i++) p[i] = 0x90;
    VirtualProtect((LPVOID)site, totalLen, op, &op);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)site, totalLen);
}

static bool InstallRangePatch()
{
    struct ImmSite { DWORD off; float val; void* stub; };
    ImmSite imm[3] = { { 0xB8, 340.0f, (void*)StubC340 },
                       { 0xF9, 350.0f, (void*)StubC350 },
                       { 0x195, 180.0f, (void*)StubC180 } };
    const DWORD fst[3] = { 0xEE, 0x16A, 0x18D };
    DWORD addr = 0;
    char buf[160];

    for (int i = 0; i < 3; i++)
    {
        unsigned char b[10];
        float f = 0.0f;
        DWORD a = 0;
        if (!ReadBytes(ADDR_DRAWMAP + imm[i].off, b, 10) || b[0] != 0xC7 || b[1] != 0x05)
        {
            sprintf_s(buf, "Patch gagal: DrawMap+0x%X bukan 'mov [mem], imm'", imm[i].off); Log(buf); return false;
        }
        memcpy(&a, b + 2, 4);
        memcpy(&f, b + 6, 4);
        if (fabsf(f - imm[i].val) > 0.01f)
        {
            sprintf_s(buf, "Patch gagal: DrawMap+0x%X imm=%f (harusnya %f)", imm[i].off, f, imm[i].val); Log(buf); return false;
        }
        if (i == 0) addr = a;
        else if (a != addr)
        {
            sprintf_s(buf, "Patch gagal: alamat range beda di DrawMap+0x%X", imm[i].off); Log(buf); return false;
        }
    }
    for (int i = 0; i < 3; i++)
    {
        unsigned char b[6];
        DWORD a = 0;
        if (!ReadBytes(ADDR_DRAWMAP + fst[i], b, 6) || b[0] != 0xD9 || b[1] != 0x1D)
        {
            sprintf_s(buf, "Patch gagal: DrawMap+0x%X bukan 'fstp [mem]'", fst[i]); Log(buf); return false;
        }
        memcpy(&a, b + 2, 4);
        if (a != addr)
        {
            sprintf_s(buf, "Patch gagal: fstp DrawMap+0x%X menulis ke alamat lain", fst[i]); Log(buf); return false;
        }
    }
    if (!addr) { Log("Patch gagal: alamat range 0"); return false; }

    g_rangeAddr = addr;
    for (int i = 0; i < 3; i++) WriteCall(ADDR_DRAWMAP + imm[i].off, imm[i].stub, 10);
    for (int i = 0; i < 3; i++) WriteCall(ADDR_DRAWMAP + fst[i], (void*)StubStore, 6);

    sprintf_s(buf, "Patch jangkauan radar: terpasang (m_radarRange @ 0x%08X)", addr);
    Log(buf);
    return true;
}

// ------------------------- LAYOUT RADAR SUMBER -----------------------
// Nilai asli (X, Y, lebar, tinggi radar di memori game) dibaca SEKALI sebelum ada
// perubahan, lalu dipulihkan setiap kali CHud::DrawRadar selesai. Jadi nilai yang
// diperbesar hanya "terlihat" oleh game selama radar digambar (tidak bocor ke
// Pause Menu atau HUD lain).
static bool  g_layoutCaptured = false;
static float g_origX = 40.0f, g_origY = 104.0f, g_origW = 94.0f, g_origH = 76.0f;

static void CaptureLayout()
{
    if (g_layoutCaptured) return;
    float x = 0, y = 0, w = 0, h = 0;
    if (PeekFloat(ADDR_RADAR_X, x) && PeekFloat(ADDR_RADAR_Y, y) &&
        PeekFloat(ADDR_RADAR_W, w) && PeekFloat(ADDR_RADAR_H, h) &&
        h > 70.0f && h < 80.0f && w > 50.0f && w < 150.0f)
    {
        g_origX = x; g_origY = y; g_origW = w; g_origH = h;
        g_layoutCaptured = true;
        char buf[160];
        sprintf_s(buf, "Layout asli radar: X=%.2f Y=%.2f W=%.2f H=%.2f", x, y, w, h);
        Log(buf);
    }
}

// enabled = true  -> tulis ukuran radar sumber yang diperbesar
// enabled = false -> kembalikan nilai asli game
static void ApplyLayout(bool enabled)
{
    if (!ENLARGE_SOURCE || !g_layoutCaptured) return;

    if (enabled)
    {
        if (g_h == 0) return;
        float H = (float)g_h;
        const float topPx = 20.0f;
        float yVU = (H - topPx) * 448.0f / H;
        PokeFloat(ADDR_RADAR_X, SRC_X_VU);
        PokeFloat(ADDR_RADAR_Y, yVU);
        PokeFloat(ADDR_RADAR_W, SRC_WIDTH_VU);
        PokeFloat(ADDR_RADAR_H, SRC_HEIGHT_VU);
    }
    else
    {
        PokeFloat(ADDR_RADAR_X, g_origX);
        PokeFloat(ADDR_RADAR_Y, g_origY);
        PokeFloat(ADDR_RADAR_W, g_origW);
        PokeFloat(ADDR_RADAR_H, g_origH);
    }
}

// ------------------------- SKALA ICON / BLIP -------------------------
// Ukuran icon/blip dihitung game dari ukuran layar internal
// (RsGlobal.maximumWidth/Height). Selama fase blip, nilai itu diperkecil sementara
// (BLIP_SCALE), lalu dipulihkan di akhir CHud::DrawRadar. Posisi blip tidak
// terpengaruh karena dihitung ulang di hkTransform.
static bool g_scaleOn = false;
static int  g_origSW = 0, g_origSH = 0;

static void BeginIconScale()
{
    if (g_scaleOn || BLIP_SCALE >= 0.999f) return;
    __try
    {
        int w = *(volatile int*)ADDR_SCREEN_W;
        int h = *(volatile int*)ADDR_SCREEN_H;
        if (w < 320 || h < 200 || w > 16384 || h > 16384) return;
        g_origSW = w; g_origSH = h;
        *(volatile int*)ADDR_SCREEN_W = (int)(w * BLIP_SCALE + 0.5f);
        *(volatile int*)ADDR_SCREEN_H = (int)(h * BLIP_SCALE + 0.5f);
        g_scaleOn = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void EndIconScale()
{
    if (!g_scaleOn) return;
    __try
    {
        *(volatile int*)ADDR_SCREEN_W = g_origSW;
        *(volatile int*)ADDR_SCREEN_H = g_origSH;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_scaleOn = false;
}

// ------------------------- ZOOM DINAMIS & PROYEKSI -------------------
// target = zoom * (1.35 + min(speed/180, 1)) saat di kendaraan, lalu dihaluskan 0.06/frame.
static void UpdateDynamic()
{
    // Zoom tetap: selalu g_zoomWorld, tanpa penghalusan dan tanpa pengaruh kendaraan/kecepatan
    if (!DYNAMIC_ZOOM) { g_dynZoom = g_zoomWorld; return; }

    if (g_dynZoom <= 0.0f) g_dynZoom = g_zoomWorld;

    float x = 0, y = 0;
    bool inCar = false;
    if (!ReadPlayerPos(x, y, inCar)) return;

    DWORD now = GetTickCount();
    if (g_dynT == 0) { g_dynX = x; g_dynY = y; g_dynT = now; }
    else
    {
        float dt = (float)(now - g_dynT) / 1000.0f;
        if (dt >= 0.1f)
        {
            float dx = x - g_dynX, dy = y - g_dynY;
            float v = sqrtf(dx * dx + dy * dy) / dt * 3.6f;       // km/jam
            if (v < 450.0f) g_speed = g_speed * 0.6f + v * 0.4f;
            g_dynX = x; g_dynY = y; g_dynT = now;
        }
    }

    float target = g_zoomWorld;
    if (DYNAMIC_ZOOM && inCar)
        target *= 1.35f + fminf(g_speed / 180.0f, 1.0f);
    g_dynZoom += (target - g_dynZoom) * 0.06f;
}

// Titik cakram layar (px,py; -1..1) -> titik di bidang peta miring, satuan D.
// Invers dari proyeksi:  X = u/(1+v*sin),  Y = v*cos/(1+v*sin)
static void PlaneFromScreen(float px, float py, float& lx, float& ly)
{
    float den = g_cosP - py * g_sinP;
    if (den < 0.15f) den = 0.15f;
    ly = py / den;
    lx = px * (1.0f + ly * g_sinP);
}

// Hitung sin/cos sudut miring dan faktor pelebaran area sumber (g_m).
static void UpdateProjection()
{
    const float PI = 3.14159265f;
    float pitch = g_pitchDeg * PI / 180.0f;
    g_cosP = cosf(pitch);
    g_sinP = sinf(pitch);

    float mx = 1.0f;
    for (int i = 0; i < 72; i++)
    {
        float a = (float)i * 2.0f * PI / 72.0f;
        float lx, ly;
        PlaneFromScreen(cosf(a), sinf(a), lx, ly);
        float r = sqrtf(lx * lx + ly * ly);
        if (r > mx) mx = r;
    }
    g_m = mx / SRC_CROP;
    if (g_m > SPAN_MAX) g_m = SPAN_MAX;
    if (g_m < 1.0f) g_m = 1.0f;
}

// ------------------------- RESOURCE D3D ------------------------------
static void ReleaseResources()
{
    if (g_texSurf) { g_texSurf->Release(); g_texSurf = nullptr; }
    if (g_tex) { g_tex->Release();     g_tex = nullptr; }
    if (g_depth) { g_depth->Release();   g_depth = nullptr; }
}

static bool EnsureResources(IDirect3DDevice9* dev)
{
    if (g_tex && g_texSurf) return true;
    ReleaseResources();

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    D3DSURFACE_DESC d; bb->GetDesc(&d); bb->Release();
    g_w = d.Width; g_h = d.Height;

    // ukuran texture = layar x supersample (maks 4096 di sisi terpanjang)
    g_ss = SUPERSAMPLE;
    if (g_ss < 1.0f) g_ss = 1.0f;
    float big = (float)(g_w > g_h ? g_w : g_h);
    if (big * g_ss > 4096.0f) g_ss = 4096.0f / big;
    if (g_ss < 1.0f) g_ss = 1.0f;
    g_tw = (UINT)(g_w * g_ss + 0.5f);
    g_th = (UINT)(g_h * g_ss + 0.5f);

    D3DFORMAT dfmt = D3DFMT_D24S8;
    IDirect3DSurface9* ds = nullptr;
    if (SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds)
    {
        D3DSURFACE_DESC dd; ds->GetDesc(&dd); dfmt = dd.Format; ds->Release();
    }

    static bool logged = false;
    g_mip = false;
    if (USE_MIPMAP &&
        SUCCEEDED(dev->CreateTexture(g_tw, g_th, 0, D3DUSAGE_RENDERTARGET | D3DUSAGE_AUTOGENMIPMAP,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_tex, nullptr)) && g_tex)
    {
        g_mip = true;
    }
    else if (FAILED(dev->CreateTexture(g_tw, g_th, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT, &g_tex, nullptr)) || !g_tex)
    {
        if (!logged) { Log("Gagal membuat render target texture"); logged = true; }
        g_tex = nullptr; return false;
    }
    HRESULT hrSurf = g_tex->GetSurfaceLevel(0, &g_texSurf);
    if (FAILED(hrSurf) || !g_texSurf)
    {
        if (!logged)
        {
            char b[120]; sprintf_s(b, "Gagal GetSurfaceLevel(0) texture radar, hr=0x%08X", (unsigned)hrSurf);
            Log(b); logged = true;
        }
        ReleaseResources(); return false;
    }
    // Depth surface: coba format milik game, lalu format umum lain. Radar 2D tidak butuh depth
    // (ZENABLE = FALSE), jadi bila semua gagal tetap lanjut tanpa depth (g_depth = null).
    {
        D3DFORMAT tries[4] = { dfmt, D3DFMT_D24S8, D3DFMT_D24X8, D3DFMT_D16 };
        HRESULT hrD = D3DERR_INVALIDCALL;
        for (int i = 0; i < 4 && !g_depth; i++)
        {
            if (i > 0 && tries[i] == dfmt) continue;
            hrD = dev->CreateDepthStencilSurface(g_tw, g_th, tries[i], D3DMULTISAMPLE_NONE, 0, TRUE,
                &g_depth, nullptr);
            if (FAILED(hrD)) g_depth = nullptr;
            else if (!logged || i > 0)
            {
                char b[120]; sprintf_s(b, "Depth surface radar dibuat (format %d)", (int)tries[i]); Log(b);
            }
        }
        if (!g_depth && !logged)
        {
            char b[160];
            sprintf_s(b, "Depth surface gagal (hr=0x%08X, format game=%d, %ux%u) -> lanjut tanpa depth",
                (unsigned)hrD, (int)dfmt, g_tw, g_th);
            Log(b);
        }
        logged = true;
    }
    Log(g_mip ? "Resource radar dibuat (supersample + mipmap)" : "Resource radar dibuat (tanpa mipmap)");
    return true;
}

// ------------------------- GAMBAR 3D ---------------------------------
static void SetBaseStates(IDirect3DDevice9* dev)
{
    dev->SetPixelShader(nullptr);
    dev->SetVertexShader(nullptr);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, g_mip ? D3DTEXF_LINEAR : D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 8);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
}

static void UseTextured(IDirect3DDevice9* dev)
{
    dev->SetTexture(0, g_tex);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetFVF(FVF_T);
}

static void UseSolid(IDirect3DDevice9* dev)
{
    dev->SetTexture(0, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetFVF(FVF_C);
}

static void DrawRect(IDirect3DDevice9* dev, float x, float y, float w, float h, DWORD color)
{
    VtxC v[4] = {
        { x,     y,     0, 1, color },
        { x + w, y,     0, 1, color },
        { x,     y + h, 0, 1, color },
        { x + w, y + h, 0, 1, color },
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(VtxC));
}

static void DrawBar(IDirect3DDevice9* dev, float x, float y, float w, float h, float frac, DWORD color)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    DrawRect(dev, x - 1, y - 1, w + 2, h + 2, 0xB0000000);
    DrawRect(dev, x, y, w, h, 0x90303030);
    if (frac > 0) DrawRect(dev, x, y, w * frac, h, color);
}

// Menggambar PETA (texture) sebagai cakram bulat miring di posisi radar bawaan.
// Blip TIDAK ikut (digambar game sesudahnya).
static void DrawRadar3D(IDirect3DDevice9* dev)
{
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) return;
    sb->Capture();
    SetBaseStates(dev);

    const float PI = 3.14159265f;
    float W = (float)g_w, H = (float)g_h;

    // --- ukuran & posisi cakram -------------------------------------
    // Radius: jangan hanya pakai lebar (radar bawaan lebih pendek daripada lebar).
    float R = fminf(g_defHW, g_defHH * 1.25f) * RADAR_SCALE;
    float cx = g_defCx + OFFSET_X;
    // Naikkan sedikit supaya bagian bawah cakram tidak melewati bawah radar bawaan.
    float cy = g_defCy + OFFSET_Y - (R - g_defHH);

    // Tinggi bar health/armor (dipakai juga untuk menjepit posisi cakram)
    float barH = H * 0.009f; if (barH < 5.0f) barH = 5.0f;
    const float barGap = 4.0f;
    float barsBlock = SHOW_BARS ? (8.0f + 2.0f * barH + barGap + 2.0f) : 0.0f;

    // Jepit supaya cakram + bar selalu masuk layar
    if (cy + R + barsBlock > H - EDGE_PAD) cy = H - EDGE_PAD - R - barsBlock;
    if (cy - R < EDGE_PAD) cy = R + EDGE_PAD;
    if (cx - R < EDGE_PAD) cx = R + EDGE_PAD;
    if (cx + R > W - EDGE_PAD) cx = W - EDGE_PAD - R;

    const int N = 72;
    const int RINGS = 6;
    static const float rr[RINGS] = { 0.0f, 0.25f, 0.5f, 0.7f, 0.85f, 1.0f };
    const int NV = RINGS * (N + 1);
    static VtxT v[NV];
    static float PXs[NV], PYs[NV], LXs[NV], LYs[NV];
    static unsigned short idx[(RINGS - 1) * N * 6];

    // 1) Tiap titik cakram di layar (px,py) -> titik di bidang peta miring (satuan D).
    //    Titik yang melewati area sumber valid ditekan ke tepinya.
    float lim = g_m * SRC_CROP;
    for (int r = 0; r < RINGS; r++)
        for (int i = 0; i <= N; i++)
        {
            int n = r * (N + 1) + i;
            float a = (float)i * 2.0f * PI / (float)N;
            float px = cosf(a) * rr[r], py = sinf(a) * rr[r];
            float lx, ly;
            PlaneFromScreen(px, py, lx, ly);
            float rad = sqrtf(lx * lx + ly * ly);
            if (rad > lim) { lx *= lim / rad; ly *= lim / rad; }
            PXs[n] = px; PYs[n] = py; LXs[n] = lx; LYs[n] = ly;
        }

    // simpan parameter proyeksi untuk posisi blip
    g_cx = cx; g_cy = cy; g_R = R;
    g_projValid = true;

    // 2) Vertex: posisi layar bulat (koreksi -0.5 px untuk RHW D3D9), UV dari pemetaan
    //    perspektif. UV ternormalisasi, jadi tidak terpengaruh skala supersample.
    for (int n = 0; n < NV; n++)
    {
        VtxT& p = v[n];
        p.x = cx + R * PXs[n] - 0.5f;
        p.y = cy - R * PYs[n] - 0.5f;
        p.z = 0.0f; p.rhw = 1.0f;
        p.u = (g_srcCx + (LXs[n] / g_m) * g_halfW) / W;
        p.v = (g_srcCy - (LYs[n] / g_m) * g_halfH) / H;
        float t = (PYs[n] + 1.0f) * 0.5f;                         // 0 dekat ... 1 jauh
        BYTE c = (BYTE)(255.0f * (1.0f - FAR_DARKEN * t));
        p.color = D3DCOLOR_ARGB(255, c, c, c);
    }
    int q = 0;
    for (int r = 0; r < RINGS - 1; r++)
        for (int i = 0; i < N; i++)
        {
            unsigned short a = (unsigned short)(r * (N + 1) + i);
            unsigned short b = a + 1;
            unsigned short c = (unsigned short)((r + 1) * (N + 1) + i);
            unsigned short d = c + 1;
            idx[q++] = a; idx[q++] = c; idx[q++] = b;
            idx[q++] = b; idx[q++] = c; idx[q++] = d;
        }

    UseTextured(dev);
    dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, NV, (RINGS - 1) * N * 2,
        idx, D3DFMT_INDEX16, v, sizeof(VtxT));

    // Bingkai bulat gelap (dimatikan secara default: SHOW_FRAME = false)
    if (SHOW_FRAME)
    {
        static VtxC bv[2 * (N + 1)];
        for (int i = 0; i <= N; i++)
        {
            float a = (float)i * 2.0f * PI / (float)N;
            VtxC& po = bv[i * 2];
            VtxC& pi = bv[i * 2 + 1];
            po.x = cx + R * 1.04f * cosf(a) - 0.5f; po.y = cy - R * 1.04f * sinf(a) - 0.5f;
            pi.x = cx + R * cosf(a) - 0.5f;         pi.y = cy - R * sinf(a) - 0.5f;
            po.z = pi.z = 0.0f; po.rhw = pi.rhw = 1.0f;
            po.color = pi.color = 0xF0202020;
        }
        UseSolid(dev);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2 * N, bv, sizeof(VtxC));
    }

    // Bar health & armor di bawah radar (posisi sudah dijamin masuk layar oleh penjepitan di atas)
    if (SHOW_BARS)
    {
        UseSolid(dev);
        float armorY = cy + R + 8.0f;
        float healthY = armorY + barH + barGap;
        float barW = 2.0f * R;
        float hp = 100.0f, ap = 0.0f;
        if (ReadPlayerStats(hp, ap))
        {
            DrawBar(dev, cx - R, armorY, barW, barH, ap / 100.0f, 0xFF4DA3FF);
            DrawBar(dev, cx - R, healthY, barW, barH, hp / 100.0f, 0xFF3DDC84);
        }
    }

    sb->Apply();
    sb->Release();
}

// ------------------------- FASE TEXTURE / LAYAR ----------------------
static bool BeginTexturePhase(IDirect3DDevice9* dev)
{
    g_oldRT = nullptr; g_oldDS = nullptr;
    dev->GetRenderTarget(0, &g_oldRT);
    if (!g_oldRT) return false;
    dev->GetDepthStencilSurface(&g_oldDS);   // boleh null
    dev->GetViewport(&g_oldVP);

    dev->SetRenderTarget(0, g_texSurf);
    dev->SetDepthStencilSurface(g_depth);
    D3DVIEWPORT9 vp = { 0, 0, g_tw, g_th, 0.0f, 1.0f };
    dev->SetViewport(&vp);
    dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_ARGB(255, 0, 0, 0), 1.0f, 0);
    g_inTex = true;
    return true;
}

static void EndTexturePhase(IDirect3DDevice9* dev)
{
    dev->SetRenderTarget(0, g_oldRT);
    dev->SetDepthStencilSurface(g_oldDS);
    dev->SetViewport(&g_oldVP);
    if (g_oldRT) { g_oldRT->Release(); g_oldRT = nullptr; }
    if (g_oldDS) { g_oldDS->Release(); g_oldDS = nullptr; }
    g_inTex = false;
}

// Dipanggil tepat sebelum blip pertama (atau di akhir DrawRadar bila tidak ada blip):
// kembali ke layar, gambar peta miring, lalu batasi gambar sisa radar ke area radar.
static void EnterScreenPhase()
{
    if (!g_inTex || !g_dev) return;
    EndTexturePhase(g_dev);
    DrawRadar3D(g_dev);

    // Scissor: elemen bawaan yang masih lolos di posisi radar sumber terpotong.
    RECT rc;
    rc.left = (LONG)(g_cx - g_R - CLIP_MARGIN);
    rc.right = (LONG)(g_cx + g_R + CLIP_MARGIN);
    rc.top = (LONG)(g_cy - g_R - CLIP_MARGIN);
    rc.bottom = (LONG)(g_cy + g_R + CLIP_MARGIN);
    if (rc.left < 0) rc.left = 0;
    if (rc.top < 0) rc.top = 0;
    if (rc.right > (LONG)g_w) rc.right = (LONG)g_w;
    if (rc.bottom > (LONG)g_h) rc.bottom = (LONG)g_h;
    if (rc.right > rc.left && rc.bottom > rc.top)
    {
        g_dev->SetScissorRect(&rc);
        g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
        g_scissorOn = true;
    }

    BeginIconScale();   // mulai sini semua icon/blip yang digambar game mengecil
}

// ------------------------- HOOK: CHud::DrawRadar ---------------------
static void __cdecl hkDrawRadar()
{
    if (!g_hooksOk) { oDrawRadar(); return; }

    // Simpan layout asli SEKALI, sebelum kita mengubah memori apa pun
    CaptureLayout();

    // Ukur radar bawaan SEKALI, sebelum kita mengubah memori apa pun
    if (!g_defMeasured)
    {
        if (MeasureRadar(g_defCx, g_defCy, g_defHW, g_defHH))
        {
            g_defMeasured = true;
            char buf[160];
            sprintf_s(buf, "Radar bawaan: pusat=(%.1f, %.1f) setengah lebar=%.1f setengah tinggi=%.1f",
                g_defCx, g_defCy, g_defHW, g_defHH);
            Log(buf);
        }
    }

    // g_rangeAddr wajib: tanpa patch jangkauan, bidang peta miring tidak bisa dijamin pas.
    bool active = g_defMeasured && g_dev && g_enabled && g_rangeAddr != 0 && EnsureResources(g_dev);
    ApplyLayout(active);

    if (active)
    {
        // ukur radar sumber (setelah ukurannya kita ubah), dalam piksel layar 1x
        float sw = 0, sh = 0;
        if (MeasureRadar(g_srcCx, g_srcCy, sw, sh))
        {
            g_halfW = sw; g_halfH = sh;
        }
        else active = false;
    }

    // SELURUH radar bawaan (peta, cincin, mask) digambar ke texture
    if (active && !BeginTexturePhase(g_dev)) active = false;

    // Jika gagal di tengah jalan, kembalikan layout asli sebelum radar bawaan digambar
    if (!active) ApplyLayout(false);

    g_inHud = active;
    g_projValid = false;
    g_scissorOn = false;

    if (active)
    {
        UpdateDynamic();       // zoom dinamis
        UpdateProjection();    // sin/cos tilt + g_m
        g_absRange = g_dynZoom * g_m;   // jangkauan radar sumber (unit dunia)
    }
    else g_absRange = 0.0f;
    oDrawRadar();
    EndIconScale();         // pulihkan ukuran layar internal game
    g_absRange = 0.0f;

    // Pulihkan layout asli game: Pause Menu / HUD lain tidak boleh melihat nilai yang diperbesar
    ApplyLayout(false);

    if (g_inTex) EnterScreenPhase();   // tidak ada blip sama sekali: tetap gambar peta
    EndIconScale();         // pengaman: EnterScreenPhase di atas bisa mengaktifkan skala lagi
    if (g_scissorOn && g_dev)
    {
        g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        g_scissorOn = false;
    }

    g_inHud = false;
    g_projValid = false;
}

// ------------------------- HOOK: CRadar::DrawMap ---------------------
// Penanda fase peta.
static void __cdecl hkDrawMap()
{
    g_inMap = true;
    oDrawMap();
    g_inMap = false;
}

// ------------------------- HOOK: posisi blip -------------------------
// 'in' = koordinat ruang radar (-1..1, y atas positif); 1.0 = jangkauan sumber = D * g_m.
// Selama fase texture, koordinat dikalikan g_ss agar peta tergambar beresolusi tinggi.
static void __cdecl hkTransform(Vec2* out, Vec2* in)
{
    oTransform(out, in);
    if (!g_inHud) return;

    if (!g_inMap && g_inTex) EnterScreenPhase();   // blip pertama: pindah ke layar

    if (g_inTex)                                    // fase texture: skala supersample
    {
        out->x *= g_ss;
        out->y *= g_ss;
        return;
    }
    if (g_inMap) return;
    if (!g_projValid) return;

    // ruang radar -> bidang peta (satuan D)
    float LX = in->x * g_m;
    float LY = in->y * g_m;

    // proyeksi maju
    float w = 1.0f + LY * g_sinP;
    if (w < 0.2f) w = 0.2f;
    float px = LX / w;
    float py = LY * g_cosP / w;

    // blip di luar jangkauan menempel di tepi cakram
    float m = sqrtf(px * px + py * py);
    if (m > 0.94f) { px *= 0.94f / m; py *= 0.94f / m; }

    out->x = g_cx + g_R * px;
    out->y = g_cy - g_R * py;
}

// ------------------------- HOOK: EndScene / Reset --------------------
HRESULT __stdcall hkEndScene(IDirect3DDevice9* dev)
{
    g_dev = dev;
    return oEndScene(dev);
}

HRESULT __stdcall hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
    if (g_inTex) EndTexturePhase(dev);
    EndIconScale();
    g_scissorOn = false;
    ReleaseResources();      // wajib dilepas sebelum Reset
    g_projValid = false;
    return oReset(dev, pp);
}

// ------------------------- INISIALISASI ------------------------------
DWORD WINAPI MainThread(LPVOID)
{
    Sleep(5000);
    Log("=== FiveMRadar3D mulai ===");

    // Cek kewajaran memori game (pastikan versi 1.0 US)
    float w = 0.0f;
    bool ok = false;
    for (int i = 0; i < 30 && !ok; i++)
    {
        if (PeekFloat(ADDR_RADAR_H, w) && w > 70.0f && w < 80.0f) ok = true;
        else Sleep(1000);
    }
    if (!ok)
    {
        char buf[512];
        float fx = 0, fy = 0, fw = 0, fh = 0;
        bool bx = PeekFloat(ADDR_RADAR_X, fx), by = PeekFloat(ADDR_RADAR_Y, fy);
        bool bw = PeekFloat(ADDR_RADAR_W, fw), bh = PeekFloat(ADDR_RADAR_H, fh);
        sprintf_s(buf, "DIAG float: X(%d)=%f Y(%d)=%f W(%d)=%f H(%d)=%f",
            bx, fx, by, fy, bw, fw, bh, fh);
        Log(buf);

        DWORD ver = 0;
        __try { ver = *(volatile DWORD*)0x82457C; }
        __except (EXCEPTION_EXECUTE_HANDLER) { ver = 0xDEADBEEF; }
        sprintf_s(buf, "DIAG dword@0x82457C = 0x%08X (1.0 US seharusnya 0x94BF)", ver);
        Log(buf);

        Log("Memori radar tidak wajar: versi gta_sa.exe kemungkinan bukan 1.0 US. ASI tidak aktif.");
        return 0;
    }

    {
        char okbuf[128];
        sprintf_s(okbuf, "Pengecekan memori lolos, tinggi radar = %f", w);
        Log(okbuf);
    }

    // Simpan layout asli radar sekarang juga (sebelum hook/patch apa pun aktif)
    CaptureLayout();

    // Log byte awal tiap fungsi yang di-hook (verifikasi alamat)
    {
        const DWORD addrs[5] = { ADDR_DRAWRADAR, ADDR_DRAWMAP, ADDR_TRANSFORM,
                                 ADDR_WORLD2RADAR, ADDR_RADAR2WORLD };
        const char* names[5] = { "DrawRadar", "DrawMap", "TransformRadarPoint",
                                 "WorldToRadar", "RadarToWorld" };
        for (int a = 0; a < 5; a++)
        {
            unsigned char b[8] = {};
            __try { for (int i = 0; i < 8; i++) b[i] = *(volatile unsigned char*)(addrs[a] + i); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            char buf[160];
            sprintf_s(buf, "DIAG bytes %s@0x%X = %02X %02X %02X %02X %02X %02X %02X %02X",
                names[a], addrs[a], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
            Log(buf);
        }
    }

    // Log nilai layar internal (dipakai BLIP_SCALE)
    {
        int sw = 0, sh = 0;
        __try { sw = *(volatile int*)ADDR_SCREEN_W; sh = *(volatile int*)ADDR_SCREEN_H; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        char buf[160];
        sprintf_s(buf, "DIAG RsGlobal maximumWidth/Height = %d x %d (BLIP_SCALE=%.2f)", sw, sh, BLIP_SCALE);
        Log(buf);
    }

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { Log("Direct3DCreate9 gagal"); return 0; }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = GetForegroundWindow();

    IDirect3DDevice9* dummy = nullptr;
    if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, pp.hDeviceWindow,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dummy)))
    {
        Log("CreateDevice dummy gagal");
        d3d->Release();
        return 0;
    }

    void** vtable = *reinterpret_cast<void***>(dummy);
    void* endSceneAddr = vtable[42];
    void* resetAddr = vtable[16];

    if (MH_Initialize() != MH_OK) { Log("MH_Initialize gagal"); return 0; }

    bool h1 = MH_CreateHook(endSceneAddr, &hkEndScene, reinterpret_cast<void**>(&oEndScene)) == MH_OK;
    bool h2 = MH_CreateHook(resetAddr, &hkReset, reinterpret_cast<void**>(&oReset)) == MH_OK;
    bool h3 = MH_CreateHook(reinterpret_cast<void*>(ADDR_DRAWRADAR), &hkDrawRadar,
        reinterpret_cast<void**>(&oDrawRadar)) == MH_OK;
    bool h4 = MH_CreateHook(reinterpret_cast<void*>(ADDR_DRAWMAP), &hkDrawMap,
        reinterpret_cast<void**>(&oDrawMap)) == MH_OK;
    bool h5 = MH_CreateHook(reinterpret_cast<void*>(ADDR_TRANSFORM), &hkTransform,
        reinterpret_cast<void**>(&oTransform)) == MH_OK;
    // patch jangkauan radar: WAJIB untuk radar 3D (kalau gagal, radar bawaan tetap tampil)
    bool h6 = InstallRangePatch();

    g_hooksOk = h1 && h2 && h3 && h4 && h5;
    {
        char buf[200];
        sprintf_s(buf, "MH_CreateHook: EndScene=%d Reset=%d DrawRadar=%d DrawMap=%d Transform=%d | RangePatch=%d",
            h1, h2, h3, h4, h5, h6);
        Log(buf);
    }

    MH_EnableHook(MH_ALL_HOOKS);
    Log("Hook terpasang");

    dummy->Release();
    d3d->Release();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
    }
    return TRUE;
}
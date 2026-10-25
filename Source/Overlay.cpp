#include "Runtime.h"
#include "PluginAPI.h"
#include <d3d11.h>
#include <dxgi.h>
#include <detours.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <chrono>
#include <cstring>
#include <mutex>

namespace edf5sub {
namespace {
using DeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using SwapFn = HRESULT(WINAPI*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
DeviceFn originalDevice = nullptr;
SwapFn originalSwap = nullptr;
PresentFn originalPresent = nullptr;
ResizeFn originalResize = nullptr;
std::mutex graphicsMutex;
ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;
ID3D11RenderTargetView* renderTarget = nullptr;
IDXGISwapChain* activeSwap = nullptr;
ImGuiContext* overlayContext = nullptr;
ImVector<ImWchar> fontRanges;
bool graphicsFailed = false;
std::vector<SubtitleInstance> active;
auto lastFrame = std::chrono::steady_clock::now();

struct ContextScope {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    bool frameOpen = false;
    ~ContextScope() {
        if (frameOpen) ImGui::EndFrame();
        ImGui::SetCurrentContext(previous);
    }
};

template<class Function>
bool HookVtable(void* object, unsigned slot, Function replacement, Function& original) {
    auto** address = &(*reinterpret_cast<void***>(object))[slot];
    if (*address == reinterpret_cast<void*>(replacement)) return true;
    if (original && *address != reinterpret_cast<void*>(original)) {
        Log("unexpected graphics vtable; hook skipped"); return false;
    }
    DWORD protection;
    if (!VirtualProtect(address, sizeof(void*), PAGE_READWRITE, &protection)) return false;
    original = reinterpret_cast<Function>(*address);
    InterlockedExchangePointer(address, reinterpret_cast<void*>(replacement));
    DWORD unused;
    VirtualProtect(address, sizeof(void*), protection, &unused);
    return true;
}

bool MakeTarget(IDXGISwapChain* swap) {
    ID3D11Texture2D* buffer = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer)))) return false;
    const HRESULT result = device->CreateRenderTargetView(buffer, nullptr, &renderTarget);
    buffer->Release();
    return SUCCEEDED(result);
}

bool InitializeGraphics(IDXGISwapChain* swap) {
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) return false;
    device->GetImmediateContext(&context);
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(swap->GetDesc(&description))) return false;
    if (!MakeTarget(swap)) return false;
    overlayContext = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    const auto text = FontGlyphText();
    builder.AddText(text.c_str());
    builder.AddText(u8"「」『』…");
    builder.BuildRanges(&fontRanges);
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;
    if (!io.Fonts->AddFontFromFileTTF("Mods/Plugins/font.ttf", Settings().fontSize, &fontConfig, fontRanges.Data)) {
        Log("font.ttf could not be loaded; overlay disabled"); return false;
    }
    if (!ImGui_ImplWin32_Init(description.OutputWindow) || !ImGui_ImplDX11_Init(device, context)) return false;
    if (!ImGui_ImplDX11_CreateDeviceObjects()) return false;
    activeSwap = swap;
    lastFrame = std::chrono::steady_clock::now();
    Log("overlay initialized; reference=1920x1080; native layout; external TTF metrics");
    return true;
}

std::size_t Utf16Length(const std::string& text) {
    std::size_t count = 0;
    for (unsigned char c : text) {
        if ((c & 0xc0) == 0x80) continue;
        count += c >= 0xf0 ? 2 : 1;
    }
    return count;
}

void DrawSubtitles(float seconds) {
    TakePending(active);
    const bool paused = IsPaused();
    const auto timing = Settings().timing;
    for (auto& item : active) {
        const bool wasEnded = item.ended;
        item.Advance(seconds, paused, timing, PlaybackState);
        if (!wasEnded && item.ended) LogPlaybackEnd(item);
    }
    active.erase(std::remove_if(active.begin(), active.end(), [&](const auto& item) {
        return item.Finished(timing);
    }), active.end());
    if (active.empty()) return;

    ImFont* font = ImGui::GetFont();
    const float size = Settings().fontSize;
    struct Block { SubtitleInstance* instance; ImVec2 full; float visible, scroll; };
    std::vector<Block> blocks;
    float width = ReferenceLayout::minBlockWidth;
    float height = 0, nextY = 0, backgroundAlpha = 0;
    for (auto& item : active) {
        const ImVec2 full = font->CalcTextSizeA(size, FLT_MAX, ReferenceLayout::wrapWidth, item.caption.c_str());
        const float visible = ReferenceLayout::RowHeight(full.y);
        if (item.y < 0) item.y = nextY;
        if (!paused) item.y += (nextY - item.y) * 0.1f;
        nextY += visible;
        width = std::max(width, full.x);
        height = std::max(height, item.y + visible);
        backgroundAlpha = std::max(backgroundAlpha, item.Alpha(timing));
        blocks.push_back({&item, full, visible,
            ReferenceLayout::Scroll(full.y, Utf16Length(item.caption), item.elapsed)});
    }
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const float left = (ReferenceLayout::width - width) * 0.5f;
    const float top = ReferenceLayout::Top(ReferenceLayout::height);
    const float radius = ReferenceLayout::cornerRadius;
    draw->AddRectFilled(ImVec2(left - radius, top - radius),
        ImVec2(left + width + radius, top + height + radius),
        IM_COL32(0, 0, 0, static_cast<int>(127.5f * backgroundAlpha)), radius);
    for (const auto& block : blocks) {
        const auto& item = *block.instance;
        const float x = (ReferenceLayout::width - block.full.x) * 0.5f;
        const float y = top + item.y;
        draw->PushClipRect(ImVec2(left, y), ImVec2(left + width, y + block.visible), true);
        draw->AddText(font, size, ImVec2(x, y - block.scroll),
            IM_COL32(255, 255, 255, static_cast<int>(255 * item.Alpha(timing))),
            item.caption.c_str(), nullptr, ReferenceLayout::wrapWidth);
        draw->PopClipRect();
    }
}

void RenderFrame(IDXGISwapChain* swap) {
    if (graphicsFailed || (activeSwap && activeSwap != swap)) return;
    ContextScope scope;
    if (!overlayContext) {
        if (!InitializeGraphics(swap)) {
            graphicsFailed = true;
            Log("graphics initialization failed; original game rendering continues");
            return;
        }
    }
    ImGui::SetCurrentContext(overlayContext);
    if (!renderTarget && !MakeTarget(swap)) return;
    ID3D11Texture2D* buffer = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer)))) {
        return;
    }
    D3D11_TEXTURE2D_DESC description{};
    buffer->GetDesc(&description);
    buffer->Release();
    const auto now = std::chrono::steady_clock::now();
    const float seconds = std::chrono::duration<float>(now - lastFrame).count();
    lastFrame = now;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(ReferenceLayout::width, ReferenceLayout::height);
    io.DeltaTime = std::max(seconds, 0.000001f);
    ImGui::NewFrame();
    scope.frameOpen = true;
    DrawSubtitles(seconds);
    ImGui::Render();
    scope.frameOpen = false;
    // The bundled DX11 backend predates framebuffer scaling. Transform its
    // draw data from EDF6 reference space to the actual back-buffer dimensions.
    ImDrawData* data = ImGui::GetDrawData();
    const float sx = description.Width / ReferenceLayout::width;
    const float sy = description.Height / ReferenceLayout::height;
    for (int i = 0; i < data->CmdListsCount; ++i)
        for (auto& vertex : data->CmdLists[i]->VtxBuffer) {
            vertex.pos.x *= sx; vertex.pos.y *= sy;
        }
    data->ScaleClipRects(ImVec2(sx, sy));
    data->DisplaySize = ImVec2(static_cast<float>(description.Width), static_cast<float>(description.Height));
    ID3D11RenderTargetView* previousTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* previousDepth = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, previousTargets, &previousDepth);
    context->OMSetRenderTargets(1, &renderTarget, nullptr);
    ImGui_ImplDX11_RenderDrawData(data);
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, previousTargets, previousDepth);
    for (auto* target : previousTargets) if (target) target->Release();
    if (previousDepth) previousDepth->Release();
}

HRESULT WINAPI HookPresent(IDXGISwapChain* swap, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) {
        std::lock_guard<std::mutex> lock(graphicsMutex);
        try { RenderFrame(swap); }
        catch (...) { graphicsFailed = true; Log("overlay exception; overlay disabled"); }
    }
    return originalPresent(swap, sync, flags);
}

HRESULT WINAPI HookResize(IDXGISwapChain* swap, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    std::lock_guard<std::mutex> lock(graphicsMutex);
    if (swap == activeSwap && renderTarget) { renderTarget->Release(); renderTarget = nullptr; }
    return originalResize(swap, count, width, height, format, flags);
}

HRESULT WINAPI HookSwap(IDXGIFactory* factory, IUnknown* incomingDevice, DXGI_SWAP_CHAIN_DESC* description, IDXGISwapChain** output) {
    const HRESULT result = originalSwap(factory, incomingDevice, description, output);
    if (SUCCEEDED(result) && output && *output) {
        std::lock_guard<std::mutex> lock(graphicsMutex);
        auto** vtable = *reinterpret_cast<void***>(*output);
        void* present = vtable[8];
        void* resize = vtable[13];
        if (present == reinterpret_cast<void*>(HookPresent) && resize == reinterpret_cast<void*>(HookResize)) return result;
        if ((originalPresent && present != reinterpret_cast<void*>(originalPresent)) ||
            (originalResize && resize != reinterpret_cast<void*>(originalResize))) {
            Log("unexpected swapchain vtable; hooks skipped"); return result;
        }
        DWORD protection;
        // Admit both slots together before touching either, including RTV cleanup.
        if (!VirtualProtect(vtable + 8, sizeof(void*) * 6, PAGE_READWRITE, &protection)) {
            Log("swapchain vtable not writable; hooks skipped"); return result;
        }
        originalPresent = reinterpret_cast<PresentFn>(present);
        originalResize = reinterpret_cast<ResizeFn>(resize);
        InterlockedExchangePointer(vtable + 13, reinterpret_cast<void*>(HookResize));
        InterlockedExchangePointer(vtable + 8, reinterpret_cast<void*>(HookPresent));
        DWORD unused;
        VirtualProtect(vtable + 8, sizeof(void*) * 6, protection, &unused);
    }
    return result;
}

HRESULT WINAPI HookDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT levelCount, UINT sdk, ID3D11Device** output,
    D3D_FEATURE_LEVEL* actualLevel, ID3D11DeviceContext** outputContext) {
    const HRESULT result = originalDevice(adapter, type, software, flags, levels, levelCount, sdk, output, actualLevel, outputContext);
    if (FAILED(result) || !output || !*output) return result;
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* parentAdapter = nullptr;
    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED((*output)->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) &&
        SUCCEEDED(dxgi->GetAdapter(&parentAdapter)) &&
        SUCCEEDED(parentAdapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory)))) {
        std::lock_guard<std::mutex> lock(graphicsMutex);
        if (!HookVtable(factory, 10, HookSwap, originalSwap)) Log("factory hook failed");
    }
    if (factory) factory->Release();
    if (parentAdapter) parentAdapter->Release();
    if (dxgi) dxgi->Release();
    return result;
}
} // namespace

#ifdef EDF5_SUBTITLE_PREVIEW
// Offline preview uses the exact draw routine, a software D3D11 device, and
// caller-provided instances. It never installs hooks or opens the game.
void PreviewDraw(std::vector<SubtitleInstance> instances) {
    active = std::move(instances);
    DrawSubtitles(0);
}
#endif

bool StartPlugin() {
    if (!InitializeRuntime()) return false;
    HMODULE d3d = LoadLibraryW(L"d3d11.dll");
    if (!d3d) return false;
    const auto entry = GetProcAddress(d3d, "D3D11CreateDevice");
    static_assert(sizeof entry == sizeof originalDevice);
    std::memcpy(&originalDevice, &entry, sizeof entry);
    if (!originalDevice) return false;
    // Hooks and graphics vtables stay valid until process exit, even if a loader
    // later releases its plugin reference. No initialization is done in DllMain.
    HMODULE pinnedModule = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&StartPlugin), &pinnedModule)) return false;
    if (DetourTransactionBegin() != NO_ERROR) return false;
    if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR) { DetourTransactionAbort(); return false; }
    InstallVoiceHooksInTransaction();
    const LONG attach = DetourAttach(reinterpret_cast<PVOID*>(&originalDevice), reinterpret_cast<PVOID>(HookDevice));
    if (!VoiceHooksReady() || attach != NO_ERROR) { DetourTransactionAbort(); Log("hook attachment aborted"); return false; }
    const LONG result = DetourTransactionCommit();
    Log("hook transaction result=%ld", result);
    return result == NO_ERROR;
}
} // namespace edf5sub

extern "C" __declspec(dllexport) bool __fastcall EML5_Load(PluginInfo* info) {
    try {
        if (!info || !edf5sub::StartPlugin()) return false;
        info->infoVersion = PluginInfo::MaxInfoVer;
        info->name = "EDF5 Subtitles - EDF6 Style";
        info->version = PLUG_VER(1, 2, 0, 1);
        return true;
    } catch (...) {
        edf5sub::Log("plugin initialization failed");
        return false;
    }
}

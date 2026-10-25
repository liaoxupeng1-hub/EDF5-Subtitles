#include "../Source/Runtime.h"
#include "../Source/SubtitleTable.h"
#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <fstream>
#include <iostream>

namespace edf5sub { void PreviewDraw(std::vector<SubtitleInstance> instances); }

int main() {
    using namespace edf5sub;
    SubtitleTable table;
    std::string error;
    if (!table.LoadFile("build/preview/EDF6_VOICETABLE.SUBTITLE.SGO", &error)) {
        std::cerr << error; return 1;
    }
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &context))) return 2;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    std::string text;
    table.ForEachRecord([&](const SubtitleRecord& record) { text += record.caption; });
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault()); builder.AddText(text.c_str());
    builder.AddText(u8"「」『』…");
    ImVector<ImWchar> ranges; builder.BuildRanges(&ranges);
    if (!io.Fonts->AddFontFromFileTTF("build/preview/font.ttf", Settings().fontSize, nullptr, ranges.Data)) return 3;
    if (!ImGui_ImplDX11_Init(device, context)) return 4;
    const struct Scenario { unsigned w,h; const char* name; bool longText; float after; } scenarios[] = {
        {1920,1080,"short-1080",false,0}, {3840,2160,"short-4k",false,0},
        {1920,1080,"long-1080",true,0}, {1920,1080,"fade-1080",false,1.5f},
        {2560,1080,"short-ultrawide",false,0}};
    for (const auto& scenario : scenarios) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=scenario.w; desc.Height=scenario.h; desc.MipLevels=1; desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1;
        desc.Usage=D3D11_USAGE_DEFAULT; desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D* texture=nullptr; ID3D11RenderTargetView* target=nullptr;
        if (FAILED(device->CreateTexture2D(&desc,nullptr,&texture)) ||
            FAILED(device->CreateRenderTargetView(texture,nullptr,&target))) return 5;
        const float background[4]={.12f,.16f,.20f,1};
        context->ClearRenderTargetView(target,background);
        context->OMSetRenderTargets(1,&target,nullptr);
        io.DisplaySize=ImVec2(1920,1080); io.DeltaTime=1.0f/60;
        ImGui_ImplDX11_NewFrame(); ImGui::NewFrame();
        std::vector<SubtitleInstance> instances;
        for (const char* id : scenario.longText ? std::vector<const char*>{"AE103_037_S"} :
                std::vector<const char*>{"AE104_052_S","AE104_053_S"}) {
            const auto* record=table.Find(id);
            if (!record) return 6;
            SubtitleInstance item; item.voiceId=id; item.caption=DisplayCaption(record->caption, false);
            item.ended=true; item.afterVoice=scenario.after; item.elapsed=scenario.longText ? 25 : 0;
            instances.push_back(std::move(item));
        }
        PreviewDraw(std::move(instances));
        ImGui::Render(); auto* draw=ImGui::GetDrawData();
        const float sx=scenario.w/1920.f, sy=scenario.h/1080.f;
        for (int i=0;i<draw->CmdListsCount;++i)
            for(auto& vertex:draw->CmdLists[i]->VtxBuffer){vertex.pos.x*=sx;vertex.pos.y*=sy;}
        draw->ScaleClipRects(ImVec2(sx,sy)); draw->DisplaySize=ImVec2(float(scenario.w),float(scenario.h));
        ImGui_ImplDX11_RenderDrawData(draw);
        context->OMSetRenderTargets(0,nullptr,nullptr);
        desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* staging=nullptr;
        if (FAILED(device->CreateTexture2D(&desc,nullptr,&staging))) return 7;
        context->CopyResource(staging,texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging,0,D3D11_MAP_READ,0,&mapped))) return 8;
        std::ofstream output(std::string("build/preview/")+scenario.name+".rgba",std::ios::binary);
        for(unsigned y=0;y<scenario.h;++y)
            output.write(static_cast<const char*>(mapped.pData)+y*mapped.RowPitch,scenario.w*4);
        output.close(); context->Unmap(staging,0);
        staging->Release();target->Release();texture->Release();
        std::cout << "Rendered " << scenario.name << " " << scenario.w << "x" << scenario.h << '\n';
    }
    ImGui_ImplDX11_Shutdown(); ImGui::DestroyContext();context->Release();device->Release();
    return 0;
}

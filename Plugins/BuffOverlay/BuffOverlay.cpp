#include "../../POEFixer/plugin_sdk/PluginSDK.h"
#include "../../POEFixer/imgui/imgui.h"

#include <Windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")

using Microsoft::WRL::ComPtr;

namespace {

enum class MatchMode : int {
    Exact = 0,
    Prefix = 1,
    Contains = 2,
};

enum class DisplayMode : int {
    Charges = 0,
    CustomText = 1,
    Icon = 2,
    IconAndCharges = 3,
    IconAndText = 4,
};

enum class MissingMode : int {
    Hide = 0,
    ShowZero = 1,
};

struct TextureData {
    ComPtr<ID3D11ShaderResourceView> srv;
    int width = 0;
    int height = 0;
    std::string loadedPath;
};

struct Tracker {
    bool enabled = true;
    char label[64] = "Tracker";
    char pattern[160] = "";
    int matchMode = static_cast<int>(MatchMode::Prefix);
    int displayMode = static_cast<int>(DisplayMode::Charges);
    int missingMode = static_cast<int>(MissingMode::Hide);
    char customText[128] = "BUFF";
    char iconPath[520] = "";
    float x = 500.0f;
    float y = 300.0f;
    float fontScale = 2.0f;
    float iconSize = 42.0f;
    bool sumCharges = true;
    ImVec4 color = ImVec4(1, 1, 1, 1);
    TextureData texture;
};

static std::string TrimCopy(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

static bool Matches(const std::string& name, const char* pattern, MatchMode mode) {
    if (!pattern || pattern[0] == '\0') return false;
    const std::string p(pattern);
    switch (mode) {
        case MatchMode::Exact:
            return name == p;
        case MatchMode::Prefix:
            return name.rfind(p, 0) == 0;
        case MatchMode::Contains:
            return name.find(p) != std::string::npos;
        default:
            return false;
    }
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (count <= 0) return {};
    std::wstring out(static_cast<size_t>(count - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), count);
    return out;
}

static std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string out(static_cast<size_t>(count - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), count, nullptr, nullptr);
    return out;
}

static bool PickImageFile(std::string& outPath) {
    std::array<wchar_t, 1024> buffer{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrFilter = L"Image files\0*.png;*.jpg;*.jpeg;*.bmp\0PNG\0*.png\0JPEG\0*.jpg;*.jpeg\0Bitmap\0*.bmp\0All files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    outPath = WideToUtf8(buffer.data());
    return !outPath.empty();
}

static bool LoadTextureWic(ID3D11Device* device, const std::string& path, TextureData& out) {
    if (!device || path.empty()) return false;

    HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninit = SUCCEEDED(initHr);

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        if (uninit) CoUninitialize();
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    const std::wstring widePath = Utf8ToWide(path);
    hr = factory->CreateDecoderFromFilename(widePath.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
    if (FAILED(hr)) {
        if (uninit) CoUninitialize();
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr)) {
        if (uninit) CoUninitialize();
        return false;
    }

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0) {
        if (uninit) CoUninitialize();
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) {
        if (uninit) CoUninitialize();
        return false;
    }

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                               WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        if (uninit) CoUninitialize();
        return false;
    }

    const UINT stride = w * 4;
    std::vector<unsigned char> pixels(static_cast<size_t>(stride) * h);
    hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(hr)) {
        if (uninit) CoUninitialize();
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = pixels.data();
    init.SysMemPitch = stride;

    ComPtr<ID3D11Texture2D> texture;
    hr = device->CreateTexture2D(&desc, &init, texture.GetAddressOf());
    if (FAILED(hr)) {
        if (uninit) CoUninitialize();
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(texture.Get(), &srvDesc, srv.GetAddressOf());
    if (FAILED(hr)) {
        if (uninit) CoUninitialize();
        return false;
    }

    out.srv = srv;
    out.width = static_cast<int>(w);
    out.height = static_cast<int>(h);
    out.loadedPath = path;

    if (uninit) CoUninitialize();
    return true;
}

static void CopyToBuffer(char* dst, size_t dstSize, const std::string& src) {
    if (!dst || dstSize == 0) return;
    std::snprintf(dst, dstSize, "%s", src.c_str());
}

static std::string SanitizeValue(std::string s) {
    std::replace(s.begin(), s.end(), '\n', ' ');
    std::replace(s.begin(), s.end(), '\r', ' ');
    return s;
}

} // namespace

class BuffOverlayPlugin final : public PluginSDK::Plugin {
public:
    const char* GetName() const override { return "Buff Overlay"; }

    void OnEnable(bool /*isGameAttached*/) override {
        if (ctx()->ImGuiContext)
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));
        LoadSettings();
        if (m_trackers.empty()) AddDefaultTracker();
        ctx()->Log.Info("BuffOverlay enabled");
    }

    void OnDisable() override {
        SaveSettings();
        for (auto& t : m_trackers) t.texture = {};
        ctx()->Log.Info("BuffOverlay disabled");
    }

    bool WantsOverlay() const override { return true; }

    void DrawSettings() override {
        if (ctx()->ImGuiContext)
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));

        ImGui::Checkbox("Preview / drag mode", &m_previewMode);
        ImGui::SameLine();
        ImGui::TextDisabled("(lets you drag trackers even if the buff is missing)");
        ImGui::Checkbox("Show debug matches", &m_showDebug);
        ImGui::Separator();

        if (ImGui::Button("Add tracker")) {
            Tracker t;
            std::snprintf(t.label, sizeof(t.label), "Tracker %d", static_cast<int>(m_trackers.size() + 1));
            t.x += static_cast<float>(m_trackers.size() * 20);
            t.y += static_cast<float>(m_trackers.size() * 20);
            m_trackers.push_back(std::move(t));
        }
        ImGui::SameLine();
        if (ImGui::Button("Save settings")) SaveSettings();

        int removeIndex = -1;
        for (size_t i = 0; i < m_trackers.size(); ++i) {
            Tracker& t = m_trackers[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::Separator();

            std::string header = std::string(t.label[0] ? t.label : "Tracker") + "##tracker";
            if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Enabled", &t.enabled);
                ImGui::InputText("Label", t.label, sizeof(t.label));
                ImGui::InputText("Buff name / pattern", t.pattern, sizeof(t.pattern));

                const char* matchItems[] = { "Exact", "Starts with", "Contains" };
                ImGui::Combo("Match mode", &t.matchMode, matchItems, IM_ARRAYSIZE(matchItems));
                ImGui::TextDisabled("For names with a changing numeric suffix, use Starts with and omit the number.");

                const char* displayItems[] = {
                    "Charges", "Custom text", "Icon", "Icon + charges", "Icon + custom text"
                };
                ImGui::Combo("Display", &t.displayMode, displayItems, IM_ARRAYSIZE(displayItems));

                const char* missingItems[] = { "Hide", "Show 0" };
                ImGui::Combo("If buff is missing", &t.missingMode, missingItems, IM_ARRAYSIZE(missingItems));
                ImGui::Checkbox("Sum charges of all matching buffs", &t.sumCharges);

                if (t.displayMode == static_cast<int>(DisplayMode::CustomText) ||
                    t.displayMode == static_cast<int>(DisplayMode::IconAndText)) {
                    ImGui::InputText("Custom text", t.customText, sizeof(t.customText));
                }

                if (t.displayMode == static_cast<int>(DisplayMode::Icon) ||
                    t.displayMode == static_cast<int>(DisplayMode::IconAndCharges) ||
                    t.displayMode == static_cast<int>(DisplayMode::IconAndText)) {
                    ImGui::InputText("Icon path", t.iconPath, sizeof(t.iconPath));
                    ImGui::SameLine();
                    if (ImGui::Button("Browse...")) {
                        std::string selected;
                        if (PickImageFile(selected)) {
                            CopyToBuffer(t.iconPath, sizeof(t.iconPath), selected);
                            t.texture = {};
                        }
                    }
                    ImGui::SliderFloat("Icon size", &t.iconSize, 12.0f, 128.0f, "%.0f px");
                }

                ImGui::DragFloat2("Position (X/Y)", &t.x, 1.0f, 0.0f, 10000.0f, "%.0f");
                ImGui::SliderFloat("Text scale", &t.fontScale, 0.5f, 5.0f, "%.2f");
                ImGui::ColorEdit4("Text color", &t.color.x,
                                  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

                if (ImGui::Button("Move to screen center")) {
                    const ImGuiIO& io = ImGui::GetIO();
                    t.x = io.DisplaySize.x * 0.5f;
                    t.y = io.DisplaySize.y * 0.5f;
                }
                ImGui::SameLine();
                if (ImGui::Button("Remove")) removeIndex = static_cast<int>(i);
            }
            ImGui::PopID();
        }

        if (removeIndex >= 0 && removeIndex < static_cast<int>(m_trackers.size()))
            m_trackers.erase(m_trackers.begin() + removeIndex);
    }

    void DrawUI() override {
        if (!ctx()->ImGuiContext) return;
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));

        const PluginSDK::Snapshot snapshot = ctx()->Game.GetSnapshot();
        if (!snapshot.IsAttached || snapshot.State != PluginSDK::GameState::InGame) return;

        // Preview/drag mode must still work when the player currently has no Buffs
        // component / no active buffs. In that case we simply render against an
        // empty list instead of returning early.
        std::vector<PluginSDK::Buff> buffs;
        if (snapshot.Player.Components.HasBuffs()) {
            buffs = ctx()->Components.EnumerateAggregatedBuffs(snapshot.Player.Components.Buffs);
            if (buffs.empty()) {
                // Backward-compatible fallback if the host doesn't expose aggregated buffs.
                buffs = ctx()->Components.EnumerateBuffs(snapshot.Player.Components.Buffs);
            }
        }

        if (m_showDebug) DrawDebugWindow(buffs);

        for (size_t i = 0; i < m_trackers.size(); ++i) {
            Tracker& tracker = m_trackers[i];
            if (!tracker.enabled) continue;
            DrawTracker(i, tracker, buffs);
        }
    }

    void SaveSettings() override {
        namespace fs = std::filesystem;
        const fs::path dir = DirectoryPath() / "config";
        std::error_code ec;
        fs::create_directories(dir, ec);

        std::ofstream out(dir / "settings.txt", std::ios::trunc);
        if (!out.is_open()) return;

        out << "Preview=" << (m_previewMode ? 1 : 0) << '\n';
        out << "Debug=" << (m_showDebug ? 1 : 0) << '\n';
        out << "TrackerCount=" << m_trackers.size() << '\n';

        for (size_t i = 0; i < m_trackers.size(); ++i) {
            const Tracker& t = m_trackers[i];
            const std::string p = "T" + std::to_string(i) + ".";
            out << p << "Enabled=" << (t.enabled ? 1 : 0) << '\n';
            out << p << "Label=" << SanitizeValue(t.label) << '\n';
            out << p << "Pattern=" << SanitizeValue(t.pattern) << '\n';
            out << p << "MatchMode=" << t.matchMode << '\n';
            out << p << "DisplayMode=" << t.displayMode << '\n';
            out << p << "MissingMode=" << t.missingMode << '\n';
            out << p << "CustomText=" << SanitizeValue(t.customText) << '\n';
            out << p << "IconPath=" << SanitizeValue(t.iconPath) << '\n';
            out << p << "X=" << t.x << '\n';
            out << p << "Y=" << t.y << '\n';
            out << p << "FontScale=" << t.fontScale << '\n';
            out << p << "IconSize=" << t.iconSize << '\n';
            out << p << "SumCharges=" << (t.sumCharges ? 1 : 0) << '\n';
            out << p << "Color=" << t.color.x << ',' << t.color.y << ',' << t.color.z << ',' << t.color.w << '\n';
        }
    }

private:
    void AddDefaultTracker() {
        Tracker t;
        CopyToBuffer(t.label, sizeof(t.label), "Ancestral Bond charges");
        CopyToBuffer(t.pattern, sizeof(t.pattern), "totem_ancestral_bond_reservation_");
        t.matchMode = static_cast<int>(MatchMode::Prefix);
        t.displayMode = static_cast<int>(DisplayMode::Charges);
        t.missingMode = static_cast<int>(MissingMode::ShowZero);
        t.x = 800.0f;
        t.y = 500.0f;
        t.fontScale = 2.5f;
        m_trackers.push_back(std::move(t));
    }

    void LoadSettings() {
        namespace fs = std::filesystem;
        const fs::path path = DirectoryPath() / "config" / "settings.txt";
        if (!fs::exists(path)) return;

        std::ifstream in(path);
        if (!in.is_open()) return;

        std::vector<std::pair<std::string, std::string>> kv;
        size_t trackerCount = 0;
        std::string line;
        while (std::getline(in, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = TrimCopy(line.substr(0, eq));
            std::string val = line.substr(eq + 1);
            kv.emplace_back(key, val);
            if (key == "TrackerCount") {
                try { trackerCount = static_cast<size_t>(std::stoul(val)); } catch (...) {}
            }
        }

        m_trackers.clear();
        m_trackers.resize(std::min<size_t>(trackerCount, 64));

        auto getTracker = [&](const std::string& key, size_t& idx, std::string& field) -> Tracker* {
            if (key.size() < 4 || key[0] != 'T') return nullptr;
            const size_t dot = key.find('.');
            if (dot == std::string::npos) return nullptr;
            try { idx = static_cast<size_t>(std::stoul(key.substr(1, dot - 1))); }
            catch (...) { return nullptr; }
            if (idx >= m_trackers.size()) return nullptr;
            field = key.substr(dot + 1);
            return &m_trackers[idx];
        };

        for (const auto& [key, val] : kv) {
            if (key == "Preview") { m_previewMode = (val == "1"); continue; }
            if (key == "Debug") { m_showDebug = (val == "1"); continue; }
            if (key == "TrackerCount") continue;

            size_t idx = 0;
            std::string field;
            Tracker* t = getTracker(key, idx, field);
            if (!t) continue;

            try {
                if (field == "Enabled") t->enabled = (val == "1");
                else if (field == "Label") CopyToBuffer(t->label, sizeof(t->label), val);
                else if (field == "Pattern") CopyToBuffer(t->pattern, sizeof(t->pattern), val);
                else if (field == "MatchMode") t->matchMode = std::stoi(val);
                else if (field == "DisplayMode") t->displayMode = std::stoi(val);
                else if (field == "MissingMode") t->missingMode = std::stoi(val);
                else if (field == "CustomText") CopyToBuffer(t->customText, sizeof(t->customText), val);
                else if (field == "IconPath") CopyToBuffer(t->iconPath, sizeof(t->iconPath), val);
                else if (field == "X") t->x = std::stof(val);
                else if (field == "Y") t->y = std::stof(val);
                else if (field == "FontScale") t->fontScale = std::stof(val);
                else if (field == "IconSize") t->iconSize = std::stof(val);
                else if (field == "SumCharges") t->sumCharges = (val == "1");
                else if (field == "Color") {
                    std::stringstream ss(val);
                    std::string part;
                    float c[4] = {1,1,1,1};
                    for (int n = 0; n < 4 && std::getline(ss, part, ','); ++n) c[n] = std::stof(part);
                    t->color = ImVec4(c[0], c[1], c[2], c[3]);
                }
            } catch (...) {
                // Keep defaults for malformed values.
            }
        }
    }

    void DrawDebugWindow(const std::vector<PluginSDK::Buff>& buffs) {
        ImGui::SetNextWindowSize(ImVec2(620, 320), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Buff Overlay - Debug##BuffOverlayDebug", &m_showDebug)) {
            ImGui::End();
            return;
        }
        ImGui::Text("Active buffs: %d", static_cast<int>(buffs.size()));
        if (ImGui::BeginTable("##buffdebug", 3,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                              ImVec2(0, 250))) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Charges", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Time left", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableHeadersRow();
            for (const auto& b : buffs) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(b.Name.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", static_cast<int>(b.Charges));
                ImGui::TableNextColumn(); ImGui::Text("%.1f", b.TimeLeft);
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }

    void DrawTracker(size_t index, Tracker& t, const std::vector<PluginSDK::Buff>& buffs) {
        bool found = false;
        int charges = 0;

        for (const auto& b : buffs) {
            if (!Matches(b.Name, t.pattern, static_cast<MatchMode>(t.matchMode))) continue;
            if (!found) {
                charges = static_cast<int>(b.Charges);
                found = true;
            } else if (t.sumCharges) {
                charges += static_cast<int>(b.Charges);
            }
            if (!t.sumCharges) break;
        }

        if (!found && !m_previewMode && t.missingMode == static_cast<int>(MissingMode::Hide)) return;
        if (!found) charges = 0;

        const bool wantsIcon =
            t.displayMode == static_cast<int>(DisplayMode::Icon) ||
            t.displayMode == static_cast<int>(DisplayMode::IconAndCharges) ||
            t.displayMode == static_cast<int>(DisplayMode::IconAndText);

        if (wantsIcon && t.iconPath[0] != '\0') {
            const std::string path(t.iconPath);
            if (!t.texture.srv || t.texture.loadedPath != path) {
                t.texture = {};
                LoadTextureWic(static_cast<ID3D11Device*>(ctx()->D3DDevice), path, t.texture);
            }
        }

        if (m_previewMode) {
            DrawDraggableTracker(index, t, charges, found, wantsIcon);
        } else {
            DrawFixedTracker(t, charges, wantsIcon);
        }
    }

    std::string BuildText(const Tracker& t, int charges) const {
        switch (static_cast<DisplayMode>(t.displayMode)) {
            case DisplayMode::Charges:
            case DisplayMode::IconAndCharges:
                return std::to_string(charges);
            case DisplayMode::CustomText:
            case DisplayMode::IconAndText:
                return t.customText;
            case DisplayMode::Icon:
            default:
                return {};
        }
    }

    void DrawFixedTracker(const Tracker& t, int charges, bool wantsIcon) {
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        ImVec2 pos(t.x, t.y);

        if (wantsIcon && t.texture.srv) {
            const ImTextureID texId = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(t.texture.srv.Get()));
            draw->AddImage(ImTextureRef(texId), pos, ImVec2(pos.x + t.iconSize, pos.y + t.iconSize));
            pos.x += t.iconSize + 6.0f;
        }

        const std::string text = BuildText(t, charges);
        if (!text.empty()) {
            ImFont* font = ImGui::GetFont();
            const float size = ImGui::GetFontSize() * t.fontScale;
            const ImU32 col = ImGui::ColorConvertFloat4ToU32(t.color);
            draw->AddText(font, size, pos, col, text.c_str());
        }
    }

    void DrawDraggableTracker(size_t index, Tracker& t, int charges, bool found, bool wantsIcon) {
        const std::string text = BuildText(t, charges);
        const float textWidth = text.empty() ? 0.0f : ImGui::CalcTextSize(text.c_str()).x * t.fontScale;
        const float textHeight = text.empty() ? 0.0f : ImGui::GetFontSize() * t.fontScale;
        const float width = (wantsIcon ? t.iconSize + 6.0f : 0.0f) + textWidth + 12.0f;
        const float height = (std::max)(wantsIcon ? t.iconSize : 0.0f, textHeight) + 10.0f;
        const float windowWidth = (std::max)(width, 80.0f);
        const float windowHeight = (std::max)(height, 34.0f);

        ImGuiIO& io = ImGui::GetIO();

        // Do not rely on ImGui's built-in window dragging here. POEFixer/ImGui can
        // be configured to only move windows by their title bar, while these
        // tracker windows intentionally have no title bar. We therefore perform
        // the drag ourselves using the mouse position.
        const ImVec2 rectMin(t.x, t.y);
        const ImVec2 rectMax(t.x + windowWidth, t.y + windowHeight);
        const bool hovered = ImGui::IsMouseHoveringRect(rectMin, rectMax, false);

        if (m_draggingTracker < 0 && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            m_draggingTracker = static_cast<int>(index);
            m_dragOffset = ImVec2(io.MousePos.x - t.x, io.MousePos.y - t.y);
        }

        if (m_draggingTracker == static_cast<int>(index)) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                t.x = io.MousePos.x - m_dragOffset.x;
                t.y = io.MousePos.y - m_dragOffset.y;

                // Keep at least part of the tracker on screen.
                const float maxX = (std::max)(0.0f, io.DisplaySize.x - windowWidth);
                const float maxY = (std::max)(0.0f, io.DisplaySize.y - windowHeight);
                t.x = std::clamp(t.x, 0.0f, maxX);
                t.y = std::clamp(t.y, 0.0f, maxY);
            } else {
                m_draggingTracker = -1;
            }
        }

        ImGui::SetNextWindowPos(ImVec2(t.x, t.y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha((hovered || m_draggingTracker == static_cast<int>(index)) ? 0.40f : 0.25f);

        const std::string windowName = "##BuffOverlayDrag" + std::to_string(index);
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus;

        if (ImGui::Begin(windowName.c_str(), nullptr, flags)) {
            ImGui::SetCursorPos(ImVec2(5, 5));

            if (wantsIcon && t.texture.srv) {
                const ImTextureID texId = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(t.texture.srv.Get()));
                ImGui::Image(ImTextureRef(texId), ImVec2(t.iconSize, t.iconSize));
                if (!text.empty()) ImGui::SameLine();
            }

            if (!text.empty()) {
                ImGui::SetWindowFontScale(t.fontScale);
                ImGui::TextColored(t.color, "%s", text.c_str());
                ImGui::SetWindowFontScale(1.0f);
            }

            if (!found) {
                ImGui::SetWindowFontScale(1.0f);
                ImGui::TextDisabled("[preview]");
            }

            // Small visual hint that this window can be dragged.
            if (hovered || m_draggingTracker == static_cast<int>(index)) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 wp = ImGui::GetWindowPos();
                const ImVec2 ws = ImGui::GetWindowSize();
                dl->AddRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                            IM_COL32(255, 255, 255, 180), 0.0f, 0, 1.0f);
            }
        }
        ImGui::End();
    }

    std::vector<Tracker> m_trackers;
    bool m_previewMode = false;
    bool m_showDebug = false;
    int m_draggingTracker = -1;
    ImVec2 m_dragOffset = ImVec2(0.0f, 0.0f);
};

extern "C" PLUGIN_API PluginSDK::Plugin* CreatePlugin() {
    return new BuffOverlayPlugin();
}

extern "C" PLUGIN_API void DestroyPlugin(PluginSDK::Plugin* plugin) {
    delete plugin;
}

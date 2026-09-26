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
#include <climits>
#include <cmath>
#include <cstdio>
#include <iomanip>
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

enum class SourceMode : int {
    Buff = 0,
    ActiveSkill = 1,
};

enum class StageCompare : int {
    Any = 0,
    Equal = 1,
    NotEqual = 2,
    Greater = 3,
    GreaterEqual = 4,
    Less = 5,
    LessEqual = 6,
};

enum class ActiveSkillValue : int {
    UseStage = 0,
    TotalUses = 1,
    AvailableCharges = 2,
    MaxUses = 3,
    CanBeUsed = 4,
};

enum class DisplayMode : int {
    // Keep the original values 0..4 unchanged for settings compatibility.
    Charges = 0,
    CustomText = 1,
    Icon = 2,
    IconAndCharges = 3,
    IconAndText = 4,

    // Buff-only Time Left display modes.
    TimeLeft = 5,
    ChargesAndTimeLeft = 6,
    CustomTextAndTimeLeft = 7,
    IconAndTimeLeft = 8,
    IconChargesAndTimeLeft = 9,
    IconTextAndTimeLeft = 10,
};

enum class MissingMode : int {
    Hide = 0,
    ShowZero = 1,
};

struct TextureData {
    ComPtr<ID3D11ShaderResourceView> srv;
    int width = 0;
    int height = 0;
    // Path for which the most recent load attempt was made. This is also set
    // on failure so we do not retry a broken path on every frame.
    std::string loadedPath;
    std::string lastError;
};

struct Tracker {
    bool enabled = true;
    char label[64] = "Tracker";
    char pattern[160] = "";
    int sourceMode = static_cast<int>(SourceMode::Buff);
    int matchMode = static_cast<int>(MatchMode::Prefix);
    int activeSkillValue = static_cast<int>(ActiveSkillValue::UseStage);
    int stageCompare = static_cast<int>(StageCompare::Any);
    int stageValue = 0;
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

    // Optional rectangle drawn only behind the text portion of the tracker.
    bool textBackground = false;
    ImVec4 backgroundColor = ImVec4(0.0f, 0.0f, 0.0f, 0.70f);

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

static bool StageMatches(int actual, StageCompare mode, int expected) {
    switch (mode) {
        case StageCompare::Any:          return true;
        case StageCompare::Equal:        return actual == expected;
        case StageCompare::NotEqual:     return actual != expected;
        case StageCompare::Greater:      return actual > expected;
        case StageCompare::GreaterEqual: return actual >= expected;
        case StageCompare::Less:         return actual < expected;
        case StageCompare::LessEqual:    return actual <= expected;
        default:                         return true;
    }
}

static int GetActiveSkillValue(const PluginSDK::ActiveSkill& skill, ActiveSkillValue mode) {
    switch (mode) {
        case ActiveSkillValue::UseStage:
            return skill.UseStage;

        case ActiveSkillValue::TotalUses:
            return skill.TotalUses;

        case ActiveSkillValue::AvailableCharges:
            return (std::max)(0, skill.MaxUses - skill.TotalActiveCooldowns);

        case ActiveSkillValue::MaxUses:
            return skill.MaxUses;

        case ActiveSkillValue::CanBeUsed:
            return skill.CanBeUsed ? 1 : 0;

        default:
            return skill.UseStage;
    }
}

static const char* ActiveSkillValueLabel(ActiveSkillValue mode) {
    switch (mode) {
        case ActiveSkillValue::UseStage:         return "Use Stage";
        case ActiveSkillValue::TotalUses:        return "Total Uses";
        case ActiveSkillValue::AvailableCharges: return "Available Charges";
        case ActiveSkillValue::MaxUses:          return "Max Uses";
        case ActiveSkillValue::CanBeUsed:        return "Can Be Used";
        default:                                  return "Value";
    }
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};

    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
        nullptr, 0);
    if (count <= 0) return {};

    std::wstring out(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
            out.data(), count) <= 0) {
        return {};
    }
    return out;
}

static std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};

    const int count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};

    std::string out(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
            out.data(), count, nullptr, nullptr) <= 0) {
        return {};
    }
    return out;
}

static std::string NormalizeIconPath(std::string path) {
    path = TrimCopy(std::move(path));

    // Paths copied from Explorer/Discord/etc. are often surrounded by quotes.
    // WIC treats the quotes as literal filename characters and then fails to
    // open the file, so strip matching outer quotes.
    while (path.size() >= 2) {
        const char first = path.front();
        const char last = path.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            path = TrimCopy(path.substr(1, path.size() - 2));
        } else {
            break;
        }
    }

    return path;
}

static bool PickImageFile(HWND owner, std::string& outPath) {
    std::array<wchar_t, 32768> buffer{};

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrFilter =
        L"Image files (*.png;*.jpg;*.jpeg;*.bmp)\0"
        L"*.png;*.jpg;*.jpeg;*.bmp\0"
        L"PNG (*.png)\0*.png\0"
        L"JPEG (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0"
        L"Bitmap (*.bmp)\0*.bmp\0"
        L"All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) return false;

    outPath = NormalizeIconPath(WideToUtf8(std::wstring(buffer.data())));
    return !outPath.empty();
}

static std::string HrText(HRESULT hr) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

static bool LoadTextureWic(ID3D11Device* device, const std::string& rawPath, TextureData& out) {
    const std::string path = NormalizeIconPath(rawPath);

    out.srv.Reset();
    out.width = 0;
    out.height = 0;
    out.loadedPath = path;
    out.lastError.clear();

    if (!device) {
        out.lastError = "D3D11 device is unavailable";
        return false;
    }
    if (path.empty()) {
        out.lastError = "Icon path is empty";
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(Utf8ToWide(path)), ec)) {
        out.lastError = "File does not exist";
        return false;
    }

    // The host may already have COM initialized in STA or MTA mode.
    // RPC_E_CHANGED_MODE is not fatal here: COM is still initialized and WIC
    // can be used on the current thread.
    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(initHr);
    if (FAILED(initHr) && initHr != RPC_E_CHANGED_MODE) {
        out.lastError = "CoInitializeEx failed: " + HrText(initHr);
        return false;
    }

    auto finish = [&]() {
        if (shouldUninitialize) CoUninitialize();
    };

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        out.lastError = "WIC factory failed: " + HrText(hr);
        finish();
        return false;
    }

    const std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty()) {
        out.lastError = "Could not convert icon path to UTF-16";
        finish();
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(
        widePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
        decoder.GetAddressOf());
    if (FAILED(hr)) {
        out.lastError = "Could not open image: " + HrText(hr);
        finish();
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr)) {
        out.lastError = "Could not decode image frame: " + HrText(hr);
        finish();
        return false;
    }

    UINT w = 0, h = 0;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr) || w == 0 || h == 0) {
        out.lastError = "Invalid image dimensions";
        finish();
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.GetAddressOf());
    if (FAILED(hr)) {
        out.lastError = "WIC format converter failed: " + HrText(hr);
        finish();
        return false;
    }

    hr = converter->Initialize(
        frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
        nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) {
        out.lastError = "Image conversion to RGBA failed: " + HrText(hr);
        finish();
        return false;
    }

    const UINT stride = w * 4;
    const size_t byteCount = static_cast<size_t>(stride) * static_cast<size_t>(h);
    if (byteCount > static_cast<size_t>(UINT_MAX)) {
        out.lastError = "Image is too large";
        finish();
        return false;
    }

    std::vector<unsigned char> pixels(byteCount);
    hr = converter->CopyPixels(
        nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(hr)) {
        out.lastError = "Could not read image pixels: " + HrText(hr);
        finish();
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
        out.lastError = "CreateTexture2D failed: " + HrText(hr);
        finish();
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    ComPtr<ID3D11ShaderResourceView> srv;
    hr = device->CreateShaderResourceView(texture.Get(), &srvDesc, srv.GetAddressOf());
    if (FAILED(hr)) {
        out.lastError = "CreateShaderResourceView failed: " + HrText(hr);
        finish();
        return false;
    }

    out.srv = srv;
    out.width = static_cast<int>(w);
    out.height = static_cast<int>(h);
    out.lastError.clear();

    finish();
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

static bool DisplayUsesIcon(DisplayMode mode) {
    return mode == DisplayMode::Icon ||
           mode == DisplayMode::IconAndCharges ||
           mode == DisplayMode::IconAndText ||
           mode == DisplayMode::IconAndTimeLeft ||
           mode == DisplayMode::IconChargesAndTimeLeft ||
           mode == DisplayMode::IconTextAndTimeLeft;
}

static bool DisplayUsesCustomText(DisplayMode mode) {
    return mode == DisplayMode::CustomText ||
           mode == DisplayMode::IconAndText ||
           mode == DisplayMode::CustomTextAndTimeLeft ||
           mode == DisplayMode::IconTextAndTimeLeft;
}

static bool DisplayUsesTimeLeft(DisplayMode mode) {
    return mode == DisplayMode::TimeLeft ||
           mode == DisplayMode::ChargesAndTimeLeft ||
           mode == DisplayMode::CustomTextAndTimeLeft ||
           mode == DisplayMode::IconAndTimeLeft ||
           mode == DisplayMode::IconChargesAndTimeLeft ||
           mode == DisplayMode::IconTextAndTimeLeft;
}

static std::string FormatTimeLeft(float seconds, bool permanent) {
    if (permanent || !std::isfinite(seconds))
        return "inf";

    if (seconds < 0.0f)
        seconds = 0.0f;

    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << seconds << "s";
    return out.str();
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

                const char* sourceItems[] = { "Buff", "Active Skill" };
                ImGui::Combo("Source", &t.sourceMode, sourceItems, IM_ARRAYSIZE(sourceItems));

                const bool isSkill = t.sourceMode == static_cast<int>(SourceMode::ActiveSkill);
                ImGui::InputText(isSkill ? "Skill name / pattern" : "Buff name / pattern",
                                 t.pattern, sizeof(t.pattern));

                const char* matchItems[] = { "Exact", "Starts with", "Contains" };
                ImGui::Combo("Match mode", &t.matchMode, matchItems, IM_ARRAYSIZE(matchItems));
                if (!isSkill)
                    ImGui::TextDisabled("For names with a changing numeric suffix, use Starts with and omit the number.");

                if (isSkill) {
                    const char* valueItems[] = {
                        "Use Stage",
                        "Total Uses",
                        "Available Charges",
                        "Max Uses",
                        "Can Be Used"
                    };
                    ImGui::Combo("Tracked value", &t.activeSkillValue,
                                 valueItems, IM_ARRAYSIZE(valueItems));

                    const ActiveSkillValue valueMode =
                        static_cast<ActiveSkillValue>(t.activeSkillValue);

                    if (valueMode == ActiveSkillValue::CanBeUsed) {
                        int boolCondition = 0; // 0 = Any, 1 = true, 2 = false
                        if (t.stageCompare == static_cast<int>(StageCompare::Equal))
                            boolCondition = (t.stageValue != 0) ? 1 : 2;

                        const char* boolConditions[] = { "Any", "Is true", "Is false" };
                        if (ImGui::Combo("Activation condition", &boolCondition,
                                         boolConditions, IM_ARRAYSIZE(boolConditions))) {
                            if (boolCondition == 0) {
                                t.stageCompare = static_cast<int>(StageCompare::Any);
                                t.stageValue = 0;
                            } else {
                                t.stageCompare = static_cast<int>(StageCompare::Equal);
                                t.stageValue = (boolCondition == 1) ? 1 : 0;
                            }
                        }
                    } else {
                        const char* compareItems[] = {
                            "Any value", "Value ==", "Value !=", "Value >",
                            "Value >=", "Value <", "Value <="
                        };
                        ImGui::Combo("Activation condition", &t.stageCompare,
                                     compareItems, IM_ARRAYSIZE(compareItems));
                        if (t.stageCompare != static_cast<int>(StageCompare::Any))
                            ImGui::InputInt("Condition value", &t.stageValue);
                    }

                    switch (valueMode) {
                        case ActiveSkillValue::UseStage:
                            ImGui::TextDisabled("Uses ActiveSkill::UseStage.");
                            break;
                        case ActiveSkillValue::TotalUses:
                            ImGui::TextDisabled("Uses ActiveSkill::TotalUses.");
                            break;
                        case ActiveSkillValue::AvailableCharges:
                            ImGui::TextDisabled("Calculated as MaxUses - TotalActiveCooldowns, clamped to 0.");
                            break;
                        case ActiveSkillValue::MaxUses:
                            ImGui::TextDisabled("Uses ActiveSkill::MaxUses.");
                            break;
                        case ActiveSkillValue::CanBeUsed:
                            ImGui::TextDisabled("Uses ActiveSkill::CanBeUsed.");
                            break;
                    }
                }

                // Active Skills do not expose Buff::TimeLeft, so Time Left
                // display modes are intentionally only available for Buff trackers.
                if (isSkill && t.displayMode > static_cast<int>(DisplayMode::IconAndText))
                    t.displayMode = static_cast<int>(DisplayMode::Charges);

                const char* displayItemsBuff[] = {
                    "Charges",
                    "Custom text",
                    "Icon",
                    "Icon + charges",
                    "Icon + custom text",
                    "Time left",
                    "Charges + time left",
                    "Custom text + time left",
                    "Icon + time left",
                    "Icon + charges + time left",
                    "Icon + custom text + time left"
                };
                const char* displayItemsSkill[] = {
                    "Value", "Custom text", "Icon", "Icon + value", "Icon + custom text"
                };

                if (isSkill) {
                    ImGui::Combo("Display", &t.displayMode,
                                 displayItemsSkill, IM_ARRAYSIZE(displayItemsSkill));
                } else {
                    ImGui::Combo("Display", &t.displayMode,
                                 displayItemsBuff, IM_ARRAYSIZE(displayItemsBuff));
                    if (DisplayUsesTimeLeft(static_cast<DisplayMode>(t.displayMode))) {
                        ImGui::TextDisabled(
                            "Time left is shown in seconds. Permanent buffs are displayed as 'inf'.");
                    }
                }

                const char* missingItems[] = { "Hide", "Show 0" };
                ImGui::Combo(isSkill ? "If skill/condition is missing" : "If buff is missing",
                             &t.missingMode, missingItems, IM_ARRAYSIZE(missingItems));
                if (!isSkill)
                    ImGui::Checkbox("Sum charges of all matching buffs", &t.sumCharges);

                const DisplayMode currentDisplay =
                    static_cast<DisplayMode>(t.displayMode);

                if (DisplayUsesCustomText(currentDisplay)) {
                    ImGui::InputText("Custom text", t.customText, sizeof(t.customText));
                }

                if (DisplayUsesIcon(currentDisplay)) {
                    if (ImGui::InputText("Icon path", t.iconPath, sizeof(t.iconPath))) {
                        // Any manual edit should trigger a fresh load attempt.
                        t.texture = {};
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Browse...")) {
                        std::string selected;
                        HWND owner = ctx() ? ctx()->Game.GetGameWindow() : nullptr;
                        if (PickImageFile(owner, selected)) {
                            CopyToBuffer(t.iconPath, sizeof(t.iconPath), selected);
                            t.texture = {};
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Reload icon")) {
                        t.texture = {};
                    }

                    ImGui::SliderFloat("Icon size", &t.iconSize, 12.0f, 128.0f, "%.0f px");

                    if (t.texture.srv) {
                        ImGui::TextDisabled("Icon loaded: %d x %d", t.texture.width, t.texture.height);
                    } else if (!t.texture.lastError.empty()) {
                        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                                           "Icon load failed: %s", t.texture.lastError.c_str());
                    }
                }

                ImGui::DragFloat2("Position (X/Y)", &t.x, 1.0f, 0.0f, 10000.0f, "%.0f");
                ImGui::SliderFloat("Text scale", &t.fontScale, 0.5f, 5.0f, "%.2f");
                ImGui::ColorEdit4("Text color", &t.color.x,
                                  ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);

                ImGui::Checkbox("Text background", &t.textBackground);
                if (t.textBackground) {
                    ImGui::ColorEdit4("Background color", &t.backgroundColor.x,
                                      ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                }

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

        std::vector<PluginSDK::ActiveSkill> skills;
        if (snapshot.Player.Components.HasActor()) {
            skills = ctx()->Components.EnumerateActiveSkills(snapshot.Player.Components.Actor);
        }

        if (m_showDebug) DrawDebugWindow(buffs, skills);

        for (size_t i = 0; i < m_trackers.size(); ++i) {
            Tracker& tracker = m_trackers[i];
            if (!tracker.enabled) continue;
            DrawTracker(i, tracker, buffs, skills);
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
            out << p << "SourceMode=" << t.sourceMode << '\n';
            out << p << "MatchMode=" << t.matchMode << '\n';
            out << p << "ActiveSkillValue=" << t.activeSkillValue << '\n';
            out << p << "StageCompare=" << t.stageCompare << '\n';
            out << p << "StageValue=" << t.stageValue << '\n';
            out << p << "DisplayMode=" << t.displayMode << '\n';
            out << p << "MissingMode=" << t.missingMode << '\n';
            out << p << "CustomText=" << SanitizeValue(t.customText) << '\n';
            out << p << "IconPath=" << SanitizeValue(NormalizeIconPath(t.iconPath)) << '\n';
            out << p << "X=" << t.x << '\n';
            out << p << "Y=" << t.y << '\n';
            out << p << "FontScale=" << t.fontScale << '\n';
            out << p << "IconSize=" << t.iconSize << '\n';
            out << p << "SumCharges=" << (t.sumCharges ? 1 : 0) << '\n';
            out << p << "Color=" << t.color.x << ',' << t.color.y << ',' << t.color.z << ',' << t.color.w << '\n';
            out << p << "TextBackground=" << (t.textBackground ? 1 : 0) << '\n';
            out << p << "BackgroundColor="
                << t.backgroundColor.x << ',' << t.backgroundColor.y << ','
                << t.backgroundColor.z << ',' << t.backgroundColor.w << '\n';
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
                else if (field == "SourceMode") t->sourceMode = std::stoi(val);
                else if (field == "MatchMode") t->matchMode = std::stoi(val);
                else if (field == "ActiveSkillValue") t->activeSkillValue = std::stoi(val);
                else if (field == "StageCompare") t->stageCompare = std::stoi(val);
                else if (field == "StageValue") t->stageValue = std::stoi(val);
                else if (field == "DisplayMode") t->displayMode = std::stoi(val);
                else if (field == "MissingMode") t->missingMode = std::stoi(val);
                else if (field == "CustomText") CopyToBuffer(t->customText, sizeof(t->customText), val);
                else if (field == "IconPath") CopyToBuffer(t->iconPath, sizeof(t->iconPath), NormalizeIconPath(val));
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
                else if (field == "TextBackground") {
                    t->textBackground = (val == "1");
                }
                else if (field == "BackgroundColor") {
                    std::stringstream ss(val);
                    std::string part;
                    float c[4] = {0.0f, 0.0f, 0.0f, 0.70f};
                    for (int n = 0; n < 4 && std::getline(ss, part, ','); ++n) c[n] = std::stof(part);
                    t->backgroundColor = ImVec4(c[0], c[1], c[2], c[3]);
                }
            } catch (...) {
                // Keep defaults for malformed values.
            }
        }
    }

    void DrawDebugWindow(const std::vector<PluginSDK::Buff>& buffs,
                         const std::vector<PluginSDK::ActiveSkill>& skills) {
        ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Buff Overlay - Debug##BuffOverlayDebug", &m_showDebug)) {
            ImGui::End();
            return;
        }

        if (ImGui::CollapsingHeader("Buffs", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Active buffs: %d", static_cast<int>(buffs.size()));
            if (ImGui::BeginTable("##buffdebug", 3,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                                  ImVec2(0, 180))) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Charges", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Time left", ImGuiTableColumnFlags_WidthFixed, 90);
                ImGui::TableHeadersRow();
                for (const auto& b : buffs) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(b.Name.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%d", static_cast<int>(b.Charges));
                    ImGui::TableNextColumn();
                    if (b.TotalTime <= 0.0f || !std::isfinite(b.TimeLeft))
                        ImGui::TextUnformatted("inf");
                    else
                        ImGui::Text("%.1f", b.TimeLeft);
                }
                ImGui::EndTable();
            }
        }

        if (ImGui::CollapsingHeader("Active Skills", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Active skills: %d", static_cast<int>(skills.size()));
            if (ImGui::BeginTable("##skilldebug", 8,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                                  ImVec2(0, 220))) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Use Stage", ImGuiTableColumnFlags_WidthFixed, 75);
                ImGui::TableSetupColumn("Total Uses", ImGuiTableColumnFlags_WidthFixed, 75);
                ImGui::TableSetupColumn("Available", ImGuiTableColumnFlags_WidthFixed, 75);
                ImGui::TableSetupColumn("Max Uses", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Active CDs", ImGuiTableColumnFlags_WidthFixed, 70);
                ImGui::TableSetupColumn("Cooldown ms", ImGuiTableColumnFlags_WidthFixed, 90);
                ImGui::TableSetupColumn("Usable", ImGuiTableColumnFlags_WidthFixed, 65);
                ImGui::TableHeadersRow();

                for (const auto& sk : skills) {
                    const int available =
                        (std::max)(0, sk.MaxUses - sk.TotalActiveCooldowns);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(sk.Name.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%d", sk.UseStage);
                    ImGui::TableNextColumn(); ImGui::Text("%d", sk.TotalUses);
                    ImGui::TableNextColumn(); ImGui::Text("%d", available);
                    ImGui::TableNextColumn(); ImGui::Text("%d", sk.MaxUses);
                    ImGui::TableNextColumn(); ImGui::Text("%d", sk.TotalActiveCooldowns);
                    ImGui::TableNextColumn(); ImGui::Text("%d", sk.TotalCooldownMs);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(sk.CanBeUsed ? "yes" : "no");
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    void DrawTracker(size_t index, Tracker& t,
                     const std::vector<PluginSDK::Buff>& buffs,
                     const std::vector<PluginSDK::ActiveSkill>& skills) {
        bool found = false;
        int value = 0;

        // Time-left data is meaningful for Buff trackers only.
        float timeLeft = 0.0f;
        bool timePermanent = false;
        bool haveTimedMatch = false;

        if (t.sourceMode == static_cast<int>(SourceMode::ActiveSkill)) {
            const ActiveSkillValue valueMode =
                static_cast<ActiveSkillValue>(t.activeSkillValue);

            for (const auto& skill : skills) {
                if (!Matches(skill.Name, t.pattern, static_cast<MatchMode>(t.matchMode)))
                    continue;

                const int candidateValue = GetActiveSkillValue(skill, valueMode);

                if (!StageMatches(candidateValue,
                                  static_cast<StageCompare>(t.stageCompare),
                                  t.stageValue)) {
                    continue;
                }

                value = candidateValue;
                found = true;
                break;
            }
        } else {
            for (const auto& b : buffs) {
                if (!Matches(b.Name, t.pattern, static_cast<MatchMode>(t.matchMode)))
                    continue;

                if (!found) {
                    value = static_cast<int>(b.Charges);
                    found = true;
                } else if (t.sumCharges) {
                    value += static_cast<int>(b.Charges);
                }

                // For a single exact match this is simply that buff's TimeLeft.
                // If a pattern happens to match multiple buffs, show the longest
                // remaining duration because the tracked effect family remains
                // present until the last matching timed buff expires.
                //
                // TotalTime <= 0 represents a permanent buff in the SDK.
                const bool permanent =
                    b.TotalTime <= 0.0f || !std::isfinite(b.TimeLeft);

                if (permanent) {
                    timePermanent = true;
                } else {
                    const float remaining = (std::max)(0.0f, b.TimeLeft);
                    if (!haveTimedMatch || remaining > timeLeft)
                        timeLeft = remaining;
                    haveTimedMatch = true;
                }

                if (!t.sumCharges)
                    break;
            }
        }

        if (!found && !m_previewMode &&
            t.missingMode == static_cast<int>(MissingMode::Hide)) {
            return;
        }

        if (!found) {
            value = 0;
            timeLeft = 0.0f;
            timePermanent = false;
        }

        const DisplayMode display = static_cast<DisplayMode>(t.displayMode);
        const bool wantsIcon = DisplayUsesIcon(display);

        if (wantsIcon && t.iconPath[0] != '\0') {
            const std::string path = NormalizeIconPath(t.iconPath);

            if (path != std::string(t.iconPath)) {
                CopyToBuffer(t.iconPath, sizeof(t.iconPath), path);
                t.texture = {};
            }

            if (t.texture.loadedPath != path) {
                LoadTextureWic(
                    static_cast<ID3D11Device*>(ctx()->D3DDevice), path, t.texture);
            }
        } else if (!wantsIcon) {
            t.texture = {};
        }

        if (m_previewMode) {
            DrawDraggableTracker(
                index, t, value, timeLeft, timePermanent, found, wantsIcon);
        } else {
            DrawFixedTracker(t, value, timeLeft, timePermanent, wantsIcon);
        }
    }

    std::string BuildText(const Tracker& t,
                          int value,
                          float timeLeft,
                          bool timePermanent) const {
        const DisplayMode mode = static_cast<DisplayMode>(t.displayMode);

        std::string numeric;
        if (t.sourceMode == static_cast<int>(SourceMode::ActiveSkill) &&
            static_cast<ActiveSkillValue>(t.activeSkillValue) == ActiveSkillValue::CanBeUsed) {
            numeric = (value != 0) ? "true" : "false";
        } else {
            numeric = std::to_string(value);
        }

        const std::string time = FormatTimeLeft(timeLeft, timePermanent);
        const std::string custom = t.customText;

        switch (mode) {
            case DisplayMode::Charges:
            case DisplayMode::IconAndCharges:
                return numeric;

            case DisplayMode::CustomText:
            case DisplayMode::IconAndText:
                return custom;

            case DisplayMode::TimeLeft:
            case DisplayMode::IconAndTimeLeft:
                return time;

            case DisplayMode::ChargesAndTimeLeft:
            case DisplayMode::IconChargesAndTimeLeft:
                return numeric + " | " + time;

            case DisplayMode::CustomTextAndTimeLeft:
            case DisplayMode::IconTextAndTimeLeft:
                return custom + " | " + time;

            case DisplayMode::Icon:
            default:
                return {};
        }
    }

    void DrawFixedTracker(const Tracker& t,
                          int value,
                          float timeLeft,
                          bool timePermanent,
                          bool wantsIcon) {
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        ImVec2 pos(t.x, t.y);

        if (wantsIcon && t.texture.srv) {
            const ImTextureID texId = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(t.texture.srv.Get()));
            draw->AddImage(ImTextureRef(texId), pos, ImVec2(pos.x + t.iconSize, pos.y + t.iconSize));
            pos.x += t.iconSize + 6.0f;
        }

        const std::string text = BuildText(t, value, timeLeft, timePermanent);
        if (!text.empty()) {
            ImFont* font = ImGui::GetFont();
            const float size = ImGui::GetFontSize() * t.fontScale;

            if (t.textBackground) {
                const ImVec2 rawSize = ImGui::CalcTextSize(text.c_str());
                const ImVec2 textSize(rawSize.x * t.fontScale,
                                      rawSize.y * t.fontScale);
                constexpr float padX = 5.0f;
                constexpr float padY = 3.0f;
                const ImU32 bg = ImGui::ColorConvertFloat4ToU32(t.backgroundColor);

                draw->AddRectFilled(
                    ImVec2(pos.x - padX, pos.y - padY),
                    ImVec2(pos.x + textSize.x + padX,
                           pos.y + textSize.y + padY),
                    bg,
                    3.0f);
            }

            const ImU32 col = ImGui::ColorConvertFloat4ToU32(t.color);
            draw->AddText(font, size, pos, col, text.c_str());
        }
    }

    void DrawDraggableTracker(size_t index,
                              Tracker& t,
                              int value,
                              float timeLeft,
                              bool timePermanent,
                              bool found,
                              bool wantsIcon) {
        const std::string text = BuildText(t, value, timeLeft, timePermanent);
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
                const ImVec2 textPos = ImGui::GetCursorScreenPos();
                const ImVec2 rawSize = ImGui::CalcTextSize(text.c_str());
                const ImVec2 textSize(rawSize.x * t.fontScale,
                                      rawSize.y * t.fontScale);

                if (t.textBackground) {
                    constexpr float padX = 5.0f;
                    constexpr float padY = 3.0f;
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(
                        ImVec2(textPos.x - padX, textPos.y - padY),
                        ImVec2(textPos.x + textSize.x + padX,
                               textPos.y + textSize.y + padY),
                        ImGui::ColorConvertFloat4ToU32(t.backgroundColor),
                        3.0f);
                }

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

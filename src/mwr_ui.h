#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "imgui.h"
#include "roboto_medium_font.h"

namespace ModTheme {
    static constexpr ImU32 kBgMain       = IM_COL32(22, 23, 26, 250);
    static constexpr ImU32 kBgSidebar    = IM_COL32(34, 36, 41, 252);
    static constexpr ImU32 kBgPill       = IM_COL32(38, 40, 46, 255);
    static constexpr ImU32 kBgPillHover  = IM_COL32(49, 52, 60, 255);
    static constexpr ImU32 kDivider      = IM_COL32(38, 40, 47, 255);
    static constexpr ImU32 kCheckBg      = IM_COL32(27, 29, 34, 255);
    static constexpr ImU32 kCheckBorder  = IM_COL32(66, 70, 80, 255);
    static constexpr ImU32 kTrackBg      = IM_COL32(43, 46, 54, 255);
    static constexpr ImU32 kAccent       = IM_COL32(99, 176, 250, 255);
    static constexpr ImU32 kAccentHover  = IM_COL32(124, 192, 255, 255);
    static constexpr ImU32 kOnAccent     = IM_COL32(20, 22, 26, 255);
    static constexpr ImU32 kTextPrimary  = IM_COL32(236, 238, 242, 255);
    static constexpr ImU32 kTextMuted    = IM_COL32(142, 147, 158, 255);
    static constexpr ImU32 kDanger       = IM_COL32(220, 88, 102, 255);
    static constexpr ImU32 kDangerBg     = IM_COL32(58, 30, 36, 255);
    static constexpr ImU32 kDangerBgHov  = IM_COL32(82, 36, 44, 255);
    static constexpr ImU32 kGoodDot      = IM_COL32(99, 176, 250, 255);
    static constexpr ImU32 kWarnDot      = IM_COL32(238, 168, 76, 255);
}

static ImFont* g_fontBody  = nullptr;
static ImFont* g_fontTitle = nullptr;
static ImFont* g_fontSmall = nullptr;
static int     g_activeTab = 0;

static void InitModFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH  = true;

    g_fontBody = io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(kRobotoMediumData),
        static_cast<int>(kRobotoMediumSize),
        16.0f * scale, &cfg);
    g_fontTitle = io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(kRobotoMediumData),
        static_cast<int>(kRobotoMediumSize),
        21.0f * scale, &cfg);
    g_fontSmall = io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(kRobotoMediumData),
        static_cast<int>(kRobotoMediumSize),
        13.0f * scale, &cfg);

    io.FontDefault     = g_fontBody;
    io.FontGlobalScale = 1.0f;
}

static void ApplyCustomStyle(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = 14.0f;
    s.ChildRounding     = 0.0f;
    s.FrameRounding     = 8.0f;
    s.PopupRounding     = 10.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding      = 8.0f;
    s.TabRounding       = 8.0f;

    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 0.0f;
    s.FrameBorderSize   = 0.0f;
    s.WindowPadding     = ImVec2(10.0f, 10.0f);
    s.FramePadding      = ImVec2(10.0f, 6.0f);
    s.ItemSpacing       = ImVec2(8.0f, 8.0f);
    s.ScrollbarSize     = 5.0f;
    s.GrabMinSize       = 14.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]             = ImVec4(22.0f / 255.0f, 23.0f / 255.0f, 26.0f / 255.0f, 0.98f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_Border]               = ImVec4(44.0f / 255.0f, 47.0f / 255.0f, 55.0f / 255.0f, 1.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(38.0f / 255.0f, 40.0f / 255.0f, 46.0f / 255.0f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(49.0f / 255.0f, 52.0f / 255.0f, 60.0f / 255.0f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(60.0f / 255.0f, 64.0f / 255.0f, 74.0f / 255.0f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(20.0f / 255.0f, 22.0f / 255.0f, 26.0f / 255.0f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(99.0f / 255.0f, 176.0f / 255.0f, 250.0f / 255.0f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(124.0f / 255.0f, 192.0f / 255.0f, 255.0f / 255.0f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(38.0f / 255.0f, 40.0f / 255.0f, 46.0f / 255.0f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(49.0f / 255.0f, 52.0f / 255.0f, 60.0f / 255.0f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(99.0f / 255.0f, 176.0f / 255.0f, 250.0f / 255.0f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(58.0f / 255.0f, 62.0f / 255.0f, 72.0f / 255.0f, 0.85f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(80.0f / 255.0f, 86.0f / 255.0f, 98.0f / 255.0f, 0.95f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(99.0f / 255.0f, 176.0f / 255.0f, 250.0f / 255.0f, 1.00f);
    c[ImGuiCol_Text]                 = ImVec4(236.0f / 255.0f, 238.0f / 255.0f, 242.0f / 255.0f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(142.0f / 255.0f, 147.0f / 255.0f, 158.0f / 255.0f, 1.00f);

    s.ScaleAllSizes(scale);
    InitModFonts(scale);
}

namespace ModIcons {

static void Crosshair(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float t) {
    dl->AddCircle(c, r * 0.64f, col, 24, t);
    dl->AddCircleFilled(c, t * 0.9f, col, 12);
    const float inner = r * 0.38f;
    const float outer = r * 1.00f;
    dl->AddLine(ImVec2(c.x, c.y - outer), ImVec2(c.x, c.y - inner), col, t);
    dl->AddLine(ImVec2(c.x, c.y + inner), ImVec2(c.x, c.y + outer), col, t);
    dl->AddLine(ImVec2(c.x - outer, c.y), ImVec2(c.x - inner, c.y), col, t);
    dl->AddLine(ImVec2(c.x + inner, c.y), ImVec2(c.x + outer, c.y), col, t);
}

static void Eye(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float t) {
    ImVec2 pL(c.x - r, c.y);
    ImVec2 pR(c.x + r, c.y);
    ImVec2 cpT1(c.x - r * 0.45f, c.y - r * 0.78f);
    ImVec2 cpT2(c.x + r * 0.45f, c.y - r * 0.78f);
    ImVec2 cpB1(c.x - r * 0.45f, c.y + r * 0.78f);
    ImVec2 cpB2(c.x + r * 0.45f, c.y + r * 0.78f);
    dl->AddBezierCubic(pL, cpT1, cpT2, pR, col, t, 16);
    dl->AddBezierCubic(pL, cpB1, cpB2, pR, col, t, 16);
    dl->AddCircle(c, r * 0.30f, col, 18, t);
}

static void Person(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float t) {
    dl->AddCircle(ImVec2(c.x, c.y - r * 0.36f), r * 0.34f, col, 20, t);
    dl->PathClear();
    dl->PathArcTo(ImVec2(c.x, c.y + r * 0.85f), r * 0.72f, 3.14159f * 1.15f, 3.14159f * 1.85f, 16);
    dl->PathStroke(col, 0, t);
    dl->AddLine(ImVec2(c.x - r * 0.62f, c.y + r * 0.68f),
                ImVec2(c.x + r * 0.62f, c.y + r * 0.68f), col, t);
}

static void List(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float t) {
    const float ys[3] = { c.y - r * 0.54f, c.y, c.y + r * 0.54f };
    for (int i = 0; i < 3; ++i) {
        dl->AddCircleFilled(ImVec2(c.x - r * 0.62f, ys[i]), t * 0.78f, col, 10);
        dl->AddLine(ImVec2(c.x - r * 0.22f, ys[i]), ImVec2(c.x + r * 0.80f, ys[i]), col, t);
    }
}

static void ResetIcon(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float t) {
    dl->PathClear();
    dl->PathArcTo(c, r * 0.72f, -1.2f, 3.8f, 20);
    dl->PathStroke(col, 0, t);
    ImVec2 tip(c.x + r * 0.72f * std::cos(-1.2f), c.y + r * 0.72f * std::sin(-1.2f));
    dl->AddLine(tip, ImVec2(tip.x - r * 0.38f, tip.y - r * 0.08f), col, t);
    dl->AddLine(tip, ImVec2(tip.x + r * 0.05f, tip.y - r * 0.38f), col, t);
}

static void Close(ImDrawList* dl, ImVec2 c, float r, ImU32 col, float t) {
    const float d = r * 0.68f;
    dl->AddLine(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), col, t);
    dl->AddLine(ImVec2(c.x + d, c.y - d), ImVec2(c.x - d, c.y + d), col, t);
}

static void DrawTabIcon(int idx, ImDrawList* dl, ImVec2 c, float r, ImU32 col, float t) {
    switch (idx) {
        case 0: Crosshair(dl, c, r, col, t); break;
        case 1: Eye(dl, c, r, col, t);       break;
        case 2: Person(dl, c, r, col, t);    break;
        default: List(dl, c, r, col, t);     break;
    }
}

} // namespace ModIcons

static bool DrawToggleRow(const char* id, const char* label, bool* val, float scale,
                          const char* badge = nullptr, bool drawDivider = true) {
    const float rowH   = 44.0f * scale;
    const float availW = std::max(1.0f, ImGui::GetContentRegionAvail().x - 8.0f * scale);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    bool clicked = ImGui::InvisibleButton(id, ImVec2(availW, rowH));
    if (clicked) {
        *val = !(*val);
    }

    const bool hovered = ImGui::IsItemHovered();
    const bool checked = *val;
    const float cy = pos.y + rowH * 0.5f;

    if (hovered) {
        dl->AddRectFilled(ImVec2(pos.x - 6.0f * scale, pos.y + 2.0f * scale),
                          ImVec2(pos.x + availW + 6.0f * scale, pos.y + rowH - 2.0f * scale),
                          IM_COL32(255, 255, 255, 8), 8.0f * scale);
    }

    const float boxSz = 20.0f * scale;
    const float boxR  = 5.5f * scale;
    ImVec2 b0(pos.x, cy - boxSz * 0.5f);
    ImVec2 b1(pos.x + boxSz, cy + boxSz * 0.5f);

    if (checked) {
        dl->AddRectFilled(b0, b1, ModTheme::kAccent, boxR);
        ImVec2 pts[3] = {
            ImVec2(b0.x + boxSz * 0.24f, b0.y + boxSz * 0.52f),
            ImVec2(b0.x + boxSz * 0.44f, b0.y + boxSz * 0.72f),
            ImVec2(b0.x + boxSz * 0.78f, b0.y + boxSz * 0.30f)
        };
        dl->AddPolyline(pts, 3, ModTheme::kOnAccent, 0, 2.2f * scale);
    } else {
        dl->AddRectFilled(b0, b1, ModTheme::kCheckBg, boxR);
        dl->AddRect(b0, b1, hovered ? ModTheme::kTextMuted : ModTheme::kCheckBorder, boxR, 0, 1.5f * scale);
    }

    const float labelX = pos.x + boxSz + 14.0f * scale;
    ImVec2 txtSz = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(labelX, cy - txtSz.y * 0.5f),
                checked ? ModTheme::kTextPrimary : ModTheme::kTextMuted, label);

    if (badge && badge[0] != '\0') {
        if (g_fontSmall) ImGui::PushFont(g_fontSmall);
        ImVec2 bSz = ImGui::CalcTextSize(badge);
        const float padX = 8.0f * scale;
        const float padY = 3.0f * scale;
        const float bx1 = pos.x + availW;
        const float bx0 = bx1 - bSz.x - padX * 2.0f;
        const float by0 = cy - bSz.y * 0.5f - padY;
        const float by1 = cy + bSz.y * 0.5f + padY;
        dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1), ModTheme::kBgPill, 6.0f * scale);
        dl->AddText(ImVec2(bx0 + padX, cy - bSz.y * 0.5f),
                    checked ? ModTheme::kAccent : ModTheme::kTextMuted, badge);
        if (g_fontSmall) ImGui::PopFont();
    }

    if (drawDivider) {
        dl->AddLine(ImVec2(pos.x, pos.y + rowH),
                    ImVec2(pos.x + availW, pos.y + rowH),
                    ModTheme::kDivider, 1.0f);
    }

    return clicked;
}

static bool DrawSliderRow(const char* id, const char* label, float* val,
                          float vmin, float vmax, const char* fmt, float scale,
                          bool drawDivider = true) {
    const float rowH   = 44.0f * scale;
    const float availW = std::max(1.0f, ImGui::GetContentRegionAvail().x - 8.0f * scale);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float cy = pos.y + rowH * 0.5f;
    const float labelX = pos.x + 34.0f * scale;

    const float sliderW = std::clamp(availW * 0.36f, 88.0f * scale, 155.0f * scale);
    const float trackX1 = pos.x + availW;
    const float trackX0 = trackX1 - sliderW;

    ImGui::SetCursorScreenPos(ImVec2(trackX0 - 10.0f * scale, pos.y));
    ImGui::InvisibleButton(id, ImVec2(sliderW + 14.0f * scale, rowH));
    bool changed = false;
    if (ImGui::IsItemActive()) {
        float mx = ImGui::GetIO().MousePos.x;
        float t  = (sliderW > 1.0f) ? std::clamp((mx - trackX0) / sliderW, 0.0f, 1.0f) : 0.0f;
        float nv = vmin + t * (vmax - vmin);
        if (std::fabs(nv - *val) > 1e-4f) {
            *val = nv;
            changed = true;
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + rowH));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));

    ImVec2 lblSz = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(labelX, cy - lblSz.y * 0.5f), ModTheme::kTextPrimary, label);

    char valBuf[32];
    std::snprintf(valBuf, sizeof(valBuf), fmt, *val);
    ImVec2 valSz = ImGui::CalcTextSize(valBuf);
    dl->AddText(ImVec2(trackX0 - valSz.x - 12.0f * scale, cy - valSz.y * 0.5f),
                ModTheme::kTextPrimary, valBuf);

    const float trackH = 4.5f * scale;
    dl->AddRectFilled(ImVec2(trackX0, cy - trackH * 0.5f),
                      ImVec2(trackX1, cy + trackH * 0.5f),
                      ModTheme::kTrackBg, trackH * 0.5f);

    const float norm  = (vmax > vmin) ? std::clamp((*val - vmin) / (vmax - vmin), 0.0f, 1.0f) : 0.0f;
    const float fillH = 14.0f * scale;
    const float fillR = fillH * 0.5f;
    const float fillW = std::max(fillH, norm * sliderW);
    const float fillX1 = trackX0 + fillW;

    dl->AddRectFilled(ImVec2(trackX0, cy - fillR),
                      ImVec2(fillX1, cy + fillR),
                      ImGui::IsItemActive() ? ModTheme::kAccentHover : ModTheme::kAccent,
                      fillR);

    dl->AddCircleFilled(ImVec2(fillX1 - fillR, cy), 2.3f * scale, ModTheme::kOnAccent, 12);

    if (drawDivider) {
        dl->AddLine(ImVec2(pos.x, pos.y + rowH),
                    ImVec2(pos.x + availW, pos.y + rowH),
                    ModTheme::kDivider, 1.0f);
    }

    return changed;
}

static void DrawInfoRow(const char* label, const char* value, float scale,
                        ImU32 valCol = ModTheme::kTextPrimary, bool drawDivider = true) {
    const float rowH   = 38.0f * scale;
    const float availW = std::max(1.0f, ImGui::GetContentRegionAvail().x - 8.0f * scale);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = pos.y + rowH * 0.5f;

    ImGui::Dummy(ImVec2(availW, rowH));

    ImVec2 lblSz = ImGui::CalcTextSize(label);
    ImVec2 valSz = ImGui::CalcTextSize(value);
    dl->AddText(ImVec2(pos.x, cy - lblSz.y * 0.5f), ModTheme::kTextMuted, label);
    dl->AddText(ImVec2(pos.x + availW - valSz.x, cy - valSz.y * 0.5f), valCol, value);

    if (drawDivider) {
        dl->AddLine(ImVec2(pos.x, pos.y + rowH),
                    ImVec2(pos.x + availW, pos.y + rowH),
                    ModTheme::kDivider, 1.0f);
    }
}

static bool DrawActionRow(const char* id, const char* label, const char* btnLabel,
                          float scale, bool isDanger = false, bool drawDivider = true) {
    const float rowH   = 46.0f * scale;
    const float availW = std::max(1.0f, ImGui::GetContentRegionAvail().x - 8.0f * scale);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = pos.y + rowH * 0.5f;

    ImVec2 bTxtSz = ImGui::CalcTextSize(btnLabel);
    const float btnW = std::max(84.0f * scale, bTxtSz.x + 24.0f * scale);
    const float btnH = 28.0f * scale;
    const float bx0  = pos.x + availW - btnW;
    const float by0  = cy - btnH * 0.5f;

    ImGui::SetCursorScreenPos(ImVec2(bx0, by0));
    bool clicked = ImGui::InvisibleButton(id, ImVec2(btnW, btnH));
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + rowH));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));

    ImVec2 lblSz = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(pos.x, cy - lblSz.y * 0.5f), ModTheme::kTextPrimary, label);

    ImU32 bgCol = isDanger
        ? (active ? ModTheme::kDanger : (hovered ? ModTheme::kDangerBgHov : ModTheme::kDangerBg))
        : (active ? ModTheme::kAccent : (hovered ? ModTheme::kBgPillHover : ModTheme::kBgPill));
    ImU32 txtCol = active
        ? ModTheme::kOnAccent
        : (isDanger ? ModTheme::kDanger : ModTheme::kTextPrimary);

    dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx0 + btnW, by0 + btnH), bgCol, 8.0f * scale);
    dl->AddText(ImVec2(bx0 + (btnW - bTxtSz.x) * 0.5f, cy - bTxtSz.y * 0.5f), txtCol, btnLabel);

    if (drawDivider) {
        dl->AddLine(ImVec2(pos.x, pos.y + rowH),
                    ImVec2(pos.x + availW, pos.y + rowH),
                    ModTheme::kDivider, 1.0f);
    }

    return clicked;
}

static bool DrawCycleRow(const char* id, const char* label, const char* curVal,
                         float scale, bool drawDivider = true) {
    const float rowH   = 44.0f * scale;
    const float availW = std::max(1.0f, ImGui::GetContentRegionAvail().x - 8.0f * scale);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cy = pos.y + rowH * 0.5f;

    bool clicked = ImGui::InvisibleButton(id, ImVec2(availW, rowH));
    bool hovered = ImGui::IsItemHovered();

    if (hovered) {
        dl->AddRectFilled(ImVec2(pos.x - 6.0f * scale, pos.y + 2.0f * scale),
                          ImVec2(pos.x + availW + 6.0f * scale, pos.y + rowH - 2.0f * scale),
                          IM_COL32(255, 255, 255, 8), 8.0f * scale);
    }

    const float labelX = pos.x + 34.0f * scale;
    ImVec2 lblSz = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(labelX, cy - lblSz.y * 0.5f), ModTheme::kTextPrimary, label);

    if (g_fontSmall) ImGui::PushFont(g_fontSmall);
    ImVec2 vSz = ImGui::CalcTextSize(curVal);
    const float padX = 10.0f * scale;
    const float padY = 4.0f * scale;
    const float bx1 = pos.x + availW;
    const float bx0 = bx1 - vSz.x - padX * 2.0f;
    const float by0 = cy - vSz.y * 0.5f - padY;
    const float by1 = cy + vSz.y * 0.5f + padY;
    dl->AddRectFilled(ImVec2(bx0, by0), ImVec2(bx1, by1),
                      hovered ? ModTheme::kBgPillHover : ModTheme::kBgPill, 6.0f * scale);
    dl->AddText(ImVec2(bx0 + padX, cy - vSz.y * 0.5f), ModTheme::kAccent, curVal);
    if (g_fontSmall) ImGui::PopFont();

    if (drawDivider) {
        dl->AddLine(ImVec2(pos.x, pos.y + rowH),
                    ImVec2(pos.x + availW, pos.y + rowH),
                    ModTheme::kDivider, 1.0f);
    }

    return clicked;
}

static void DrawModUI(int surfW, int surfH, std::vector<RectF>& outRects) {
    const float w = static_cast<float>(surfW);
    const float h = static_cast<float>(surfH);
    const float scale = std::clamp(std::min(w, h) / 450.0f, 1.3f, 2.6f);

    if (!g_pillPosInit) {
        g_pillPos = ImVec2(14.0f * scale, 22.0f * scale);
        g_pillPosInit = true;
    }

    // 1. Floating [MWR] Toggle Pill
    ImGui::SetNextWindowPos(g_pillPos, ImGuiCond_FirstUseEver);
    ImGuiWindowFlags pillFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * scale, 4.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f * scale);
    if (ImGui::Begin("##MwrPill", nullptr, pillFlags)) {
        const bool isOpen = g_menuOpen.load();
        if (isOpen) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(99.0f / 255.0f, 176.0f / 255.0f, 250.0f / 255.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(124.0f / 255.0f, 192.0f / 255.0f, 255.0f / 255.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(20.0f / 255.0f, 22.0f / 255.0f, 26.0f / 255.0f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(34.0f / 255.0f, 36.0f / 255.0f, 41.0f / 255.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(46.0f / 255.0f, 49.0f / 255.0f, 56.0f / 255.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(236.0f / 255.0f, 238.0f / 255.0f, 242.0f / 255.0f, 1.0f));
        }
        if (ImGui::Button("MWR", ImVec2(58.0f * scale, 28.0f * scale))) {
            g_menuOpen.store(!isOpen);
        }
        ImGui::PopStyleColor(3);

        ImVec2 wp = ImGui::GetWindowPos();
        ImVec2 ws = ImGui::GetWindowSize();
        g_pillPos = wp;
        outRects.push_back({wp.x, wp.y, wp.x + ws.x, wp.y + ws.y});
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    // 2. Main Split-Pane Window
    if (g_menuOpen.load()) {
        const float winW = std::min(w * 0.76f, 420.0f * scale);
        const float winH = std::min(h * 0.76f, 340.0f * scale);
        ImGui::SetNextWindowPos(ImVec2((w - winW) * 0.5f, (h - winH) * 0.5f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_FirstUseEver);

        ImGuiWindowFlags mainFlags = ImGuiWindowFlags_NoTitleBar |
                                     ImGuiWindowFlags_NoCollapse |
                                     ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoScrollbar |
                                     ImGuiWindowFlags_NoScrollWithMouse |
                                     ImGuiWindowFlags_NoSavedSettings;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        if (ImGui::Begin("##MwrModernMenu", nullptr, mainFlags)) {
            ImVec2 wp = ImGui::GetWindowPos();
            ImVec2 ws = ImGui::GetWindowSize();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            const float sideW   = 56.0f * scale;
            const float headerH = 52.0f * scale;
            const float winR    = ImGui::GetStyle().WindowRounding;

            dl->AddRectFilled(wp, ImVec2(wp.x + sideW, wp.y + ws.y),
                              ModTheme::kBgSidebar, winR, ImDrawFlags_RoundCornersLeft);
            dl->AddLine(ImVec2(wp.x + sideW, wp.y),
                        ImVec2(wp.x + sideW, wp.y + ws.y),
                        ModTheme::kDivider, 1.0f);

            const float btnD     = 36.0f * scale;
            const float btnStep  = 50.0f * scale;
            const float startY   = wp.y + 16.0f * scale;
            const float centerX  = wp.x + sideW * 0.5f;
            const float strokeT  = 1.75f * scale;

            static const char* kTabIds[4]    = { "##TabCombat", "##TabVisuals", "##TabMove", "##TabInfo" };
            static const char* kTabTitles[4] = { "Combat", "Visuals", "Movement", "System" };

            for (int i = 0; i < 4; ++i) {
                float cy = startY + i * btnStep + btnD * 0.5f;
                ImGui::SetCursorScreenPos(ImVec2(centerX - btnD * 0.5f, cy - btnD * 0.5f));
                if (ImGui::InvisibleButton(kTabIds[i], ImVec2(btnD, btnD))) {
                    g_activeTab = i;
                }
                bool active  = (g_activeTab == i);
                bool hovered = ImGui::IsItemHovered();

                if (active) {
                    dl->AddCircleFilled(ImVec2(centerX, cy), btnD * 0.5f, ModTheme::kAccent, 28);
                } else if (hovered) {
                    dl->AddCircleFilled(ImVec2(centerX, cy), btnD * 0.5f, ModTheme::kBgPillHover, 28);
                }

                ImU32 iconCol = active ? ModTheme::kOnAccent : ModTheme::kTextPrimary;
                ModIcons::DrawTabIcon(i, dl, ImVec2(centerX, cy), 9.0f * scale, iconCol, strokeT);
            }

            const float padX     = 18.0f * scale;
            const float hdrLeft  = wp.x + sideW + padX;
            const float hdrRight = wp.x + ws.x - padX;
            const float hdrCy    = wp.y + headerH * 0.5f;

            ModIcons::DrawTabIcon(g_activeTab, dl, ImVec2(hdrLeft + 9.0f * scale, hdrCy),
                                  9.0f * scale, ModTheme::kTextPrimary, strokeT);

            if (g_fontTitle) ImGui::PushFont(g_fontTitle);
            const char* title = kTabTitles[g_activeTab];
            ImVec2 titleSz = ImGui::CalcTextSize(title);
            dl->AddText(ImVec2(hdrLeft + 28.0f * scale, hdrCy - titleSz.y * 0.5f),
                        ModTheme::kTextPrimary, title);
            if (g_fontTitle) ImGui::PopFont();

            const float closeSz = 26.0f * scale;
            const float closeX0 = hdrRight - closeSz;
            ImGui::SetCursorScreenPos(ImVec2(closeX0, hdrCy - closeSz * 0.5f));
            if (ImGui::InvisibleButton("##HdrClose", ImVec2(closeSz, closeSz))) {
                g_menuOpen.store(false);
            }
            if (ImGui::IsItemHovered()) {
                dl->AddCircleFilled(ImVec2(closeX0 + closeSz * 0.5f, hdrCy),
                                    closeSz * 0.48f, ModTheme::kBgPillHover, 20);
            }
            ModIcons::Close(dl, ImVec2(closeX0 + closeSz * 0.5f, hdrCy),
                            7.2f * scale, ModTheme::kTextMuted, 1.6f * scale);

            const float sepX = closeX0 - 8.0f * scale;
            dl->AddLine(ImVec2(sepX, hdrCy - 9.0f * scale),
                        ImVec2(sepX, hdrCy + 9.0f * scale),
                        ModTheme::kDivider, 1.0f);

            const float rstD  = 28.0f * scale;
            const float rstX0 = sepX - 10.0f * scale - rstD;
            ImGui::SetCursorScreenPos(ImVec2(rstX0, hdrCy - rstD * 0.5f));
            if (ImGui::InvisibleButton("##HdrReset", ImVec2(rstD, rstD))) {
                ResetStateToDefaults();
            }
            dl->AddCircleFilled(ImVec2(rstX0 + rstD * 0.5f, hdrCy), rstD * 0.5f,
                                ImGui::IsItemHovered() ? ModTheme::kBgPillHover : ModTheme::kBgPill, 24);
            ModIcons::ResetIcon(dl, ImVec2(rstX0 + rstD * 0.5f, hdrCy),
                                7.0f * scale, ModTheme::kTextPrimary, 1.5f * scale);

            const bool readyOk = (g_tele.camMgrAddr != 0 && g_tele.playerMgrAddr != 0);
            const char* pillTxt = (g_tele.localPlayerAddr != 0) ? "In Match" : (readyOk ? "Lobby" : "Scan");
            if (g_fontSmall) ImGui::PushFont(g_fontSmall);
            ImVec2 ptSz = ImGui::CalcTextSize(pillTxt);
            const float stPillH = 26.0f * scale;
            const float stPillW = ptSz.x + 26.0f * scale;
            const float stPillX0 = rstX0 - 8.0f * scale - stPillW;
            if (stPillX0 > hdrLeft + 28.0f * scale + titleSz.x + 10.0f * scale) {
                ImGui::SetCursorScreenPos(ImVec2(stPillX0, hdrCy - stPillH * 0.5f));
                if (ImGui::InvisibleButton("##HdrScan", ImVec2(stPillW, stPillH))) {
                    g_state.reqRescan = 1;
                }
                dl->AddRectFilled(ImVec2(stPillX0, hdrCy - stPillH * 0.5f),
                                  ImVec2(stPillX0 + stPillW, hdrCy + stPillH * 0.5f),
                                  ImGui::IsItemHovered() ? ModTheme::kBgPillHover : ModTheme::kBgPill,
                                  stPillH * 0.5f);
                dl->AddCircleFilled(ImVec2(stPillX0 + 11.0f * scale, hdrCy),
                                    3.2f * scale, readyOk ? ModTheme::kGoodDot : ModTheme::kWarnDot, 12);
                dl->AddText(ImVec2(stPillX0 + 19.0f * scale, hdrCy - ptSz.y * 0.5f),
                            ModTheme::kTextMuted, pillTxt);
            }
            if (g_fontSmall) ImGui::PopFont();

            ImGui::SetCursorScreenPos(ImVec2(wp.x + sideW + padX, wp.y + headerH + 4.0f * scale));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
            const float contentW = ws.x - sideW - padX * 2.0f + 8.0f * scale;
            const float contentH = ws.y - headerH - 12.0f * scale;

            if (ImGui::BeginChild("##ModContent", ImVec2(contentW, contentH), false)) {
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

                if (g_activeTab == 0) {
                    bool aim = g_state.aimbot != 0;
                    if (DrawToggleRow("##aimbot", "Aimbot (LookAngles)", &aim, scale, aim ? "LOCK" : nullptr)) {
                        g_state.aimbot = aim ? 1 : 0;
                    }
                    if (aim) {
                        DrawSliderRow("##aimFov", "FOV radius", &g_state.aimFovPx, 40.0f, 450.0f, "%.0f px", scale);
                        DrawSliderRow("##aimSmooth", "Smoothing", &g_state.aimSmooth, 1.0f, 15.0f, "%.1f", scale);
                        static const char* kPrioNames[3] = { "Near FOV", "Closest", "Low HP" };
                        int prio = std::clamp(g_state.aimPriority, 0, 2);
                        if (DrawCycleRow("##aimPrio", "Target priority", kPrioNames[prio], scale)) {
                            g_state.aimPriority = (prio + 1) % 3;
                        }
                        bool visChk = g_state.aimVisCheck != 0;
                        if (DrawToggleRow("##aimVis", "Visibility check", &visChk, scale)) {
                            g_state.aimVisCheck = visChk ? 1 : 0;
                        }
                        bool tracer = g_state.aimTracer != 0;
                        if (DrawToggleRow("##aimTracer", "Target tracer", &tracer, scale)) {
                            g_state.aimTracer = tracer ? 1 : 0;
                        }
                        bool drawFov = g_state.drawFov != 0;
                        if (DrawToggleRow("##drawFov", "Draw FOV circle", &drawFov, scale)) {
                            g_state.drawFov = drawFov ? 1 : 0;
                        }
                        bool aimTeam = g_state.aimTeamCheck != 0;
                        if (DrawToggleRow("##aimTeam", "Aim team check", &aimTeam, scale)) {
                            g_state.aimTeamCheck = aimTeam ? 1 : 0;
                        }
                    }

                    bool noRec = g_state.noRecoil != 0;
                    if (DrawToggleRow("##noRec", "No recoil", &noRec, scale)) {
                        g_state.noRecoil = noRec ? 1 : 0;
                    }
                    bool noSpr = g_state.noSpread != 0;
                    if (DrawToggleRow("##noSpr", "No spread", &noSpr, scale)) {
                        g_state.noSpread = noSpr ? 1 : 0;
                    }
                    bool rapid = g_state.rapidFire != 0;
                    if (DrawToggleRow("##rapid", "Rapid fire", &rapid, scale, nullptr, false)) {
                        g_state.rapidFire = rapid ? 1 : 0;
                    }
                } else if (g_activeTab == 1) {
                    bool esp = g_state.espEnabled != 0;
                    if (DrawToggleRow("##espOn", "Player ESP", &esp, scale, esp ? "ON" : nullptr)) {
                        g_state.espEnabled = esp ? 1 : 0;
                    }
                    bool box = g_state.espBox != 0;
                    if (DrawToggleRow("##espBox", "2D bounding box", &box, scale)) {
                        g_state.espBox = box ? 1 : 0;
                    }
                    bool line = g_state.espLine != 0;
                    if (DrawToggleRow("##espLine", "Snaplines", &line, scale)) {
                        g_state.espLine = line ? 1 : 0;
                    }
                    bool hp = g_state.espHp != 0;
                    if (DrawToggleRow("##espHp", "Health bar", &hp, scale)) {
                        g_state.espHp = hp ? 1 : 0;
                    }
                    bool info = g_state.espInfo != 0;
                    if (DrawToggleRow("##espInfo", "Name & distance", &info, scale)) {
                        g_state.espInfo = info ? 1 : 0;
                    }
                    bool team = g_state.espTeamCheck != 0;
                    if (DrawToggleRow("##espTeam", "Hide teammates", &team, scale)) {
                        g_state.espTeamCheck = team ? 1 : 0;
                    }
                    DrawSliderRow("##espDist", "Max distance", &g_state.espMaxDist, 30.0f, 500.0f, "%.0f m", scale, false);
                } else if (g_activeTab == 2) {
                    bool spd = g_state.speedHack != 0;
                    if (DrawToggleRow("##spdHack", "Speed multiplier", &spd, scale)) {
                        g_state.speedHack = spd ? 1 : 0;
                    }
                    if (spd) {
                        DrawSliderRow("##spdMult", "Speed scale", &g_state.speedMult, 1.0f, 4.0f, "%.2fx", scale);
                    }

                    bool grav = g_state.lowGravity != 0;
                    if (DrawToggleRow("##lowGrav", "Low gravity", &grav, scale)) {
                        g_state.lowGravity = grav ? 1 : 0;
                    }
                    if (grav) {
                        DrawSliderRow("##gravScale", "Gravity scale", &g_state.gravityScale, 0.10f, 1.00f, "%.2fx", scale);
                    }

                    bool ac = g_state.acBypass != 0;
                    if (DrawToggleRow("##acBypass", "Disable AC telemetry", &ac, scale, ac ? "SAFE" : nullptr, false)) {
                        g_state.acBypass = ac ? 1 : 0;
                    }
                } else {
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "%.0f FPS  |  %dx%d", ImGui::GetIO().Framerate, surfW, surfH);
                    DrawInfoRow("Render", buf, scale);

                    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(g_tele.nativeCamAddr));
                    DrawInfoRow("NativeCam", buf, scale, g_tele.nativeCamAddr ? ModTheme::kAccent : ModTheme::kWarnDot);

                    std::snprintf(buf, sizeof(buf), "Total %d  |  Enemies %d", g_tele.playerCount, g_tele.enemyCount);
                    DrawInfoRow("Players", buf, scale);

                    std::snprintf(buf, sizeof(buf), "%.1f, %.1f, %.1f", g_tele.localX, g_tele.localY, g_tele.localZ);
                    DrawInfoRow("Position", buf, scale);

                    std::snprintf(buf, sizeof(buf), "%.0f / %.0f  |  Team %d", g_tele.localHp, g_tele.localMaxHp, g_tele.localTeamId);
                    DrawInfoRow("Local", buf, scale);

                    if (g_tele.statusText[0]) {
                        DrawInfoRow("Status", g_tele.statusText, scale);
                    }

                    if (DrawActionRow("##btnRescan", "Rescan IL2CPP", "Rescan", scale, false)) {
                        g_state.reqRescan = 1;
                    }
                    if (DrawActionRow("##btnReset", "Reset all toggles", "Reset", scale, true)) {
                        ResetStateToDefaults();
                    }
                    if (DrawActionRow("##btnKill", "Unload mod menu", "Unload", scale, true, false)) {
                        g_state.reqKill = 1;
                    }
                }

                ImGui::PopStyleVar();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();

            outRects.push_back({wp.x, wp.y, wp.x + ws.x, wp.y + ws.y});
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}

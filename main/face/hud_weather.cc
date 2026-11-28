#include "hud_weather.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include <esp_log.h>
#include <lvgl.h>

#include "board.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "face/weather.h"
#include "music/music_tester.h"

#define TAG "SidePanel"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace {

// Right-hand column of the 320x240 screen: 80 px wide, full height.
constexpr int kPanelX = 240;
constexpr int kPanelW = 80;
constexpr int kPanelH = 240;

constexpr int kIconX = 260;
constexpr int kIconY = 8;
constexpr int kIconSize = 40;

constexpr int kDigitW = 22;
constexpr int kDigitH = 34;
constexpr int kDigitT = 5;
constexpr int kTensX = 256;
constexpr int kUnitsX = 282;
constexpr int kMinusX = 242;
constexpr int kDegreeX = 307;
constexpr int kDigitY = 64;

constexpr int kBarY = 212;
constexpr int kBarX = 270;

constexpr uint32_t kInk = 0xffffff;
constexpr uint32_t kSun = 0xffc94d;
constexpr uint32_t kCloud = 0xcfd8e3;
constexpr uint32_t kDrop = 0x5aa9ff;
constexpr uint32_t kSnow = 0xffffff;
constexpr uint32_t kFog = 0xb9c2cf;
constexpr uint32_t kAccent = 0x4fa3ff;

struct Digit {
    lv_obj_t* box = nullptr;
    lv_obj_t* seg[7] = {};
};

struct Panel {
    lv_obj_t* root = nullptr;
    Digit tens;
    Digit units;
    lv_obj_t* minus = nullptr;
    lv_obj_t* degree = nullptr;

    lv_obj_t* sun = nullptr;
    lv_obj_t* ray[4] = {};
    lv_obj_t* cloud[4] = {};
    lv_obj_t* drop[3] = {};
    lv_obj_t* flake[3] = {};
    lv_obj_t* fog[3] = {};
    lv_obj_t* bolt[2] = {};

    lv_obj_t* station_name = nullptr;
    const lv_font_t* station_font = nullptr;
    lv_obj_t* bar[3] = {};

    int last_temp = 1000;
    int last_code = -1;
    std::string last_station;
    bool last_playing = false;
    int tick = 0;
    bool built = false;
};

Panel g_panel;

lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 1, 0);
    return o;
}

void Hide(lv_obj_t* o) {
    if (o) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

void Show(lv_obj_t* o) {
    if (o) {
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

// The theme font is the one the whole UI draws Russian with: it is loaded from assets at boot
// (font_noto_sans_common_*) and carries the full Cyrillic set, while the compiled-in
// BUILTIN_TEXT_FONT subset is missing letters — it rendered «РАДИО» as «РАИО».
const lv_font_t* PanelFont() {
    auto* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        auto* theme = static_cast<LvglTheme*>(display->GetTheme());
        if (theme != nullptr && theme->text_font() != nullptr) {
            const lv_font_t* font = theme->text_font()->font();
            if (font != nullptr && font->dsc != BUILTIN_TEXT_FONT.dsc) {
                return font;
            }
        }
    }
    // Before the assets are applied the theme still wraps the compiled-in font in a heap object
    // that SetTextFont later destroys; hand out the static font instead of a dangling wrapper.
    return &BUILTIN_TEXT_FONT;
}

// segments: 0=a top, 1=b right-top, 2=c right-bottom, 3=d bottom, 4=e left-bottom,
// 5=f left-top, 6=g middle
void SetDigit(const Digit& d, int value) {
    static const int kMasks[10] = {63, 6, 91, 79, 102, 109, 125, 7, 127, 111};
    int mask = (value >= 0 && value <= 9) ? kMasks[value] : kMasks[8];
    for (int i = 0; i < 7; ++i) {
        if ((mask >> i) & 1) {
            Show(d.seg[i]);
        } else {
            Hide(d.seg[i]);
        }
    }
}

void BuildDigit(lv_obj_t* parent, int x, Digit& d) {
    const int w = kDigitW;
    const int h = kDigitH;
    const int t = kDigitT;
    d.box = lv_obj_create(parent);
    lv_obj_remove_style_all(d.box);
    lv_obj_clear_flag(d.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(d.box, x - kPanelX, kDigitY);
    lv_obj_set_size(d.box, w, h);
    lv_obj_set_style_bg_opa(d.box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(d.box, 0, 0);

    d.seg[0] = Box(d.box, 1, 0, w - 2, t, kInk);
    d.seg[1] = Box(d.box, w - t, 1, t, h / 2 - 1, kInk);
    d.seg[2] = Box(d.box, w - t, h / 2, t, h / 2 - 1, kInk);
    d.seg[3] = Box(d.box, 1, h - t, w - 2, t, kInk);
    d.seg[4] = Box(d.box, 0, h / 2, t, h / 2 - 1, kInk);
    d.seg[5] = Box(d.box, 0, 1, t, h / 2 - 1, kInk);
    d.seg[6] = Box(d.box, 1, h / 2 - t / 2, w - 2, t, kInk);
}

void BuildIcon(lv_obj_t* parent) {
    const int ix = kIconX - kPanelX;
    const int iy = kIconY;

    g_panel.sun = Box(parent, ix + 12, iy + 12, 18, 18, kSun);
    lv_obj_set_style_radius(g_panel.sun, LV_RADIUS_CIRCLE, 0);
    g_panel.ray[0] = Box(parent, ix + 19, iy + 2, 4, 8, kSun);
    g_panel.ray[1] = Box(parent, ix + 19, iy + 32, 4, 8, kSun);
    g_panel.ray[2] = Box(parent, ix + 2, iy + 19, 8, 4, kSun);
    g_panel.ray[3] = Box(parent, ix + 32, iy + 19, 8, 4, kSun);

    g_panel.cloud[0] = Box(parent, ix + 6, iy + 18, 18, 13, kCloud);
    lv_obj_set_style_radius(g_panel.cloud[0], LV_RADIUS_CIRCLE, 0);
    g_panel.cloud[1] = Box(parent, ix + 14, iy + 11, 20, 18, kCloud);
    lv_obj_set_style_radius(g_panel.cloud[1], LV_RADIUS_CIRCLE, 0);
    g_panel.cloud[2] = Box(parent, ix + 26, iy + 18, 12, 13, kCloud);
    lv_obj_set_style_radius(g_panel.cloud[2], LV_RADIUS_CIRCLE, 0);
    g_panel.cloud[3] = Box(parent, ix + 7, iy + 27, 28, 5, kCloud);

    for (int i = 0; i < 3; ++i) {
        g_panel.drop[i] = Box(parent, ix + 10 + i * 9, iy + 32, 3, 7, kDrop);
        g_panel.flake[i] = Box(parent, ix + 9 + i * 9, iy + 33, 5, 5, kSnow);
        lv_obj_set_style_radius(g_panel.flake[i], LV_RADIUS_CIRCLE, 0);
        g_panel.fog[i] = Box(parent, ix + 6, iy + 14 + i * 8, 28, 3, kFog);
    }
    g_panel.bolt[0] = Box(parent, ix + 17, iy + 30, 4, 6, kSun);
    g_panel.bolt[1] = Box(parent, ix + 21, iy + 35, 4, 6, kSun);
}

void HideIcon() {
    Hide(g_panel.sun);
    for (int i = 0; i < 4; ++i) {
        Hide(g_panel.ray[i]);
        Hide(g_panel.cloud[i]);
    }
    for (int i = 0; i < 3; ++i) {
        Hide(g_panel.drop[i]);
        Hide(g_panel.flake[i]);
        Hide(g_panel.fog[i]);
    }
    Hide(g_panel.bolt[0]);
    Hide(g_panel.bolt[1]);
}

void ApplyIcon(int code) {
    HideIcon();
    if (code == 0) {
        Show(g_panel.sun);
        for (int i = 0; i < 4; ++i) {
            Show(g_panel.ray[i]);
        }
    } else if (code <= 2) {
        Show(g_panel.sun);
        Show(g_panel.cloud[1]);
        Show(g_panel.cloud[3]);
    } else if (code == 3) {
        for (int i = 0; i < 4; ++i) {
            Show(g_panel.cloud[i]);
        }
    } else if (code == 45 || code == 48) {
        for (int i = 0; i < 3; ++i) {
            Show(g_panel.fog[i]);
        }
    } else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        for (int i = 0; i < 3; ++i) {
            Show(g_panel.cloud[i]);
            Show(g_panel.drop[i]);
        }
        Show(g_panel.cloud[3]);
    } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        for (int i = 0; i < 3; ++i) {
            Show(g_panel.cloud[i]);
            Show(g_panel.flake[i]);
        }
        Show(g_panel.cloud[3]);
    } else if (code >= 95) {
        for (int i = 0; i < 4; ++i) {
            Show(g_panel.cloud[i]);
        }
        Show(g_panel.bolt[0]);
        Show(g_panel.bolt[1]);
    } else {
        for (int i = 0; i < 4; ++i) {
            Show(g_panel.cloud[i]);
        }
    }
}

void ApplyTemperature(int temp) {
    int value = std::abs(temp);
    if (value > 99) {
        value = 99;
    }
    bool negative = temp < 0;
    bool has_tens = value >= 10;

    // The two digits live in a container each, so a single-digit temperature can be centred in
    // the same field instead of leaving the tens slot empty.
    const int single_x = kTensX - kPanelX + (kUnitsX - kTensX + kDigitW) / 2 - kDigitW / 2;
    if (has_tens) {
        lv_obj_set_x(g_panel.tens.box, kTensX - kPanelX);
        Show(g_panel.tens.box);
        SetDigit(g_panel.tens, value / 10);
        lv_obj_set_x(g_panel.units.box, kUnitsX - kPanelX);
    } else {
        Hide(g_panel.tens.box);
        lv_obj_set_x(g_panel.units.box, single_x);
    }
    SetDigit(g_panel.units, value % 10);

    int right_edge = has_tens ? kUnitsX + kDigitW : single_x + kPanelX + kDigitW;
    lv_obj_set_pos(g_panel.degree, right_edge + 2 - kPanelX, kDigitY + 2);
    Show(g_panel.degree);

    if (negative) {
        Show(g_panel.minus);
        int minus_x = has_tens ? kMinusX : single_x + kPanelX - 16;
        lv_obj_set_pos(g_panel.minus, minus_x - kPanelX, kDigitY + kDigitH / 2 - 2);
    } else {
        Hide(g_panel.minus);
    }
}

void Build() {
    lv_obj_t* top = lv_layer_top();
    g_panel.root = lv_obj_create(top);
    lv_obj_remove_style_all(g_panel.root);
    lv_obj_clear_flag(g_panel.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_panel.root, kPanelX, 0);
    lv_obj_set_size(g_panel.root, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(g_panel.root, lv_color_hex(0x0b0d13), 0);
    lv_obj_set_style_bg_opa(g_panel.root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_panel.root, 0, 0);

    BuildIcon(g_panel.root);
    BuildDigit(g_panel.root, kTensX, g_panel.tens);
    BuildDigit(g_panel.root, kUnitsX, g_panel.units);
    g_panel.minus = Box(g_panel.root, kMinusX - kPanelX, kDigitY + kDigitH / 2 - 2, 12, 5, kInk);
    g_panel.degree = Box(g_panel.root, kDegreeX - kPanelX, kDigitY + 4, 7, 7, kInk);
    lv_obj_set_style_radius(g_panel.degree, LV_RADIUS_CIRCLE, 0);

    Box(g_panel.root, 4, 112, kPanelW - 8, 1, 0x232838);

    g_panel.station_name = lv_label_create(g_panel.root);
    g_panel.station_font = PanelFont();
    lv_obj_set_style_text_font(g_panel.station_name, g_panel.station_font, 0);
    lv_obj_set_style_text_color(g_panel.station_name, lv_color_hex(0xf2f5fa), 0);
    lv_obj_set_style_text_align(g_panel.station_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(g_panel.station_name, 2, 0);
    lv_label_set_long_mode(g_panel.station_name, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(g_panel.station_name, 4, 118);
    lv_obj_set_width(g_panel.station_name, kPanelW - 8);
    g_panel.last_station = MusicTester::GetInstance().CurrentStationPanel();
    lv_label_set_text(g_panel.station_name,
                      g_panel.last_station.empty() ? "-" : g_panel.last_station.c_str());

    for (int i = 0; i < 3; ++i) {
        g_panel.bar[i] = Box(g_panel.root, kBarX - kPanelX + i * 8, kBarY, 4, 10, kAccent);
    }

    HideIcon();
    Hide(g_panel.tens.box);
    Hide(g_panel.units.box);
    Hide(g_panel.minus);
    Hide(g_panel.degree);
    for (int i = 0; i < 3; ++i) {
        Hide(g_panel.bar[i]);
    }

    g_panel.built = true;
    ESP_LOGI(TAG, "side panel built");
}

void Tick(lv_timer_t*) {
    if (!g_panel.built) {
        Build();
        return;
    }

    g_panel.tick++;

    // The panel is built before Application applies the assets font, so pick the theme font up
    // as soon as it is installed: only that one has the full Cyrillic set.
    const lv_font_t* font = PanelFont();
    if (font != g_panel.station_font) {
        g_panel.station_font = font;
        lv_obj_set_style_text_font(g_panel.station_name, font, 0);
        ESP_LOGI(TAG, "station label font refreshed");
    }

    int temp = 0;
    int code = -1;
    if (GetWeather(temp, code)) {
        if (temp != g_panel.last_temp) {
            g_panel.last_temp = temp;
            ApplyTemperature(temp);
        }
        if (code != g_panel.last_code) {
            g_panel.last_code = code;
            ApplyIcon(code);
        }
    }

    std::string status = MusicTester::GetInstance().Status();
    bool playing = status.rfind("playing radio", 0) == 0;

    std::string station = MusicTester::GetInstance().CurrentStationPanel();
    if (station != g_panel.last_station) {
        g_panel.last_station = station;
        lv_label_set_text(g_panel.station_name, station.empty() ? "-" : station.c_str());
    }

    if (playing != g_panel.last_playing) {
        g_panel.last_playing = playing;
        for (int i = 0; i < 3; ++i) {
            if (playing) {
                Show(g_panel.bar[i]);
            } else {
                Hide(g_panel.bar[i]);
            }
        }
    }

    if (playing) {
        // three bars dance so the column shows that the stream is alive
        static const int kHeights[3][3] = {{6, 14, 10}, {12, 6, 16}, {16, 10, 6}};
        int step = (g_panel.tick / 3) % 3;
        for (int i = 0; i < 3; ++i) {
            int h = kHeights[i][step];
            lv_obj_set_height(g_panel.bar[i], h);
            lv_obj_set_y(g_panel.bar[i], kBarY + (16 - h));
        }
    }
}

}  // namespace

void InitializeWeatherHud() {
    lv_timer_create(Tick, 500, nullptr);
    ESP_LOGI(TAG, "side panel timer started");
}

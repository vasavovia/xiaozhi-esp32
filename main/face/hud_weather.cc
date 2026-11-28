#include "hud_weather.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include <esp_log.h>
#include <lvgl.h>

#include "face/weather.h"
#include "music/music_tester.h"

#define TAG "SidePanel"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace {

// Right-hand column of the 320x240 screen: 80 px wide, full height.
constexpr int kPanelX = 240;
constexpr int kPanelW = 80;
constexpr int kPanelH = 240;

constexpr int kIconX = 263;
constexpr int kIconY = 8;
constexpr int kDigitW = 14;
constexpr int kDigitH = 22;
constexpr int kDigitT = 3;
constexpr int kTensX = 252;
constexpr int kUnitsX = 270;
constexpr int kMinusX = 241;
constexpr int kDigitY = 50;

constexpr uint32_t kInk = 0xe8edf5;
constexpr uint32_t kSun = 0xffc94d;
constexpr uint32_t kCloud = 0xcfd8e3;
constexpr uint32_t kDrop = 0x5aa9ff;
constexpr uint32_t kSnow = 0xffffff;
constexpr uint32_t kFog = 0xb9c2cf;

struct Digit {
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

    lv_obj_t* stations_label = nullptr;
    lv_obj_t* station_name = nullptr;

    int last_temp = 1000;
    int last_code = -1;
    std::string last_station;
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
    const int bx = x - kPanelX;
    const int by = kDigitY;
    d.seg[0] = Box(parent, bx + 1, by, w - 2, t, kInk);
    d.seg[1] = Box(parent, bx + w - t, by + 1, t, h / 2 - 1, kInk);
    d.seg[2] = Box(parent, bx + w - t, by + h / 2, t, h / 2 - 1, kInk);
    d.seg[3] = Box(parent, bx + 1, by + h - t, w - 2, t, kInk);
    d.seg[4] = Box(parent, bx, by + h / 2, t, h / 2 - 1, kInk);
    d.seg[5] = Box(parent, bx, by + 1, t, h / 2 - 1, kInk);
    d.seg[6] = Box(parent, bx + 1, by + h / 2 - t / 2, w - 2, t, kInk);
}

void BuildIcon(lv_obj_t* parent) {
    const int ix = kIconX - kPanelX;
    const int iy = kIconY;

    g_panel.sun = Box(parent, ix + 10, iy + 10, 15, 15, kSun);
    lv_obj_set_style_radius(g_panel.sun, LV_RADIUS_CIRCLE, 0);
    g_panel.ray[0] = Box(parent, ix + 16, iy + 2, 3, 6, kSun);
    g_panel.ray[1] = Box(parent, ix + 16, iy + 27, 3, 6, kSun);
    g_panel.ray[2] = Box(parent, ix + 2, iy + 16, 6, 3, kSun);
    g_panel.ray[3] = Box(parent, ix + 27, iy + 16, 6, 3, kSun);

    g_panel.cloud[0] = Box(parent, ix + 5, iy + 15, 15, 11, kCloud);
    lv_obj_set_style_radius(g_panel.cloud[0], LV_RADIUS_CIRCLE, 0);
    g_panel.cloud[1] = Box(parent, ix + 12, iy + 10, 17, 16, kCloud);
    lv_obj_set_style_radius(g_panel.cloud[1], LV_RADIUS_CIRCLE, 0);
    g_panel.cloud[2] = Box(parent, ix + 21, iy + 15, 10, 11, kCloud);
    lv_obj_set_style_radius(g_panel.cloud[2], LV_RADIUS_CIRCLE, 0);
    g_panel.cloud[3] = Box(parent, ix + 6, iy + 23, 24, 4, kCloud);

    for (int i = 0; i < 3; ++i) {
        g_panel.drop[i] = Box(parent, ix + 9 + i * 8, iy + 27, 2, 5, kDrop);
        g_panel.flake[i] = Box(parent, ix + 8 + i * 8, iy + 28, 4, 4, kSnow);
        lv_obj_set_style_radius(g_panel.flake[i], LV_RADIUS_CIRCLE, 0);
        g_panel.fog[i] = Box(parent, ix + 5, iy + 12 + i * 7, 25, 2, kFog);
    }
    g_panel.bolt[0] = Box(parent, ix + 14, iy + 25, 3, 5, kSun);
    g_panel.bolt[1] = Box(parent, ix + 17, iy + 29, 3, 5, kSun);
}

void HideIcon() {
    Hide(g_panel.sun);
    for (int i = 0; i < 4; ++i) {
        Hide(g_panel.ray[i]);
    }
    for (int i = 0; i < 4; ++i) {
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
        for (int i = 0; i < 4; ++i) {
            Show(g_panel.cloud[i]);
        }
        for (int i = 0; i < 3; ++i) {
            Show(g_panel.drop[i]);
        }
    } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        for (int i = 0; i < 4; ++i) {
            Show(g_panel.cloud[i]);
        }
        for (int i = 0; i < 3; ++i) {
            Show(g_panel.flake[i]);
        }
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

    if (has_tens) {
        SetDigit(g_panel.tens, value / 10);
    } else {
        for (int i = 0; i < 7; ++i) {
            Hide(g_panel.tens.seg[i]);
        }
    }
    SetDigit(g_panel.units, value % 10);
    Show(g_panel.degree);

    if (negative) {
        Show(g_panel.minus);
        lv_obj_set_pos(g_panel.minus, kMinusX - kPanelX + (has_tens ? 0 : 18),
                       kDigitY + kDigitH / 2 - 2);
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
    lv_obj_set_style_bg_color(g_panel.root, lv_color_hex(0x0d0f15), 0);
    lv_obj_set_style_bg_opa(g_panel.root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_panel.root, 0, 0);

    BuildIcon(g_panel.root);

    BuildDigit(g_panel.root, kTensX, g_panel.tens);
    BuildDigit(g_panel.root, kUnitsX, g_panel.units);
    g_panel.minus = Box(g_panel.root, kMinusX - kPanelX, kDigitY + kDigitH / 2 - 2, 9, 3, kInk);
    g_panel.degree = Box(g_panel.root, 288 - kPanelX, kDigitY + 4, 5, 5, kInk);
    lv_obj_set_style_radius(g_panel.degree, LV_RADIUS_CIRCLE, 0);

    // separators
    Box(g_panel.root, 6, 78, kPanelW - 12, 1, 0x2a2f3a);
    Box(g_panel.root, 6, 196, kPanelW - 12, 1, 0x2a2f3a);

    g_panel.stations_label = lv_label_create(g_panel.root);
    lv_obj_set_style_text_font(g_panel.stations_label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_panel.stations_label, lv_color_hex(0x8f9aab), 0);
    lv_label_set_text(g_panel.stations_label, "РАДИО");
    lv_obj_set_pos(g_panel.stations_label, 6, 86);
    lv_obj_set_width(g_panel.stations_label, kPanelW - 12);

    g_panel.station_name = lv_label_create(g_panel.root);
    lv_obj_set_style_text_font(g_panel.station_name, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_panel.station_name, lv_color_hex(0xf2f5fa), 0);
    lv_label_set_long_mode(g_panel.station_name, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_panel.station_name, "—");
    lv_obj_set_pos(g_panel.station_name, 6, 106);
    lv_obj_set_width(g_panel.station_name, kPanelW - 12);

    HideIcon();
    for (int i = 0; i < 7; ++i) {
        Hide(g_panel.tens.seg[i]);
        Hide(g_panel.units.seg[i]);
    }
    Hide(g_panel.minus);
    Hide(g_panel.degree);

    g_panel.built = true;
    ESP_LOGI(TAG, "side panel built");
}

void Tick(lv_timer_t*) {
    if (!g_panel.built) {
        Build();
        return;
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
    static const char* kPrefix = "playing radio: ";
    std::string station;
    if (status.rfind(kPrefix, 0) == 0) {
        station = status.substr(std::strlen(kPrefix));
    }
    if (station != g_panel.last_station) {
        g_panel.last_station = station;
        lv_label_set_text(g_panel.station_name, station.empty() ? "выключено" : station.c_str());
    }
}

}  // namespace

void InitializeWeatherHud() {
    lv_timer_create(Tick, 1000, nullptr);
    ESP_LOGI(TAG, "side panel timer started");
}

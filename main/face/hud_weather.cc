#include "hud_weather.h"

#include <cstdlib>

#include <esp_log.h>
#include <lvgl.h>

#include "face/weather.h"

#define TAG "WeatherHud"

namespace {

// panel geometry (top-right of the 240x320 panel)
constexpr int kPanelX = 118;
constexpr int kPanelY = 2;
constexpr int kPanelW = 118;
constexpr int kPanelH = 34;

constexpr int kIconX = 122;
constexpr int kIconY = 4;
constexpr int kDigitW = 12;
constexpr int kDigitH = 20;
constexpr int kDigitT = 3;
constexpr int kTensX = 168;
constexpr int kUnitsX = 182;
constexpr int kMinusX = 158;
constexpr int kDigitY = 8;

constexpr uint32_t kInk = 0xe8edf5;
constexpr uint32_t kSun = 0xffc94d;
constexpr uint32_t kCloud = 0xcfd8e3;
constexpr uint32_t kDrop = 0x5aa9ff;
constexpr uint32_t kSnow = 0xffffff;
constexpr uint32_t kFog = 0xb9c2cf;

struct Digit {
    lv_obj_t* seg[7] = {};
};

struct Hud {
    lv_obj_t* panel = nullptr;
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

    int last_temp = 1000;
    int last_code = -1;
    bool built = false;
};

Hud g_hud;

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
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void Show(lv_obj_t* o) {
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// segments: bit0=a top, 1=b right-top, 2=c right-bottom, 3=d bottom, 4=e left-bottom,
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
    (void)kDigitT;
}

void BuildDigit(lv_obj_t* parent, int x, Digit& d) {
    const int w = kDigitW;
    const int h = kDigitH;
    const int t = kDigitT;
    d.seg[0] = Box(parent, x + 1, 0, w - 2, t, kInk);                 // a
    d.seg[1] = Box(parent, x + w - t, 1, t, h / 2 - 1, kInk);         // b
    d.seg[2] = Box(parent, x + w - t, h / 2, t, h / 2 - 1, kInk);     // c
    d.seg[3] = Box(parent, x + 1, h - t, w - 2, t, kInk);             // d
    d.seg[4] = Box(parent, x, h / 2, t, h / 2 - 1, kInk);             // e
    d.seg[5] = Box(parent, x, 1, t, h / 2 - 1, kInk);                 // f
    d.seg[6] = Box(parent, x + 1, h / 2 - t / 2, w - 2, t, kInk);     // g
}

void BuildIcon(lv_obj_t* parent) {
    const int ix = kIconX - kPanelX;
    const int iy = kIconY - kPanelY;

    g_hud.sun = Box(parent, ix + 8, iy + 8, 14, 14, kSun);
    lv_obj_set_style_radius(g_hud.sun, LV_RADIUS_CIRCLE, 0);
    g_hud.ray[0] = Box(parent, ix + 13, iy + 1, 3, 5, kSun);
    g_hud.ray[1] = Box(parent, ix + 13, iy + 24, 3, 5, kSun);
    g_hud.ray[2] = Box(parent, ix + 1, iy + 13, 5, 3, kSun);
    g_hud.ray[3] = Box(parent, ix + 24, iy + 13, 5, 3, kSun);

    g_hud.cloud[0] = Box(parent, ix + 4, iy + 13, 13, 10, kCloud);
    lv_obj_set_style_radius(g_hud.cloud[0], LV_RADIUS_CIRCLE, 0);
    g_hud.cloud[1] = Box(parent, ix + 10, iy + 8, 15, 14, kCloud);
    lv_obj_set_style_radius(g_hud.cloud[1], LV_RADIUS_CIRCLE, 0);
    g_hud.cloud[2] = Box(parent, ix + 18, iy + 13, 9, 10, kCloud);
    lv_obj_set_style_radius(g_hud.cloud[2], LV_RADIUS_CIRCLE, 0);
    g_hud.cloud[3] = Box(parent, ix + 5, iy + 20, 21, 4, kCloud);

    for (int i = 0; i < 3; ++i) {
        g_hud.drop[i] = Box(parent, ix + 7 + i * 7, iy + 24, 2, 5, kDrop);
        g_hud.flake[i] = Box(parent, ix + 6 + i * 7, iy + 25, 4, 4, kSnow);
        lv_obj_set_style_radius(g_hud.flake[i], LV_RADIUS_CIRCLE, 0);
        g_hud.fog[i] = Box(parent, ix + 4, iy + 10 + i * 6, 22, 2, kFog);
    }
    g_hud.bolt[0] = Box(parent, ix + 12, iy + 22, 3, 5, kSun);
    g_hud.bolt[1] = Box(parent, ix + 15, iy + 26, 3, 5, kSun);
}

void HideIcon() {
    if (g_hud.sun) {
        Hide(g_hud.sun);
    }
    for (int i = 0; i < 4; ++i) {
        if (g_hud.ray[i]) Hide(g_hud.ray[i]);
    }
    for (int i = 0; i < 4; ++i) {
        if (g_hud.cloud[i]) Hide(g_hud.cloud[i]);
    }
    for (int i = 0; i < 3; ++i) {
        if (g_hud.drop[i]) Hide(g_hud.drop[i]);
        if (g_hud.flake[i]) Hide(g_hud.flake[i]);
        if (g_hud.fog[i]) Hide(g_hud.fog[i]);
    }
    if (g_hud.bolt[0]) Hide(g_hud.bolt[0]);
    if (g_hud.bolt[1]) Hide(g_hud.bolt[1]);
}

void ShowSun() {
    Show(g_hud.sun);
    for (int i = 0; i < 4; ++i) {
        Show(g_hud.ray[i]);
    }
}

void ShowCloud() {
    for (int i = 0; i < 4; ++i) {
        Show(g_hud.cloud[i]);
    }
}

void ApplyIcon(int code) {
    HideIcon();
    if (code == 0) {
        ShowSun();
    } else if (code <= 2) {
        ShowSun();
        Show(g_hud.cloud[1]);
        Show(g_hud.cloud[3]);
    } else if (code == 3) {
        ShowCloud();
    } else if (code == 45 || code == 48) {
        for (int i = 0; i < 3; ++i) {
            Show(g_hud.fog[i]);
        }
    } else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        ShowCloud();
        for (int i = 0; i < 3; ++i) {
            Show(g_hud.drop[i]);
        }
    } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        ShowCloud();
        for (int i = 0; i < 3; ++i) {
            Show(g_hud.flake[i]);
        }
    } else if (code >= 95) {
        ShowCloud();
        Show(g_hud.bolt[0]);
        Show(g_hud.bolt[1]);
    } else {
        ShowCloud();
    }
}

void ApplyTemperature(int temp) {
    int value = std::abs(temp);
    if (value > 99) {
        value = 99;
    }
    bool negative = temp < 0;
    bool has_tens = value >= 10;
    Show(g_hud.degree);

    if (has_tens) {
        Show(g_hud.tens.seg[0]);
        SetDigit(g_hud.tens, value / 10);
    } else {
        for (int i = 0; i < 7; ++i) {
            Hide(g_hud.tens.seg[i]);
        }
    }
    SetDigit(g_hud.units, value % 10);

    if (negative) {
        Show(g_hud.minus);
        lv_obj_set_pos(g_hud.minus, has_tens ? kMinusX - kPanelX : kMinusX - kPanelX + 18,
                       kDigitY + kDigitH / 2 - 2);
    } else {
        Hide(g_hud.minus);
    }
}

void Build() {
    lv_obj_t* top = lv_layer_top();
    g_hud.panel = lv_obj_create(top);
    lv_obj_remove_style_all(g_hud.panel);
    lv_obj_clear_flag(g_hud.panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_hud.panel, kPanelX, kPanelY);
    lv_obj_set_size(g_hud.panel, kPanelW, kPanelH);
    lv_obj_set_style_bg_color(g_hud.panel, lv_color_hex(0x11131c), 0);
    lv_obj_set_style_bg_opa(g_hud.panel, 175, 0);
    lv_obj_set_style_radius(g_hud.panel, 9, 0);

    BuildIcon(g_hud.panel);

    BuildDigit(g_hud.panel, kTensX - kPanelX, g_hud.tens);
    BuildDigit(g_hud.panel, kUnitsX - kPanelX, g_hud.units);

    g_hud.minus = Box(g_hud.panel, kMinusX - kPanelX, kDigitY + kDigitH / 2 - 2, 8, 3, kInk);

    g_hud.degree = Box(g_hud.panel, 200 - kPanelX, 8, 5, 5, kInk);
    lv_obj_set_style_radius(g_hud.degree, LV_RADIUS_CIRCLE, 0);

    HideIcon();
    for (int i = 0; i < 7; ++i) {
        Hide(g_hud.tens.seg[i]);
        Hide(g_hud.units.seg[i]);
    }
    Hide(g_hud.minus);
    Hide(g_hud.degree);
    g_hud.built = true;
    ESP_LOGI(TAG, "weather hud built");
}

void Tick(lv_timer_t*) {
    if (!g_hud.built) {
        Build();
        return;
    }
    int temp = 0;
    int code = -1;
    if (!GetWeather(temp, code)) {
        return;
    }
    if (temp != g_hud.last_temp) {
        g_hud.last_temp = temp;
        ApplyTemperature(temp);
    }
    if (code != g_hud.last_code) {
        g_hud.last_code = code;
        ApplyIcon(code);
    }
}

}  // namespace

void InitializeWeatherHud() {
    lv_timer_create(Tick, 1000, nullptr);
    ESP_LOGI(TAG, "weather hud timer started");
}

#include "dashboard.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include "application.h"
#include "board.h"
#include "device_state.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "face/face_portrait.h"
#include "face/weather.h"
#include "music/music_tester.h"

#define TAG "Dashboard"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace {

// ---------------------------------------------------------------- palette
constexpr uint32_t kBg = 0x0b2226;
constexpr uint32_t kBarBg = 0x081a1d;
constexpr uint32_t kCardBg = 0x103034;
constexpr uint32_t kCardLine = 0x2d6f78;
constexpr uint32_t kCyan = 0x7fe3e8;
constexpr uint32_t kDim = 0x4f9aa3;
constexpr uint32_t kWhite = 0xf2fbfc;
constexpr uint32_t kGreen = 0x63d69b;
constexpr uint32_t kWarm = 0xffb066;
constexpr uint32_t kBlue = 0x4fa3ff;
constexpr uint32_t kAccent = 0x123b40;

// ---------------------------------------------------------------- geometry
// Screen is 320x240: top bar 0..26, content 28..208, bottom strip 210..240.
constexpr int kScreenW = 320;
constexpr int kScreenH = 240;

constexpr int kLeftX = 4;
constexpr int kLeftY = 28;
constexpr int kLeftW = 150;
constexpr int kLeftH = 180;
constexpr int kPortraitInset = 6;
constexpr int kPortraitW = kLeftW - 2 * kPortraitInset;
constexpr int kPortraitH = kLeftH - 2 * kPortraitInset;

constexpr int kRightX = 158;
constexpr int kRightW = 158;
constexpr int kMediaY = 28;
constexpr int kMediaH = 76;
constexpr int kWindY = 108;
constexpr int kWindH = 26;
constexpr int kWeatherY = 138;
constexpr int kWeatherH = 70;

// ---------------------------------------------------------------- state
struct Digit {
    lv_obj_t* box = nullptr;
    lv_obj_t* seg[7] = {};
};

struct Ui {
    lv_obj_t* root = nullptr;

    lv_obj_t* wifi[3] = {};
    lv_obj_t* state_text = nullptr;
    lv_obj_t* clock_text = nullptr;
    lv_obj_t* volume_text = nullptr;

    lv_obj_t* frame = nullptr;
    const lv_font_t* font = nullptr;

    lv_obj_t* station_text = nullptr;
    lv_obj_t* header_text = nullptr;
    lv_obj_t* city_text = nullptr;
    lv_obj_t* bar[3] = {};

    lv_obj_t* wind_text = nullptr;
    lv_obj_t* wind_label = nullptr;

    Digit tens;
    Digit units;
    lv_obj_t* degree = nullptr;
    lv_obj_t* minus = nullptr;

    lv_obj_t* sun = nullptr;
    lv_obj_t* ray[4] = {};
    lv_obj_t* cloud[4] = {};
    lv_obj_t* drop[3] = {};
    lv_obj_t* flake[3] = {};
    lv_obj_t* fog[3] = {};
    lv_obj_t* bolt[2] = {};

    int last_temp = 1000;
    int last_code = -1;
    int last_wind10 = -1;
    int last_volume = -1;
    int last_rssi = 1000;
    int last_state = -1;
    std::string last_clock;
    std::string last_station;
    bool last_playing = false;
    int tick = 0;
    bool built = false;
};

Ui g_ui;

// ---------------------------------------------------------------- helpers
lv_obj_t* Panel(lv_obj_t* parent, int x, int y, int w, int h, uint32_t bg, int radius,
                uint32_t border, int border_width) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    if (border_width > 0) {
        lv_obj_set_style_border_width(o, border_width, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
        lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    }
    return o;
}

lv_obj_t* Rect(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius = 0) {
    return Panel(parent, x, y, w, h, color, radius, 0, 0);
}

lv_obj_t* Ring(lv_obj_t* parent, int x, int y, int size, uint32_t color, int width) {
    lv_obj_t* o = Panel(parent, x, y, size, size, color, LV_RADIUS_CIRCLE, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, width, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    return o;
}

lv_obj_t* Poly(lv_obj_t* parent, int x, int y, const lv_point_precise_t* points, int count,
               uint32_t color, int width) {
    lv_obj_t* o = lv_line_create(parent);
    lv_line_set_points(o, points, count);
    lv_obj_set_style_line_width(o, width, 0);
    lv_obj_set_style_line_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(o, true, 0);
    lv_obj_set_pos(o, x, y);
    return o;
}

lv_obj_t* Text(lv_obj_t* parent, int x, int y, int w, const char* text, uint32_t color,
               lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, g_ui.font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(l, x, y);
    if (w > 0) {
        lv_obj_set_width(l, w);
    }
    lv_label_set_text(l, text);
    return l;
}

// The theme font is the one the whole UI draws Russian with: it is loaded from assets at boot
// (font_noto_sans_common_*) and carries the full Cyrillic set, while the compiled-in
// BUILTIN_TEXT_FONT subset is missing letters — it rendered «РАДИО» as «РАИО».
const lv_font_t* ThemeFont() {
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

// ---------------------------------------------------------------- top bar
void BuildTopBar(lv_obj_t* root) {
    lv_obj_t* bar = Panel(root, 0, 0, kScreenW, 26, kBarBg, 0, 0, 0);
    Rect(root, 0, 25, kScreenW, 1, kCardLine);

    // signal bars, lit from the RSSI the Wi-Fi driver reports
    for (int i = 0; i < 3; ++i) {
        int h = 4 + i * 2;
        g_ui.wifi[i] = Rect(bar, 6 + i * 5, 19 - h, 3, h, kCardLine, 1);
    }

    g_ui.state_text = Text(bar, 24, 0, 140, "\xd0\x93\xd0\xbe\xd1\x82\xd0\xbe\xd0\xb2", kGreen);
    g_ui.clock_text = Text(bar, 186, 0, 56, "--:--", kWhite, LV_TEXT_ALIGN_RIGHT);

    // speaker glyph: body plus two sound chevrons
    Rect(bar, 250, 10, 4, 6, kCyan, 1);
    static const lv_point_precise_t kWave1[] = {{255, 9}, {257, 13}, {255, 17}};
    static const lv_point_precise_t kWave2[] = {{259, 7}, {262, 13}, {259, 19}};
    Poly(bar, 0, 0, kWave1, 3, kCyan, 1);
    Poly(bar, 0, 0, kWave2, 3, kCyan, 1);
    g_ui.volume_text = Text(bar, 280, 0, 32, "0", kWhite, LV_TEXT_ALIGN_RIGHT);
}

// ---------------------------------------------------------------- portrait
void BuildPortrait(lv_obj_t* root) {
    lv_obj_t* card = Panel(root, kLeftX, kLeftY, kLeftW, kLeftH, kCardBg, 10, kCardLine, 1);

    lv_obj_t* holder = Panel(card, kPortraitInset, kPortraitInset, kPortraitW, kPortraitH, kBg, 8,
                             0, 0);
    lv_obj_set_style_clip_corner(holder, true, 0);

    lv_obj_t* picture = lv_image_create(holder);
    lv_image_set_src(picture, &face_portrait);
    lv_obj_set_pos(picture, 0, 0);

    // border drawn after the picture so it stays on top; its colour tracks the device state
    g_ui.frame = Panel(card, kPortraitInset, kPortraitInset, kPortraitW, kPortraitH, kBg, 8,
                       kCardLine, 2);
    lv_obj_set_style_bg_opa(g_ui.frame, LV_OPA_TRANSP, 0);
}

// ---------------------------------------------------------------- media card
void BuildChevrons(lv_obj_t* parent, int x, uint32_t color, bool left) {
    static const lv_point_precise_t kLeft[] = {{6, 0}, {0, 6}, {6, 12}};
    static const lv_point_precise_t kRight[] = {{0, 0}, {6, 6}, {0, 12}};
    const lv_point_precise_t* shape = left ? kLeft : kRight;
    Poly(parent, x, 58, shape, 3, color, 2);
    Poly(parent, x + 7, 58, shape, 3, color, 2);
}

void BuildMedia(lv_obj_t* root) {
    lv_obj_t* card = Panel(root, kRightX, kMediaY, kRightW, kMediaH, kCardBg, 8, kCardLine, 1);

    g_ui.header_text = Text(card, 8, 2, kRightW - 16,
                            "\xd0\xa1\xd0\xb5\xd0\xb9\xd1\x87\xd0\xb0\xd1\x81 "
                            "\xd0\xb8\xd0\xb3\xd1\x80\xd0\xb0\xd0\xb5\xd1\x82",
                            kDim);

    g_ui.station_text = lv_label_create(card);
    lv_obj_set_style_text_font(g_ui.station_text, g_ui.font, 0);
    lv_obj_set_style_text_color(g_ui.station_text, lv_color_hex(kWhite), 0);
    lv_obj_set_style_text_align(g_ui.station_text, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(g_ui.station_text, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(g_ui.station_text, 8, 28);
    lv_obj_set_width(g_ui.station_text, kRightW - 16);
    g_ui.last_station = MusicTester::GetInstance().CurrentStation();
    lv_label_set_text(g_ui.station_text,
                      g_ui.last_station.empty() ? "-" : g_ui.last_station.c_str());

    BuildChevrons(card, 10, kDim, true);
    BuildChevrons(card, kRightW - 27, kDim, false);

    for (int i = 0; i < 3; ++i) {
        g_ui.bar[i] = Rect(card, 70 + i * 8, 64, 4, 8, kCyan, 1);
    }
}

// ---------------------------------------------------------------- wind card
void BuildWind(lv_obj_t* root) {
    lv_obj_t* card = Panel(root, kRightX, kWindY, kRightW, kWindH, kCardBg, 8, kCardLine, 1);
    g_ui.wind_label = Text(card, 8, -2, 100,
                           "\xd0\x92\xd0\xb5\xd1\x82\xd0\xb5\xd1\x80, \xd0\xbc/\xd1\x81", kDim);
    g_ui.wind_text = Text(card, 108, -2, 42, "--", kWhite, LV_TEXT_ALIGN_RIGHT);
}

// ---------------------------------------------------------------- weather card
void BuildDigit(lv_obj_t* parent, int x, int y, int w, int h, int t, Digit& d) {
    d.box = Panel(parent, x, y, w, h, kBg, 0, 0, 0);
    lv_obj_set_style_bg_opa(d.box, LV_OPA_TRANSP, 0);

    d.seg[0] = Rect(d.box, 1, 0, w - 2, t, kCyan, 1);
    d.seg[1] = Rect(d.box, w - t, 1, t, h / 2 - 1, kCyan, 1);
    d.seg[2] = Rect(d.box, w - t, h / 2, t, h / 2 - 1, kCyan, 1);
    d.seg[3] = Rect(d.box, 1, h - t, w - 2, t, kCyan, 1);
    d.seg[4] = Rect(d.box, 0, h / 2, t, h / 2 - 1, kCyan, 1);
    d.seg[5] = Rect(d.box, 0, 1, t, h / 2 - 1, kCyan, 1);
    d.seg[6] = Rect(d.box, 1, h / 2 - t / 2, w - 2, t, kCyan, 1);
}

void BuildWeatherIcon(lv_obj_t* parent) {
    const int ix = 110;
    const int iy = 26;

    g_ui.sun = Ring(parent, ix + 12, iy + 12, 18, kWarm, 2);
    g_ui.ray[0] = Rect(parent, ix + 19, iy + 2, 3, 7, kWarm, 1);
    g_ui.ray[1] = Rect(parent, ix + 19, iy + 33, 3, 7, kWarm, 1);
    g_ui.ray[2] = Rect(parent, ix + 2, iy + 19, 7, 3, kWarm, 1);
    g_ui.ray[3] = Rect(parent, ix + 33, iy + 19, 7, 3, kWarm, 1);

    g_ui.cloud[0] = Rect(parent, ix + 5, iy + 20, 18, 12, kCyan, 6);
    g_ui.cloud[1] = Rect(parent, ix + 13, iy + 13, 19, 16, kCyan, 8);
    g_ui.cloud[2] = Rect(parent, ix + 27, iy + 20, 10, 12, kCyan, 5);
    g_ui.cloud[3] = Rect(parent, ix + 6, iy + 27, 27, 5, kCyan, 2);

    for (int i = 0; i < 3; ++i) {
        g_ui.drop[i] = Rect(parent, ix + 10 + i * 8, iy + 31, 2, 7, kBlue, 1);
        g_ui.flake[i] = Ring(parent, ix + 9 + i * 8, iy + 32, 5, kWhite, 1);
        g_ui.fog[i] = Rect(parent, ix + 5, iy + 15 + i * 7, 30, 2, kDim, 1);
    }
    static const lv_point_precise_t kBolt[] = {{0, 0}, {4, 0}, {1, 7}, {5, 7}, {-1, 18}};
    g_ui.bolt[0] = Poly(parent, ix + 17, iy + 26, kBolt, 5, kWarm, 2);
}

void BuildWeather(lv_obj_t* root) {
    lv_obj_t* card =
        Panel(root, kRightX, kWeatherY, kRightW, kWeatherH, kCardBg, 8, kCardLine, 1);

    g_ui.city_text = Text(card, 8, 0, 60, "\xd0\x95\xd0\x9a\xd0\x91", kDim);

    BuildDigit(card, 10, 26, 26, 40, 6, g_ui.tens);
    BuildDigit(card, 40, 26, 26, 40, 6, g_ui.units);
    g_ui.degree = Rect(card, 70, 28, 6, 6, kCyan, 3);
    g_ui.minus = Rect(card, 0, 44, 8, 4, kCyan, 1);

    BuildWeatherIcon(card);
}

// ---------------------------------------------------------------- bottom strip
void BuildNavIcon(lv_obj_t* root, int center, int index) {
    const int x = center - 8;
    const int y = 217;

    if (index == 0) {
        Panel(root, center - 13, 212, 26, 24, kAccent, 12, 0, 0);
    }

    switch (index) {
        case 0: {  // home
            static const lv_point_precise_t kRoof[] = {{2, 8}, {8, 2}, {14, 8}};
            static const lv_point_precise_t kBody[] = {{4, 8}, {4, 14}, {12, 14}, {12, 8}};
            Poly(root, x, y, kRoof, 3, kCyan, 2);
            Poly(root, x, y, kBody, 4, kCyan, 2);
            break;
        }
        case 1: {  // radio
            static const lv_point_precise_t kCase[] = {{2, 13}, {2, 6}, {14, 6}, {14, 13}, {2, 13}};
            static const lv_point_precise_t kAntenna[] = {{10, 6}, {15, 1}};
            Poly(root, x, y, kCase, 5, kCyan, 2);
            Poly(root, x, y, kAntenna, 2, kCyan, 1);
            Ring(root, x + 4, y + 8, 4, kDim, 1);
            Rect(root, x + 10, y + 9, 2, 2, kDim, 1);
            break;
        }
        case 2: {  // music note
            static const lv_point_precise_t kStem[] = {{6, 12}, {6, 3}, {13, 1}};
            Poly(root, x, y, kStem, 3, kCyan, 2);
            Rect(root, x + 2, y + 11, 5, 4, kCyan, 2);
            break;
        }
        case 3: {  // library
            static const lv_point_precise_t kFrame[] = {{2, 4}, {14, 4}, {14, 14}, {2, 14}, {2, 4}};
            static const lv_point_precise_t kPlay[] = {{6, 7}, {6, 12}, {10, 9}, {6, 7}};
            Poly(root, x, y, kFrame, 5, kCyan, 1);
            Poly(root, x, y, kPlay, 4, kCyan, 1);
            break;
        }
        case 4: {  // cloud
            Rect(root, x + 2, y + 8, 11, 5, kCyan, 3);
            Rect(root, x + 6, y + 4, 7, 7, kCyan, 4);
            break;
        }
        case 5: {  // camera
            static const lv_point_precise_t kBody[] = {{2, 8}, {14, 8}, {14, 14}, {2, 14}, {2, 8}};
            static const lv_point_precise_t kHump[] = {{6, 8}, {7, 5}, {9, 5}, {10, 8}};
            Poly(root, x, y, kBody, 5, kCyan, 1);
            Poly(root, x, y, kHump, 4, kCyan, 1);
            Ring(root, x + 6, y + 9, 5, kCyan, 1);
            break;
        }
        default: {  // gear
            Ring(root, x + 3, y + 3, 10, kCyan, 2);
            Rect(root, x + 7, y, 2, 3, kCyan, 1);
            Rect(root, x + 7, y + 13, 2, 3, kCyan, 1);
            Rect(root, x, y + 7, 3, 2, kCyan, 1);
            Rect(root, x + 13, y + 7, 3, 2, kCyan, 1);
            break;
        }
    }
}

void BuildNav(lv_obj_t* root) {
    Panel(root, 0, 210, kScreenW, 30, kBarBg, 0, 0, 0);
    Rect(root, 0, 210, kScreenW, 1, kCardLine);
    for (int i = 0; i < 7; ++i) {
        BuildNavIcon(root, 23 + i * 45, i);
    }
}

// ---------------------------------------------------------------- weather drawing
void HideWeatherIcon() {
    lv_obj_add_flag(g_ui.sun, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 4; ++i) {
        lv_obj_add_flag(g_ui.ray[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_ui.cloud[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 3; ++i) {
        lv_obj_add_flag(g_ui.drop[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_ui.flake[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_ui.fog[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(g_ui.bolt[0], LV_OBJ_FLAG_HIDDEN);
}

void Show(lv_obj_t* o) {
    if (o) {
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

void ApplyWeatherIcon(int code) {
    HideWeatherIcon();
    if (code == 0) {
        Show(g_ui.sun);
        for (int i = 0; i < 4; ++i) {
            Show(g_ui.ray[i]);
        }
    } else if (code <= 2) {
        Show(g_ui.sun);
        Show(g_ui.cloud[1]);
        Show(g_ui.cloud[3]);
    } else if (code == 3) {
        for (int i = 0; i < 4; ++i) {
            Show(g_ui.cloud[i]);
        }
    } else if (code == 45 || code == 48) {
        for (int i = 0; i < 3; ++i) {
            Show(g_ui.fog[i]);
        }
    } else if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        for (int i = 0; i < 3; ++i) {
            Show(g_ui.cloud[i]);
            Show(g_ui.drop[i]);
        }
        Show(g_ui.cloud[3]);
    } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        for (int i = 0; i < 3; ++i) {
            Show(g_ui.cloud[i]);
            Show(g_ui.flake[i]);
        }
        Show(g_ui.cloud[3]);
    } else if (code >= 95) {
        for (int i = 0; i < 4; ++i) {
            Show(g_ui.cloud[i]);
        }
        Show(g_ui.bolt[0]);
    } else {
        for (int i = 0; i < 4; ++i) {
            Show(g_ui.cloud[i]);
        }
    }
}

void SetDigit(const Digit& d, int value) {
    static const int kMasks[10] = {63, 6, 91, 79, 102, 109, 125, 7, 127, 111};
    int mask = (value >= 0 && value <= 9) ? kMasks[value] : kMasks[8];
    for (int i = 0; i < 7; ++i) {
        if ((mask >> i) & 1) {
            Show(d.seg[i]);
        } else {
            lv_obj_add_flag(d.seg[i], LV_OBJ_FLAG_HIDDEN);
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

    // one digit is centred in the two-digit field instead of leaving the tens slot empty
    Show(g_ui.units.box);
    if (has_tens) {
        lv_obj_set_x(g_ui.tens.box, 10);
        Show(g_ui.tens.box);
        SetDigit(g_ui.tens, value / 10);
        lv_obj_set_x(g_ui.units.box, 40);
        lv_obj_set_x(g_ui.degree, 70);
    } else {
        lv_obj_add_flag(g_ui.tens.box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(g_ui.units.box, 25);
        lv_obj_set_x(g_ui.degree, 55);
    }
    SetDigit(g_ui.units, value % 10);
    Show(g_ui.degree);

    if (negative) {
        lv_obj_set_x(g_ui.minus, has_tens ? 0 : 15);
        Show(g_ui.minus);
    } else {
        lv_obj_add_flag(g_ui.minus, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------- clock
// SNTP is started from a task that waits for the station address, so the poll cannot run
// against an interface that does not exist yet.
void ClockTask(void*) {
    for (int attempt = 0; attempt < 150; ++attempt) {
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t info = {};
        if (netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK &&
            info.ip.addr != 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    vTaskDelete(nullptr);
}

void StartClock() {
    setenv("TZ", "YEKT-5", 1);
    tzset();
    if (xTaskCreate(ClockTask, "clock", 4096, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "clock task could not be created");
    }
}

std::string ClockText() {
    time_t now = time(nullptr);
    struct tm local = {};
    localtime_r(&now, &local);
    if (local.tm_year + 1900 < 2024) {
        return "--:--";
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour, local.tm_min);
    return std::string(buf);
}

// ---------------------------------------------------------------- build
void Build() {
    g_ui.font = ThemeFont();

    g_ui.root = Panel(lv_layer_top(), 0, 0, kScreenW, kScreenH, kBg, 0, 0, 0);

    BuildTopBar(g_ui.root);
    BuildPortrait(g_ui.root);
    BuildMedia(g_ui.root);
    BuildWind(g_ui.root);
    BuildWeather(g_ui.root);
    BuildNav(g_ui.root);

    HideWeatherIcon();
    lv_obj_add_flag(g_ui.tens.box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ui.units.box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ui.degree, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_ui.minus, LV_OBJ_FLAG_HIDDEN);

    g_ui.built = true;
    ESP_LOGI(TAG, "dashboard built");
}

// ---------------------------------------------------------------- update
const char* StateText(DeviceState state) {
    switch (state) {
        case kDeviceStateIdle:
            return "\xd0\x93\xd0\xbe\xd1\x82\xd0\xbe\xd0\xb2";  // Готов
        case kDeviceStateConnecting:
            return "\xd0\xa1\xd0\xb2\xd1\x8f\xd0\xb7\xd1\x8c";  // Связь
        case kDeviceStateListening:
            return "\xd0\xa1\xd0\xbb\xd1\x83\xd1\x88\xd0\xb0\xd1\x8e";  // Слушаю
        case kDeviceStateSpeaking:
            return "\xd0\x93\xd0\xbe\xd0\xb2\xd0\xbe\xd1\x80\xd1\x8e";  // Говорю
        case kDeviceStateNotifying:
            return "\xd0\x97\xd0\xb2\xd0\xbe\xd0\xbd\xd0\xbe\xd0\xba";  // Звонок
        case kDeviceStateUpgrading:
            return "\xd0\x9e\xd0\xb1\xd0\xbd\xd0\xbe\xd0\xb2\xd0\xbb\xd0\xb5\xd0\xbd\xd0\xb8\xd0\xb5";
        case kDeviceStateActivating:
            return "\xd0\x9d\xd0\xb0\xd1\x81\xd1\x82\xd1\x80\xd0\xbe\xd0\xb9\xd0\xba\xd0\xb0";
        case kDeviceStateWifiConfiguring:
            return "Wi-Fi";
        case kDeviceStateFatalError:
            return "\xd0\x9e\xd1\x88\xd0\xb8\xd0\xb1\xd0\xba\xd0\xb0";  // Ошибка
        default:
            return "...";
    }
}

void UpdateState() {
    DeviceState state = Application::GetInstance().GetDeviceState();
    if (static_cast<int>(state) == g_ui.last_state) {
        return;
    }
    g_ui.last_state = static_cast<int>(state);

    lv_label_set_text(g_ui.state_text, StateText(state));

    uint32_t color = kCardLine;
    if (state == kDeviceStateListening || state == kDeviceStateConnecting) {
        color = kBlue;
    } else if (state == kDeviceStateSpeaking) {
        color = kWarm;
    } else if (state == kDeviceStateIdle) {
        color = kCyan;
    }
    lv_obj_set_style_border_color(g_ui.frame, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(g_ui.state_text,
                                lv_color_hex(color == kCardLine ? kDim : color), 0);
}

void UpdateSignal() {
    wifi_ap_record_t ap = {};
    int rssi = 0;
    bool linked = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    if (linked) {
        rssi = ap.rssi;
    }
    if (rssi == g_ui.last_rssi) {
        return;
    }
    g_ui.last_rssi = rssi;

    int lit = 0;
    if (linked) {
        lit = rssi > -60 ? 3 : (rssi > -75 ? 2 : 1);
    }
    for (int i = 0; i < 3; ++i) {
        lv_obj_set_style_bg_color(g_ui.wifi[i], lv_color_hex(i < lit ? kCyan : kCardLine), 0);
    }
}

void UpdateVolume() {
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }
    int volume = codec->output_volume();
    if (volume == g_ui.last_volume) {
        return;
    }
    g_ui.last_volume = volume;
    lv_label_set_text(g_ui.volume_text, std::to_string(volume).c_str());
}

void UpdateClock() {
    std::string now = ClockText();
    if (now == g_ui.last_clock) {
        return;
    }
    bool first = g_ui.last_clock == "--:--" && now != "--:--";
    g_ui.last_clock = now;
    lv_label_set_text(g_ui.clock_text, now.c_str());
    if (first) {
        ESP_LOGI(TAG, "clock synced: %s", now.c_str());
    }
}

void UpdateWeather() {
    int temp = 0;
    int code = -1;
    float wind = 0.0f;
    if (!GetWeather(temp, code, wind)) {
        return;
    }
    if (temp != g_ui.last_temp) {
        g_ui.last_temp = temp;
        ApplyTemperature(temp);
        ESP_LOGI(TAG, "weather card: %d C, wmo %d", temp, code);
    }
    if (code != g_ui.last_code) {
        g_ui.last_code = code;
        ApplyWeatherIcon(code);
    }
    int wind10 = static_cast<int>(std::lround(wind * 10.0f));
    if (wind10 != g_ui.last_wind10) {
        g_ui.last_wind10 = wind10;
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", wind);
        lv_label_set_text(g_ui.wind_text, buf);
    }
}

void UpdateMedia() {
    std::string status = MusicTester::GetInstance().Status();
    bool playing = status.rfind("playing radio", 0) == 0;

    std::string station = MusicTester::GetInstance().CurrentStation();
    if (station != g_ui.last_station) {
        g_ui.last_station = station;
        lv_label_set_text(g_ui.station_text, station.empty() ? "-" : station.c_str());
    }

    if (playing != g_ui.last_playing) {
        g_ui.last_playing = playing;
        for (int i = 0; i < 3; ++i) {
            if (playing) {
                Show(g_ui.bar[i]);
            } else {
                lv_obj_add_flag(g_ui.bar[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_height(g_ui.bar[i], 8);
                lv_obj_set_y(g_ui.bar[i], 64);
            }
        }
    }

    if (playing) {
        // the three bars dance so the card shows that the stream is alive
        static const int kHeights[3][3] = {{6, 14, 10}, {12, 6, 16}, {16, 10, 6}};
        int step = (g_ui.tick / 3) % 3;
        for (int i = 0; i < 3; ++i) {
            int h = kHeights[i][step];
            lv_obj_set_height(g_ui.bar[i], h);
            lv_obj_set_y(g_ui.bar[i], 56 + (16 - h));
        }
    }
}

void Tick(lv_timer_t*) {
    DeviceState state = Application::GetInstance().GetDeviceState();

    if (!g_ui.built) {
        // The setup and activation screens belong to the firmware UI: stay out of the way until
        // the device is actually running, otherwise the Wi-Fi QR code would be covered.
        if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring ||
            state == kDeviceStateActivating) {
            return;
        }
        Build();
        return;
    }

    g_ui.tick++;

    if (state == kDeviceStateWifiConfiguring || state == kDeviceStateActivating ||
        state == kDeviceStateUpgrading) {
        lv_obj_add_flag(g_ui.root, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(g_ui.root, LV_OBJ_FLAG_HIDDEN);

    // the dashboard is built before Application installs the assets font, so follow the theme
    // until the full-Cyrillic one is in place
    const lv_font_t* font = ThemeFont();
    if (font != g_ui.font) {
        g_ui.font = font;
        lv_obj_set_style_text_font(g_ui.state_text, font, 0);
        lv_obj_set_style_text_font(g_ui.clock_text, font, 0);
        lv_obj_set_style_text_font(g_ui.volume_text, font, 0);
        lv_obj_set_style_text_font(g_ui.station_text, font, 0);
        lv_obj_set_style_text_font(g_ui.header_text, font, 0);
        lv_obj_set_style_text_font(g_ui.city_text, font, 0);
        lv_obj_set_style_text_font(g_ui.wind_label, font, 0);
        lv_obj_set_style_text_font(g_ui.wind_text, font, 0);
        ESP_LOGI(TAG, "dashboard font refreshed");
    }

    UpdateState();
    UpdateSignal();
    UpdateVolume();
    UpdateClock();
    UpdateWeather();
    UpdateMedia();
}

}  // namespace

void InitializeDashboard() {
    StartClock();
    lv_timer_create(Tick, 250, nullptr);
    ESP_LOGI(TAG, "dashboard timer started");
}

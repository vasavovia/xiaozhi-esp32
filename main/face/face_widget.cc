#include "face_widget.h"

#include <cstdlib>

#include <esp_log.h>
#include <lvgl.h>

#include "application.h"
#include "device_state.h"

#define TAG "Face"

namespace {

constexpr int kWidth = 240;
constexpr int kHeight = 320;

// palette
constexpr uint32_t kBgTop = 0x14121b;
constexpr uint32_t kBgBottom = 0x2a1d25;
constexpr uint32_t kGlow = 0xff8a5c;
constexpr uint32_t kSkin = 0xf7d2b8;
constexpr uint32_t kSkinLight = 0xfce0cb;
constexpr uint32_t kSkinShade = 0xe7b696;
constexpr uint32_t kSkinDeep = 0xd39a79;
constexpr uint32_t kHair = 0x271812;
constexpr uint32_t kHairLight = 0x3d261c;
constexpr uint32_t kEyeWhite = 0xf9f6f3;
constexpr uint32_t kIris = 0x527f6b;
constexpr uint32_t kPupil = 0x120f0e;
constexpr uint32_t kLipUpper = 0xbe5468;
constexpr uint32_t kLipLower = 0xd97a8c;
constexpr uint32_t kBlush = 0xef9583;
constexpr uint32_t kBlouse = 0x4a2c3d;
constexpr uint32_t kBlouseLight = 0x5d3a4d;

struct FaceParts {
    lv_obj_t* root = nullptr;
    lv_obj_t* glow = nullptr;
    lv_obj_t* shoulders = nullptr;
    lv_obj_t* collar = nullptr;
    lv_obj_t* chest = nullptr;
    lv_obj_t* neck = nullptr;
    lv_obj_t* head = nullptr;
    lv_obj_t* hair_top = nullptr;
    lv_obj_t* hair_left = nullptr;
    lv_obj_t* hair_right = nullptr;
    lv_obj_t* fringe = nullptr;
    lv_obj_t* brow_left = nullptr;
    lv_obj_t* brow_right = nullptr;
    lv_obj_t* eye_left = nullptr;
    lv_obj_t* eye_right = nullptr;
    lv_obj_t* iris_left = nullptr;
    lv_obj_t* iris_right = nullptr;
    lv_obj_t* pupil_left = nullptr;
    lv_obj_t* pupil_right = nullptr;
    lv_obj_t* spark_left = nullptr;
    lv_obj_t* spark_right = nullptr;
    lv_obj_t* lid_left = nullptr;
    lv_obj_t* lid_right = nullptr;
    lv_obj_t* nose = nullptr;
    lv_obj_t* chin = nullptr;
    lv_obj_t* blush_left = nullptr;
    lv_obj_t* blush_right = nullptr;
    lv_obj_t* lip_upper = nullptr;
    lv_obj_t* lip_lower = nullptr;
};

FaceParts g_face;
int g_tick = 0;
int g_blink_left = 0;
int g_next_blink = 40;
int g_mouth_phase = 0;

// geometry constants (bust portrait, 240x320)
constexpr int kLidRestY = 94;
constexpr int kLipUpperY = 146;
constexpr int kLipLowerY = 157;

lv_obj_t* NewLayer(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    return obj;
}

void Paint(lv_obj_t* obj, uint32_t color) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

lv_obj_t* Ellipse(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t* obj = NewLayer(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    Paint(obj, color);
    return obj;
}

void BuildFace() {
    lv_obj_t* top = lv_layer_top();
    g_face.root = NewLayer(top);
    lv_obj_set_pos(g_face.root, 0, 0);
    lv_obj_set_size(g_face.root, kWidth, kHeight);
    lv_obj_set_style_bg_color(g_face.root, lv_color_hex(kBgTop), 0);
    lv_obj_set_style_bg_grad_color(g_face.root, lv_color_hex(kBgBottom), 0);
    lv_obj_set_style_bg_grad_dir(g_face.root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(g_face.root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_face.root, 0, 0);

    g_face.glow = Ellipse(g_face.root, 10, 0, 220, 220, kGlow);
    lv_obj_set_style_bg_opa(g_face.glow, 30, 0);

    // shoulders and chest reach the bottom of the panel
    g_face.shoulders = Ellipse(g_face.root, 12, 208, 216, 140, kBlouse);
    g_face.collar = Ellipse(g_face.root, 62, 214, 116, 70, kBlouseLight);
    lv_obj_set_style_bg_opa(g_face.collar, 120, 0);
    g_face.chest = Ellipse(g_face.root, 78, 196, 84, 74, kSkinShade);

    g_face.neck = NewLayer(g_face.root);
    lv_obj_set_pos(g_face.neck, 104, 162);
    lv_obj_set_size(g_face.neck, 32, 52);
    lv_obj_set_style_radius(g_face.neck, 14, 0);
    Paint(g_face.neck, kSkinShade);

    g_face.head = NewLayer(g_face.root);
    lv_obj_set_pos(g_face.head, 62, 30);
    lv_obj_set_size(g_face.head, 116, 150);
    lv_obj_set_style_radius(g_face.head, 58, 0);
    lv_obj_set_style_bg_color(g_face.head, lv_color_hex(kSkinLight), 0);
    lv_obj_set_style_bg_grad_color(g_face.head, lv_color_hex(kSkin), 0);
    lv_obj_set_style_bg_grad_dir(g_face.head, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(g_face.head, LV_OPA_COVER, 0);

    g_face.hair_left = Ellipse(g_face.root, 40, 46, 44, 168, kHair);
    g_face.hair_right = Ellipse(g_face.root, 156, 46, 44, 168, kHair);
    g_face.hair_top = NewLayer(g_face.root);
    lv_obj_set_pos(g_face.hair_top, 52, 18);
    lv_obj_set_size(g_face.hair_top, 136, 80);
    lv_obj_set_style_radius(g_face.hair_top, 66, 0);
    lv_obj_set_style_bg_color(g_face.hair_top, lv_color_hex(kHairLight), 0);
    lv_obj_set_style_bg_grad_color(g_face.hair_top, lv_color_hex(kHair), 0);
    lv_obj_set_style_bg_grad_dir(g_face.hair_top, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(g_face.hair_top, LV_OPA_COVER, 0);

    g_face.fringe = Ellipse(g_face.root, 66, 36, 108, 52, kHair);

    g_face.brow_left = Ellipse(g_face.root, 76, 84, 30, 6, kHairLight);
    g_face.brow_right = Ellipse(g_face.root, 134, 84, 30, 6, kHairLight);

    g_face.eye_left = Ellipse(g_face.root, 78, 96, 30, 21, kEyeWhite);
    g_face.eye_right = Ellipse(g_face.root, 132, 96, 30, 21, kEyeWhite);

    g_face.iris_left = Ellipse(g_face.root, 85, 99, 16, 16, kIris);
    g_face.iris_right = Ellipse(g_face.root, 139, 99, 16, 16, kIris);
    g_face.pupil_left = Ellipse(g_face.root, 90, 104, 7, 7, kPupil);
    g_face.pupil_right = Ellipse(g_face.root, 144, 104, 7, 7, kPupil);
    g_face.spark_left = Ellipse(g_face.root, 87, 101, 4, 4, kEyeWhite);
    g_face.spark_right = Ellipse(g_face.root, 141, 101, 4, 4, kEyeWhite);

    g_face.lid_left = NewLayer(g_face.root);
    lv_obj_set_pos(g_face.lid_left, 76, kLidRestY);
    lv_obj_set_size(g_face.lid_left, 34, 26);
    lv_obj_set_style_radius(g_face.lid_left, 12, 0);
    Paint(g_face.lid_left, kSkin);
    g_face.lid_right = NewLayer(g_face.root);
    lv_obj_set_pos(g_face.lid_right, 130, kLidRestY);
    lv_obj_set_size(g_face.lid_right, 34, 26);
    lv_obj_set_style_radius(g_face.lid_right, 12, 0);
    Paint(g_face.lid_right, kSkin);

    g_face.nose = Ellipse(g_face.root, 116, 118, 9, 20, kSkinShade);
    lv_obj_set_style_bg_opa(g_face.nose, 150, 0);
    g_face.chin = Ellipse(g_face.root, 102, 160, 36, 14, kSkinDeep);
    lv_obj_set_style_bg_opa(g_face.chin, 55, 0);

    g_face.blush_left = Ellipse(g_face.root, 68, 126, 26, 16, kBlush);
    lv_obj_set_style_bg_opa(g_face.blush_left, 55, 0);
    g_face.blush_right = Ellipse(g_face.root, 146, 126, 26, 16, kBlush);
    lv_obj_set_style_bg_opa(g_face.blush_right, 55, 0);

    g_face.lip_upper = Ellipse(g_face.root, 99, kLipUpperY, 42, 11, kLipUpper);
    g_face.lip_lower = Ellipse(g_face.root, 99, kLipLowerY, 42, 15, kLipLower);

    ESP_LOGI(TAG, "bust portrait built");
}

void ShowIdle() {
    lv_obj_set_pos(g_face.lid_left, 76, kLidRestY);
    lv_obj_set_pos(g_face.lid_right, 130, kLidRestY);
    lv_obj_set_pos(g_face.brow_left, 76, 84);
    lv_obj_set_pos(g_face.brow_right, 134, 84);
    lv_obj_set_pos(g_face.lip_upper, 99, kLipUpperY);
    lv_obj_set_pos(g_face.lip_lower, 99, kLipLowerY);
    lv_obj_set_style_bg_color(g_face.glow, lv_color_hex(kGlow), 0);
    lv_obj_set_style_bg_opa(g_face.glow, 30, 0);
}

void ShowListening() {
    lv_obj_set_pos(g_face.lid_left, 76, kLidRestY - 4);
    lv_obj_set_pos(g_face.lid_right, 130, kLidRestY - 4);
    lv_obj_set_pos(g_face.brow_left, 76, 80);
    lv_obj_set_pos(g_face.brow_right, 134, 80);
    lv_obj_set_style_bg_color(g_face.glow, lv_color_hex(0x63b3ff), 0);
    lv_obj_set_style_bg_opa(g_face.glow, 58, 0);
}

void ShowSpeaking() {
    int openness = (g_mouth_phase % 4) * 3;
    lv_obj_set_pos(g_face.lip_upper, 99, kLipUpperY - openness / 2);
    lv_obj_set_pos(g_face.lip_lower, 99, kLipLowerY + openness);
    lv_obj_set_style_bg_color(g_face.glow, lv_color_hex(0xffb066), 0);
    lv_obj_set_style_bg_opa(g_face.glow, 66, 0);
}

void Tick(lv_timer_t*) {
    if (g_face.root == nullptr) {
        BuildFace();
        ShowIdle();
        return;
    }

    g_tick++;
    DeviceState state = Application::GetInstance().GetDeviceState();

    if (g_blink_left > 0) {
        g_blink_left--;
        int offset = g_blink_left > 2 ? 20 : 8;
        lv_obj_set_pos(g_face.lid_left, 76, kLidRestY + offset);
        lv_obj_set_pos(g_face.lid_right, 130, kLidRestY + offset);
    } else if (state != kDeviceStateListening) {
        if (--g_next_blink <= 0) {
            g_blink_left = 5;
            g_next_blink = 40 + (std::rand() % 60);
        }
    }

    if (state == kDeviceStateSpeaking) {
        g_mouth_phase++;
        ShowSpeaking();
    } else if (state == kDeviceStateListening || state == kDeviceStateConnecting) {
        ShowListening();
        lv_obj_set_pos(g_face.lip_upper, 99, kLipUpperY);
        lv_obj_set_pos(g_face.lip_lower, 99, kLipLowerY);
    } else {
        ShowIdle();
    }
}

}  // namespace

void InitializeFace() {
    lv_timer_create(Tick, 50, nullptr);
    ESP_LOGI(TAG, "face timer started");
}

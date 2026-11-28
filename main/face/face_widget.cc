#include "face_widget.h"

#include <cstdlib>

#include <esp_log.h>
#include <lvgl.h>

#include "application.h"
#include "device_state.h"

#define TAG "Face"

namespace {

constexpr int kWidth = 240;
constexpr int kFaceHeight = 240;

// palette
constexpr uint32_t kBgTop = 0x12141c;
constexpr uint32_t kBgBottom = 0x241a26;
constexpr uint32_t kGlow = 0xff8a5c;
constexpr uint32_t kSkin = 0xf6cfb4;
constexpr uint32_t kSkinShade = 0xe6b394;
constexpr uint32_t kSkinDeep = 0xd79a78;
constexpr uint32_t kHair = 0x2a1a14;
constexpr uint32_t kHairLight = 0x3d2a20;
constexpr uint32_t kEyeWhite = 0xf7f4f2;
constexpr uint32_t kIris = 0x4f7f6a;
constexpr uint32_t kIrisInner = 0x2f5a48;
constexpr uint32_t kPupil = 0x14100f;
constexpr uint32_t kLipUpper = 0xc1576c;
constexpr uint32_t kLipLower = 0xd9768a;
constexpr uint32_t kBlush = 0xef9a86;

struct FaceParts {
    lv_obj_t* root = nullptr;
    lv_obj_t* glow = nullptr;
    lv_obj_t* head = nullptr;
    lv_obj_t* hair_top = nullptr;
    lv_obj_t* hair_left = nullptr;
    lv_obj_t* hair_right = nullptr;
    lv_obj_t* neck = nullptr;
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
    lv_obj_t* lip_upper = nullptr;
    lv_obj_t* lip_lower = nullptr;
    lv_obj_t* blush_left = nullptr;
    lv_obj_t* blush_right = nullptr;
    lv_obj_t* chin = nullptr;
};

FaceParts g_face;
int g_tick = 0;
int g_blink_left = 0;
int g_next_blink = 40;  // ticks (50 ms each)
int g_mouth_phase = 0;

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

void PaintSoft(lv_obj_t* obj, uint32_t color, lv_opa_t opa) {
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
}

lv_obj_t* MakeEllipse(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color) {
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
    lv_obj_set_size(g_face.root, kWidth, kFaceHeight);
    lv_obj_set_style_bg_color(g_face.root, lv_color_hex(kBgTop), 0);
    lv_obj_set_style_bg_grad_color(g_face.root, lv_color_hex(kBgBottom), 0);
    lv_obj_set_style_bg_grad_dir(g_face.root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(g_face.root, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_face.root, 0, 0);

    // warm rim light behind the head
    g_face.glow = MakeEllipse(g_face.root, 20, 10, 200, 200, kGlow);
    lv_obj_set_style_bg_opa(g_face.glow, 34, 0);

    // neck first, the head overlaps it
    g_face.neck = NewLayer(g_face.root);
    lv_obj_set_pos(g_face.neck, 102, 168);
    lv_obj_set_size(g_face.neck, 36, 52);
    lv_obj_set_style_radius(g_face.neck, 16, 0);
    Paint(g_face.neck, kSkinShade);

    g_face.head = NewLayer(g_face.root);
    lv_obj_set_pos(g_face.head, 60, 40);
    lv_obj_set_size(g_face.head, 120, 156);
    lv_obj_set_style_radius(g_face.head, 60, 0);
    lv_obj_set_style_bg_color(g_face.head, lv_color_hex(0xfbd9c2), 0);
    lv_obj_set_style_bg_grad_color(g_face.head, lv_color_hex(kSkin), 0);
    lv_obj_set_style_bg_grad_dir(g_face.head, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(g_face.head, LV_OPA_COVER, 0);

    // hair mass behind and above the head
    g_face.hair_left = MakeEllipse(g_face.root, 44, 56, 44, 150, kHair);
    g_face.hair_right = MakeEllipse(g_face.root, 152, 56, 44, 150, kHair);
    g_face.hair_top = NewLayer(g_face.root);
    lv_obj_set_pos(g_face.hair_top, 56, 30);
    lv_obj_set_size(g_face.hair_top, 128, 74);
    lv_obj_set_style_radius(g_face.hair_top, 64, 0);
    lv_obj_set_style_bg_color(g_face.hair_top, lv_color_hex(kHairLight), 0);
    lv_obj_set_style_bg_grad_color(g_face.hair_top, lv_color_hex(kHair), 0);
    lv_obj_set_style_bg_grad_dir(g_face.hair_top, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(g_face.hair_top, LV_OPA_COVER, 0);

    // a fringe crossing the forehead
    lv_obj_t* fringe = MakeEllipse(g_face.root, 66, 46, 108, 46, kHair);
    lv_obj_set_style_bg_opa(fringe, LV_OPA_COVER, 0);

    // brows
    g_face.brow_left = MakeEllipse(g_face.root, 78, 96, 32, 7, kHairLight);
    g_face.brow_right = MakeEllipse(g_face.root, 130, 96, 32, 7, kHairLight);

    // eye sockets
    g_face.eye_left = MakeEllipse(g_face.root, 80, 108, 34, 24, kEyeWhite);
    g_face.eye_right = MakeEllipse(g_face.root, 126, 108, 34, 24, kEyeWhite);

    g_face.iris_left = MakeEllipse(g_face.root, 90, 111, 18, 18, kIris);
    g_face.iris_right = MakeEllipse(g_face.root, 136, 111, 18, 18, kIris);
    g_face.pupil_left = MakeEllipse(g_face.root, 96, 116, 8, 8, kPupil);
    g_face.pupil_right = MakeEllipse(g_face.root, 142, 116, 8, 8, kPupil);
    g_face.spark_left = MakeEllipse(g_face.root, 92, 113, 5, 5, kEyeWhite);
    g_face.spark_right = MakeEllipse(g_face.root, 138, 113, 5, 5, kEyeWhite);

    // eyelids: skin-coloured strips that slide down to blink
    g_face.lid_left = NewLayer(g_face.root);
    lv_obj_set_pos(g_face.lid_left, 78, 106);
    lv_obj_set_size(g_face.lid_left, 38, 30);
    lv_obj_set_style_radius(g_face.lid_left, 12, 0);
    Paint(g_face.lid_left, kSkin);
    g_face.lid_right = NewLayer(g_face.root);
    lv_obj_set_pos(g_face.lid_right, 124, 106);
    lv_obj_set_size(g_face.lid_right, 38, 30);
    lv_obj_set_style_radius(g_face.lid_right, 12, 0);
    Paint(g_face.lid_right, kSkin);

    // nose and chin shading
    g_face.nose = MakeEllipse(g_face.root, 115, 132, 10, 22, kSkinShade);
    lv_obj_set_style_bg_opa(g_face.nose, 150, 0);
    g_face.chin = MakeEllipse(g_face.root, 100, 176, 40, 16, kSkinDeep);
    lv_obj_set_style_bg_opa(g_face.chin, 60, 0);

    // blush
    g_face.blush_left = MakeEllipse(g_face.root, 70, 140, 30, 18, kBlush);
    lv_obj_set_style_bg_opa(g_face.blush_left, 60, 0);
    g_face.blush_right = MakeEllipse(g_face.root, 140, 140, 30, 18, kBlush);
    lv_obj_set_style_bg_opa(g_face.blush_right, 60, 0);

    // lips
    g_face.lip_upper = MakeEllipse(g_face.root, 98, 160, 44, 12, kLipUpper);
    g_face.lip_lower = MakeEllipse(g_face.root, 98, 172, 44, 16, kLipLower);

    ESP_LOGI(TAG, "face built");
}

void ApplyIdle() {
    lv_obj_set_pos(g_face.lid_left, 78, 106);
    lv_obj_set_pos(g_face.lid_right, 124, 106);
    lv_obj_set_pos(g_face.lip_upper, 98, 160);
    lv_obj_set_pos(g_face.lip_lower, 98, 172);
    lv_obj_set_style_bg_color(g_face.glow, lv_color_hex(kGlow), 0);
    lv_obj_set_style_bg_opa(g_face.glow, 34, 0);
}

void ApplyListening() {
    // eyes wide open, brows up, faint blue glow
    lv_obj_set_pos(g_face.lid_left, 78, 100);
    lv_obj_set_pos(g_face.lid_right, 124, 100);
    lv_obj_set_pos(g_face.brow_left, 78, 92);
    lv_obj_set_pos(g_face.brow_right, 130, 92);
    lv_obj_set_style_bg_color(g_face.glow, lv_color_hex(0x63b3ff), 0);
    lv_obj_set_style_bg_opa(g_face.glow, 60, 0);
}

void ApplySpeaking() {
    // mouth opens and closes in a slow pseudo-random rhythm
    int openness = (g_mouth_phase % 4) * 3;
    lv_obj_set_pos(g_face.lip_upper, 98, 158 - openness / 2);
    lv_obj_set_pos(g_face.lip_lower, 98, 170 + openness);
    lv_obj_set_style_bg_color(g_face.glow, lv_color_hex(0xffb066), 0);
    lv_obj_set_style_bg_opa(g_face.glow, 70, 0);
}

void Tick(lv_timer_t*) {
    if (g_face.root == nullptr) {
        BuildFace();
        ApplyIdle();
        return;
    }

    g_tick++;
    DeviceState state = Application::GetInstance().GetDeviceState();

    // blinking happens in every state except while the mouth is busy
    if (g_blink_left > 0) {
        g_blink_left--;
        int offset = g_blink_left > 2 ? 22 : 8;
        lv_obj_set_pos(g_face.lid_left, 78, 106 + offset);
        lv_obj_set_pos(g_face.lid_right, 124, 106 + offset);
    } else if (state != kDeviceStateListening) {
        if (--g_next_blink <= 0) {
            g_blink_left = 5;
            g_next_blink = 40 + (std::rand() % 60);
        }
    }

    if (state == kDeviceStateSpeaking) {
        g_mouth_phase++;
        ApplySpeaking();
    } else if (state == kDeviceStateListening || state == kDeviceStateConnecting) {
        ApplyListening();
        lv_obj_set_pos(g_face.lip_upper, 98, 160);
        lv_obj_set_pos(g_face.lip_lower, 98, 172);
    } else {
        ApplyIdle();
    }

    // gentle breathing: the whole face drifts a couple of pixels
    int bob = (g_tick / 20) % 2;
    lv_obj_set_pos(g_face.head, 60, 40 + bob);
}

}  // namespace

void InitializeFace() {
    lv_timer_create(Tick, 50, nullptr);
    ESP_LOGI(TAG, "face timer started");
}

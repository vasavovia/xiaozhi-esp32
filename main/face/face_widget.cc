#include "face_widget.h"

#include <esp_log.h>
#include <lvgl.h>

#include "application.h"
#include "device_state.h"
#include "face/face_portrait.h"

#define TAG "Face"

namespace {

lv_obj_t* g_frame = nullptr;
bool g_built = false;

void Build() {
    lv_obj_t* top = lv_layer_top();

    lv_obj_t* root = lv_obj_create(top);
    lv_obj_remove_style_all(root);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, 240, 320);

    lv_obj_t* picture = lv_image_create(root);
    lv_image_set_src(picture, &face_portrait);
    lv_obj_set_pos(picture, 0, 0);

    // thin state frame drawn over the portrait: blue while listening, warm while speaking
    g_frame = lv_obj_create(top);
    lv_obj_remove_style_all(g_frame);
    lv_obj_clear_flag(g_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(g_frame, 0, 0);
    lv_obj_set_size(g_frame, 240, 320);
    lv_obj_set_style_bg_opa(g_frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_frame, 3, 0);
    lv_obj_set_style_border_color(g_frame, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_opa(g_frame, LV_OPA_TRANSP, 0);

    g_built = true;
    ESP_LOGI(TAG, "portrait image shown");
}

void Tick(lv_timer_t*) {
    if (!g_built) {
        Build();
        return;
    }
    DeviceState state = Application::GetInstance().GetDeviceState();
    if (state == kDeviceStateListening || state == kDeviceStateConnecting) {
        lv_obj_set_style_border_color(g_frame, lv_color_hex(0x4fa3ff), 0);
        lv_obj_set_style_border_opa(g_frame, LV_OPA_COVER, 0);
    } else if (state == kDeviceStateSpeaking) {
        lv_obj_set_style_border_color(g_frame, lv_color_hex(0xffb066), 0);
        lv_obj_set_style_border_opa(g_frame, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_border_opa(g_frame, LV_OPA_TRANSP, 0);
    }
}

}  // namespace

void InitializeFace() {
    lv_timer_create(Tick, 100, nullptr);
    ESP_LOGI(TAG, "face timer started");
}

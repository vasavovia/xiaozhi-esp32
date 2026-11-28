#include "weather.h"

#include <cmath>
#include <string>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"

#define TAG "Weather"

namespace {

// Yekaterinburg, current temperature and WMO weather code.
constexpr const char* kUrl =
    "https://api.open-meteo.com/v1/forecast?latitude=56.8389&longitude=60.6057"
    "&current=temperature_2m,weather_code&timezone=Asia%2FYekaterinburg";

constexpr TickType_t kHour = pdMS_TO_TICKS(3600 * 1000);
constexpr TickType_t kRetry = pdMS_TO_TICKS(5 * 60 * 1000);

volatile int g_temp = 0;
volatile int g_code = -1;
volatile bool g_ready = false;

bool Fetch() {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (!http) {
        return false;
    }
    http->SetTimeout(15000);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Accept-Encoding", "identity");

    auto opened = http->Open("GET", kUrl);
    if (!opened) {
        ESP_LOGW(TAG, "open failed: %s", opened.error().ToString().c_str());
        http->Close();
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status < 200 || *status >= 300) {
        ESP_LOGW(TAG, "http status %d", status ? *status : -1);
        http->Close();
        return false;
    }

    std::string body;
    char buffer[512];
    while (true) {
        auto read = http->Read(buffer, sizeof(buffer));
        if (!read || *read == 0) {
            break;
        }
        body.append(buffer, *read);
        if (body.size() > 4096) {
            break;
        }
    }
    http->Close();

    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (root == nullptr) {
        ESP_LOGW(TAG, "bad json");
        return false;
    }
    const cJSON* current = cJSON_GetObjectItem(root, "current");
    const cJSON* temp = cJSON_GetObjectItem(current, "temperature_2m");
    const cJSON* code = cJSON_GetObjectItem(current, "weather_code");
    bool ok = cJSON_IsNumber(temp) && cJSON_IsNumber(code);
    if (ok) {
        g_temp = static_cast<int>(std::lround(temp->valuedouble));
        g_code = code->valueint;
        g_ready = true;
        ESP_LOGI(TAG, "Yekaterinburg %d C, wmo %d", g_temp, g_code);
    }
    cJSON_Delete(root);
    return ok;
}

void WeatherTask(void*) {
    for (int attempt = 0; attempt < 90; attempt++) {
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t info = {};
        if (netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK &&
            info.ip.addr != 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    while (true) {
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            ok = Fetch();
            if (!ok) {
                vTaskDelay(pdMS_TO_TICKS(20000));
            }
        }
        vTaskDelay(ok ? kHour : kRetry);
    }
}

}  // namespace

bool GetWeather(int& temp_c, int& wmo_code) {
    if (!g_ready) {
        return false;
    }
    temp_c = g_temp;
    wmo_code = g_code;
    return true;
}

void InitializeWeather() {
    if (xTaskCreate(WeatherTask, "weather", 24576, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "weather task could not be created");
    }
}

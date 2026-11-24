#include "music_tester.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include "esp_audio_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_enc.h"
#include "esp_audio_types.h"
#include "esp_opus_enc.h"

#include "application.h"
#include "audio/audio_service.h"
#include "board.h"
#include "mcp_server.h"
#include "music/yandex_music.h"
#include "protocols/protocol.h"

#define TAG "MusicPlayer"

namespace {

constexpr int kOutputRate = ESP_AUDIO_SAMPLE_RATE_24K;
constexpr int kFrameSamples = kOutputRate * OPUS_FRAME_DURATION_MS / 1000;
constexpr int kHttpTimeoutMs = 20000;

int16_t ToMono(const int16_t* frame, int channels) {
    if (channels <= 1) {
        return frame[0];
    }
    if (channels == 2) {
        int sum = static_cast<int>(frame[0]) + static_cast<int>(frame[1]);
        return static_cast<int16_t>(sum / 2);
    }
    return frame[0];
}

}  // namespace

MusicTester& MusicTester::GetInstance() {
    static MusicTester instance;
    return instance;
}

bool MusicTester::StopRequested() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_requested_;
}

bool MusicTester::StartUrl(const std::string& url, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_handle_ != nullptr) {
        stop_requested_ = true;
        error = "already playing, stop requested";
        return false;
    }
    url_ = url;
    playlist_.clear();
    playlist_mode_ = false;
    stop_requested_ = false;
    if (xTaskCreate(WorkerEntry, "music_player", 10240, this, 3, &task_handle_) != pdPASS) {
        task_handle_ = nullptr;
        error = "task create failed";
        return false;
    }
    return true;
}

bool MusicTester::StartPlaylist(const std::string& playlist, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_handle_ != nullptr) {
        stop_requested_ = true;
        error = "already playing, stop requested";
        return false;
    }
    playlist_ = playlist;
    url_.clear();
    playlist_mode_ = true;
    stop_requested_ = false;
    if (xTaskCreate(WorkerEntry, "music_player", 12288, this, 3, &task_handle_) != pdPASS) {
        task_handle_ = nullptr;
        error = "task create failed";
        return false;
    }
    return true;
}

void MusicTester::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = true;
}

bool MusicTester::IsPlaying() {
    std::lock_guard<std::mutex> lock(mutex_);
    return task_handle_ != nullptr;
}

std::string MusicTester::Status() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_handle_ == nullptr) {
        return "idle";
    }
    if (playlist_mode_) {
        return "playing playlist '" + playlist_ + "'";
    }
    return "playing " + url_;
}

void MusicTester::WorkerEntry(void* arg) {
    static_cast<MusicTester*>(arg)->WorkerTask();
    vTaskDelete(nullptr);
}

void MusicTester::WorkerTask() {
    std::string url;
    std::string playlist;
    bool playlist_mode = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = url_;
        playlist = playlist_;
        playlist_mode = playlist_mode_;
    }

    if (playlist_mode) {
        YandexMusic client;
        std::vector<YmTrack> tracks;
        std::string error;
        if (client.ResolvePlaylist(playlist, tracks, error)) {
            ESP_LOGI(TAG, "Playlist has %u tracks", static_cast<unsigned>(tracks.size()));
            for (const auto& track : tracks) {
                if (StopRequested()) {
                    break;
                }
                std::string stream_url;
                if (!client.ResolveTrackUrl(track.id, stream_url, error)) {
                    ESP_LOGW(TAG, "Skip %s: %s", track.title.c_str(), error.c_str());
                    continue;
                }
                ESP_LOGI(TAG, "Track: %s", track.title.c_str());
                StreamOne(stream_url);
            }
        } else {
            ESP_LOGE(TAG, "Playlist resolve failed: %s", error.c_str());
        }
    } else {
        StreamOne(url);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        task_handle_ = nullptr;
        stop_requested_ = false;
    }
    ESP_LOGI(TAG, "Playback finished");
}

void MusicTester::StreamOne(const std::string& url) {
    ESP_LOGI(TAG, "Open %s", url.c_str());

    auto& audio = Application::GetInstance().GetAudioService();
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (!http) {
        ESP_LOGE(TAG, "No HTTP client");
        return;
    }

    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "audio/mpeg, audio/mp3, application/octet-stream, */*");
    http->SetHeader("Accept-Encoding", "identity");
    http->SetHeader("Icy-MetaData", "0");

    auto opened = http->Open("GET", url);
    if (!opened) {
        ESP_LOGE(TAG, "Open failed: %s", opened.error().ToString().c_str());
        http->Close();
        return;
    }
    auto status = http->GetStatusCode();
    if (!status) {
        ESP_LOGE(TAG, "Status failed: %s", status.error().ToString().c_str());
        http->Close();
        return;
    }
    if (*status < 200 || *status >= 300) {
        ESP_LOGE(TAG, "HTTP status %d", *status);
        http->Close();
        return;
    }
    ESP_LOGI(TAG, "Streaming, status %d", *status);

    esp_audio_dec_cfg_t dec_cfg = {};
    dec_cfg.type = ESP_AUDIO_TYPE_MP3;
    esp_audio_dec_handle_t decoder = nullptr;
    if (esp_audio_dec_open(&dec_cfg, &decoder) != ESP_AUDIO_ERR_OK || decoder == nullptr) {
        ESP_LOGE(TAG, "MP3 decoder open failed");
        http->Close();
        return;
    }

    esp_opus_enc_config_t enc_cfg = AS_OPUS_ENC_CONFIG();
    enc_cfg.sample_rate = kOutputRate;
    enc_cfg.bitrate = ESP_OPUS_BITRATE_AUTO;
    void* encoder = nullptr;
    esp_audio_err_t enc_err = esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &encoder);
    if (enc_err != ESP_AUDIO_ERR_OK || encoder == nullptr) {
        ESP_LOGE(TAG, "Opus encoder open failed: %d", static_cast<int>(enc_err));
        esp_audio_dec_close(decoder);
        http->Close();
        return;
    }

    int enc_frame_size = 0;
    int enc_out_size = 0;
    esp_opus_enc_get_frame_size(encoder, &enc_frame_size, &enc_out_size);
    ESP_LOGI(TAG, "Opus frame %d samples, out buffer %d bytes", enc_frame_size, enc_out_size);

    std::vector<uint8_t> opus_buf(enc_out_size > 0 ? enc_out_size : 1024);
    std::vector<int16_t> mono_frame(kFrameSamples);
    int mono_fill = 0;
    double resample_pos = 0.0;
    int last_src_rate = 0;

    std::vector<uint8_t> input(16384);
    std::vector<uint8_t> pending;
    pending.reserve(32768);
    bool id3_checked = false;
    std::vector<int16_t> pcm(4096 * 4);
    esp_audio_dec_info_t info = {};

    audio.ResetDecoder();

    while (true) {
        if (StopRequested()) {
            ESP_LOGI(TAG, "Stop requested");
            break;
        }

        auto read = http->Read(reinterpret_cast<char*>(input.data()), input.size());
        if (!read) {
            ESP_LOGE(TAG, "Read failed: %s", read.error().ToString().c_str());
            break;
        }
        if (*read == 0) {
            ESP_LOGI(TAG, "Stream finished");
            break;
        }

        size_t offset = 0;
        size_t length = *read;

        if (!id3_checked) {
            if (length >= 10 && std::memcmp(input.data(), "ID3", 3) == 0) {
                size_t tag_size = ((input[6] & 0x7f) << 21) | ((input[7] & 0x7f) << 14) |
                                  ((input[8] & 0x7f) << 7) | (input[9] & 0x7f);
                size_t total = 10 + tag_size;
                ESP_LOGI(TAG, "Skipping ID3v2 tag, %u bytes", static_cast<unsigned>(total));
                offset = std::min(total, length);
            }
            id3_checked = true;
        }

        pending.insert(pending.end(), input.begin() + offset, input.begin() + length);

        size_t consumed_total = 0;
        while (consumed_total < pending.size()) {
            esp_audio_dec_in_raw_t raw = {};
            raw.buffer = pending.data() + consumed_total;
            raw.len = pending.size() - consumed_total;
            raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

            esp_audio_dec_out_frame_t out = {};
            out.buffer = reinterpret_cast<uint8_t*>(pcm.data());
            out.len = pcm.size() * sizeof(int16_t);

            esp_audio_err_t err = esp_audio_dec_process(decoder, &raw, &out);
            if (raw.consumed > 0) {
                consumed_total += raw.consumed;
            }
            if (err != ESP_AUDIO_ERR_OK) {
                if (out.needed_size > out.len) {
                    pcm.resize((out.needed_size + sizeof(int16_t) - 1) / sizeof(int16_t));
                    continue;
                }
                break;
            }
            if (out.decoded_size == 0) {
                if (raw.consumed == 0) {
                    break;
                }
                continue;
            }

            esp_audio_dec_get_info(decoder, &info);
            int channels = info.channel > 0 ? info.channel : 1;
            int rate = info.sample_rate > 0 ? info.sample_rate : 44100;
            if (rate != last_src_rate) {
                ESP_LOGI(TAG, "Decoded stream: %d Hz, %d ch", rate, channels);
                last_src_rate = rate;
                resample_pos = 0.0;
            }

            int frames = out.decoded_size / (sizeof(int16_t) * channels);
            if (frames < 2) {
                continue;
            }
            const int16_t* src = pcm.data();
            double step = static_cast<double>(rate) / kOutputRate;

            while (resample_pos < frames - 1) {
                int idx = static_cast<int>(resample_pos);
                double frac = resample_pos - idx;
                int16_t s0 = ToMono(src + idx * channels, channels);
                int16_t s1 = ToMono(src + (idx + 1) * channels, channels);
                int16_t value = static_cast<int16_t>(s0 + (s1 - s0) * frac);

                mono_frame[mono_fill++] = value;
                if (mono_fill == kFrameSamples) {
                    esp_audio_enc_in_frame_t in = {};
                    in.buffer = reinterpret_cast<uint8_t*>(mono_frame.data());
                    in.len = kFrameSamples * sizeof(int16_t);
                    esp_audio_enc_out_frame_t enc_out = {};
                    enc_out.buffer = opus_buf.data();
                    enc_out.len = opus_buf.size();
                    enc_out.encoded_bytes = 0;

                    if (esp_opus_enc_process(encoder, &in, &enc_out) == ESP_AUDIO_ERR_OK &&
                        enc_out.encoded_bytes > 0) {
                        auto packet = std::make_unique<AudioStreamPacket>();
                        packet->sample_rate = kOutputRate;
                        packet->frame_duration = OPUS_FRAME_DURATION_MS;
                        packet->payload.assign(opus_buf.data(),
                                               opus_buf.data() + enc_out.encoded_bytes);
                        if (!audio.PushPacketToDecodeQueue(std::move(packet), true)) {
                            ESP_LOGW(TAG, "Decode queue full, dropping frame");
                        }
                    }
                    mono_fill = 0;
                }
                resample_pos += step;
            }
            resample_pos -= frames;
        }

        if (consumed_total > 0) {
            pending.erase(pending.begin(), pending.begin() + consumed_total);
        }
        if (pending.size() > 65536) {
            ESP_LOGW(TAG, "Decoder stalled, dropping %u bytes",
                     static_cast<unsigned>(pending.size()));
            pending.clear();
        }
    }

    esp_opus_enc_close(encoder);
    esp_audio_dec_close(decoder);
    http->Close();
}

namespace {

httpd_handle_t g_token_server = nullptr;

esp_err_t TokenHandler(httpd_req_t* req) {
    char query[512] = {};
    char value[384] = {};
    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (!YandexMusic::GetToken().empty()) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "token already stored");
        return ESP_OK;
    }
    if (query_len > 1 && query_len <= sizeof(query) &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "token", value, sizeof(value)) == ESP_OK && value[0] != 0) {
        YandexMusic::SetToken(value);
        ESP_LOGI(TAG, "Yandex token provisioned over LAN (%u chars)",
                 static_cast<unsigned>(std::strlen(value)));
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "ok: token stored");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "usage: /ym?token=YOUR_YANDEX_MUSIC_TOKEN");
    return ESP_OK;
}

esp_err_t PlayHandler(httpd_req_t* req) {
    char query[768] = {};
    char value[512] = {};
    std::string error;
    bool started = false;

    if (httpd_req_get_url_query_len(req) + 1 <= sizeof(query) &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "likes", value, sizeof(value)) == ESP_OK) {
            started = MusicTester::GetInstance().StartPlaylist("", error);
        } else if (httpd_query_key_value(query, "list", value, sizeof(value)) == ESP_OK) {
            started = MusicTester::GetInstance().StartPlaylist(value, error);
        } else if (httpd_query_key_value(query, "url", value, sizeof(value)) == ESP_OK) {
            started = MusicTester::GetInstance().StartUrl(value, error);
        }
    }

    httpd_resp_set_type(req, "text/plain");
    if (started) {
        httpd_resp_sendstr(req, "ok: playing");
    } else if (!error.empty()) {
        httpd_resp_sendstr(req, ("error: " + error).c_str());
    } else {
        httpd_resp_sendstr(req, "usage: /play?list=NAME | /play?likes=1 | /play?url=URL");
    }
    return ESP_OK;
}

esp_err_t StatusHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, MusicTester::GetInstance().Status().c_str());
    return ESP_OK;
}

}  // namespace

namespace {

void StartTokenHttpd() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.ctrl_port = 32769;
    config.lru_purge_enable = true;
    if (httpd_start(&g_token_server, &config) != ESP_OK) {
        ESP_LOGW(TAG, "Token provisioning server failed to start");
        g_token_server = nullptr;
        return;
    }
    httpd_uri_t uri = {};
    uri.uri = "/ym";
    uri.method = HTTP_GET;
    uri.handler = TokenHandler;
    httpd_register_uri_handler(g_token_server, &uri);

    httpd_uri_t play = {};
    play.uri = "/play";
    play.method = HTTP_GET;
    play.handler = PlayHandler;
    httpd_register_uri_handler(g_token_server, &play);

    httpd_uri_t status = {};
    status.uri = "/status";
    status.method = HTTP_GET;
    status.handler = StatusHandler;
    httpd_register_uri_handler(g_token_server, &status);

    ESP_LOGI(TAG, "Music HTTP control on port 8080: /play?list=NAME, /play?likes=1, /play?url=URL, /status");
}

// lwIP must be up before httpd_start(), so wait for the station to get an address.
void TokenServerTask(void*) {
    for (int attempt = 0; attempt < 90; attempt++) {
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t info = {};
        if (netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK &&
            info.ip.addr != 0) {
            StartTokenHttpd();
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGW(TAG, "No station IP within 180 s, token provisioning server not started");
    vTaskDelete(nullptr);
}

}  // namespace

void StartTokenProvisioning() {
    if (YandexMusic::GetToken().empty()) {
        ESP_LOGI(TAG, "No Yandex token yet, /ym will accept one");
    }
    if (xTaskCreate(TokenServerTask, "ym_token", 4096, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "Music HTTP task could not be created");
    }
}

void InitializeMusicTester() {
    esp_audio_dec_register_default();

    StartTokenProvisioning();

    auto& server = McpServer::GetInstance();

    server.AddTool(
        "self.music.play_playlist",
        "Play a Yandex Music playlist on the device speaker. "
        "Pass the playlist name the user asked for, or an empty string for liked songs. "
        "Stops any previous playback.",
        PropertyList({Property("playlist", kPropertyTypeString, std::string(""))}),
        [](const PropertyList& properties) -> ToolResult {
            std::string playlist = properties["playlist"].value<std::string>();
            std::string error;
            if (!MusicTester::GetInstance().StartPlaylist(playlist, error)) {
                return std::unexpected(error);
            }
            return std::string("playing");
        });

    server.AddTool(
        "self.music.play_url",
        "Play a direct audio URL (MP3 over HTTP/HTTPS) on the device speaker.",
        PropertyList({Property("url", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ToolResult {
            std::string url = properties["url"].value<std::string>();
            std::string error;
            if (!MusicTester::GetInstance().StartUrl(url, error)) {
                return std::unexpected(error);
            }
            return std::string("playing");
        });

    server.AddTool("self.music.stop", "Stop music playback on the device.",
                   PropertyList(), [](const PropertyList&) -> ToolResult {
                       MusicTester::GetInstance().Stop();
                       return std::string("stopped");
                   });

    server.AddTool("self.music.status", "Report whether the device is playing music.",
                   PropertyList(), [](const PropertyList&) -> ToolResult {
                       return MusicTester::GetInstance().Status();
                   });

    server.AddUserOnlyTool(
        "self.music.set_token",
        "Store the Yandex Music OAuth token on the device (user only, never call from the model).",
        PropertyList({Property("token", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ToolResult {
            std::string token = properties["token"].value<std::string>();
            if (token.empty()) {
                return std::unexpected("empty token");
            }
            YandexMusic::SetToken(token);
            return std::string("token stored");
        });

    ESP_LOGI(TAG, "Music tools registered");
}

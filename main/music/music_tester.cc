#include "music_tester.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include <esp_log.h>
#include "esp_audio_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_enc.h"
#include "esp_audio_types.h"
#include "esp_opus_enc.h"

#include "application.h"
#include "audio/audio_service.h"
#include "board.h"
#include "mcp_server.h"
#include "protocols/protocol.h"

#define TAG "MusicTester"

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

bool MusicTester::Start(const std::string& url, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_handle_ != nullptr) {
        error = "already playing";
        return false;
    }
    url_ = url;
    stop_requested_ = false;
    if (xTaskCreate(WorkerEntry, "music_test", 8192, this, 3, &task_handle_) != pdPASS) {
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
    return "playing " + url_;
}

void MusicTester::WorkerEntry(void* arg) {
    static_cast<MusicTester*>(arg)->WorkerTask();
    vTaskDelete(nullptr);
}

void MusicTester::WorkerTask() {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = url_;
    }
    ESP_LOGI(TAG, "Open %s", url.c_str());

    auto& audio = Application::GetInstance().GetAudioService();
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (!http) {
        ESP_LOGE(TAG, "No HTTP client");
        goto cleanup;
    }

    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "audio/mpeg, audio/mp3, application/octet-stream, */*");
    http->SetHeader("Accept-Encoding", "identity");
    http->SetHeader("Icy-MetaData", "0");

    {
        auto opened = http->Open("GET", url);
        if (!opened) {
            ESP_LOGE(TAG, "Open failed: %s", opened.error().ToString().c_str());
            goto cleanup;
        }
        auto status = http->GetStatusCode();
        if (!status) {
            ESP_LOGE(TAG, "Status failed: %s", status.error().ToString().c_str());
            goto cleanup;
        }
        if (*status < 200 || *status >= 300) {
            ESP_LOGE(TAG, "HTTP status %d", *status);
            goto cleanup;
        }
        ESP_LOGI(TAG, "Streaming, status %d", *status);
    }

    {
        esp_audio_dec_cfg_t dec_cfg = {};
        dec_cfg.type = ESP_AUDIO_TYPE_MP3;
        esp_audio_dec_handle_t decoder = nullptr;
        if (esp_audio_dec_open(&dec_cfg, &decoder) != ESP_AUDIO_ERR_OK || decoder == nullptr) {
            ESP_LOGE(TAG, "MP3 decoder open failed");
            http->Close();
            goto cleanup;
        }

        esp_opus_enc_config_t enc_cfg = AS_OPUS_ENC_CONFIG();
        enc_cfg.sample_rate = kOutputRate;
        enc_cfg.bitrate = ESP_OPUS_BITRATE_AUTO;
        void* encoder = nullptr;
        esp_audio_err_t enc_err =
            esp_opus_enc_open(&enc_cfg, sizeof(enc_cfg), &encoder);
        if (enc_err != ESP_AUDIO_ERR_OK || encoder == nullptr) {
            ESP_LOGE(TAG, "Opus encoder open failed: %d", static_cast<int>(enc_err));
            esp_audio_dec_close(decoder);
            http->Close();
            goto cleanup;
        }

        int enc_frame_size = 0;
        int enc_out_size = 0;
        esp_opus_enc_get_frame_size(encoder, &enc_frame_size, &enc_out_size);
        ESP_LOGI(TAG, "Opus frame %d samples, out buffer %d bytes", enc_frame_size, enc_out_size);
        std::vector<uint8_t> opus_buf(enc_out_size > 0 ? enc_out_size : 1024);
        int16_t mono_frame[kFrameSamples];
        int mono_fill = 0;
        double resample_pos = 0.0;
        int last_src_rate = 0;

        std::vector<uint8_t> input(16384);
        std::vector<uint8_t> pending;
        pending.reserve(32768);
        bool id3_skipped = false;
        std::vector<int16_t> pcm(2048 * 4);
        esp_audio_dec_info_t info = {};

        audio.ResetDecoder();

        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_requested_) {
                    ESP_LOGI(TAG, "Stop requested");
                    break;
                }
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

            if (!id3_skipped && pending.empty()) {
                if (length >= 10 && std::memcmp(input.data(), "ID3", 3) == 0) {
                    size_t tag_size = ((input[6] & 0x7f) << 21) | ((input[7] & 0x7f) << 14) |
                                      ((input[8] & 0x7f) << 7) | (input[9] & 0x7f);
                    size_t total = 10 + tag_size;
                    ESP_LOGI(TAG, "Skipping ID3v2 tag, %u bytes", static_cast<unsigned>(total));
                    offset = std::min(total, length);
                }
                id3_skipped = true;
            }

            pending.insert(pending.end(), input.begin() + offset, input.begin() + length);

            size_t consumed_total = 0;
            while (pending.size() - consumed_total > 0) {
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
                    if (out.needed_size > 0 && out.needed_size > out.len) {
                        pcm.resize((out.needed_size + sizeof(int16_t) - 1) / sizeof(int16_t));
                        continue;
                    }
                    if (raw.consumed == 0) {
                        break;
                    }
                    continue;
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
                        in.buffer = reinterpret_cast<uint8_t*>(mono_frame);
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
                ESP_LOGW(TAG, "Decoder made no progress, dropping %u bytes",
                         static_cast<unsigned>(pending.size()));
                pending.clear();
            }
        }

        esp_opus_enc_close(encoder);
        esp_audio_dec_close(decoder);
        http->Close();
    }

cleanup:
    {
        std::lock_guard<std::mutex> lock(mutex_);
        task_handle_ = nullptr;
        stop_requested_ = false;
    }
    ESP_LOGI(TAG, "Playback finished");
}

void InitializeMusicTester() {
    esp_audio_dec_register_default();

    auto& server = McpServer::GetInstance();
    server.AddTool(
        "self.music.play_url",
        "Play an internet audio stream (MP3 over HTTP/HTTPS) on the device speaker. "
        "Stops any previous playback. Use a direct audio URL.",
        PropertyList({Property("url", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ToolResult {
            std::string url = properties["url"].value<std::string>();
            std::string error;
            if (!MusicTester::GetInstance().Start(url, error)) {
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

    ESP_LOGI(TAG, "Music tools registered");
}

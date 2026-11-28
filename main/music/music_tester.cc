#include "music_tester.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
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
#include "device_state.h"
#include "mcp_server.h"
#include "music/radio_stations.h"
#include "music/yandex_music.h"
#include "protocols/protocol.h"

#define TAG "MusicPlayer"

namespace {

constexpr int kOutputRate = ESP_AUDIO_SAMPLE_RATE_24K;
constexpr int kFrameSamples = kOutputRate * OPUS_FRAME_DURATION_MS / 1000;
constexpr int kHttpTimeoutMs = 20000;

int16_t ToMono(const int16_t* frame, int channels) {    if (channels <= 1) {
        return frame[0];
    }
    if (channels == 2) {
        int sum = static_cast<int>(frame[0]) + static_cast<int>(frame[1]);
        return static_cast<int16_t>(sum / 2);
    }
    return frame[0];
}

// Music plays only while the assistant is not talking or listening: those states share the
// decode queue with TTS. Booting states (starting, activating, wifi configuring) are fine.
bool AssistantBusy() {
    switch (Application::GetInstance().GetDeviceState()) {
        case kDeviceStateConnecting:
        case kDeviceStateListening:
        case kDeviceStateSpeaking:
        case kDeviceStateNotifying:
        case kDeviceStateUpgrading:
        case kDeviceStateAudioTesting:
            return true;
        default:
            return false;
    }
}

// Lower-cases ASCII and the Russian Cyrillic block, so «Европа» and «европа» compare equal.
std::string LowerUtf8(const std::string& input) {
    std::string out = input;
    for (size_t i = 0; i + 1 < out.size(); ++i) {
        unsigned char lead = static_cast<unsigned char>(out[i]);
        unsigned char tail = static_cast<unsigned char>(out[i + 1]);
        if (lead == 0xD0) {
            if (tail >= 0x90 && tail <= 0x9F) {  // А..П -> а..п
                out[i + 1] = static_cast<char>(tail + 0x20);
            } else if (tail >= 0xA0 && tail <= 0xAF) {  // Р..Я -> р..я
                out[i] = static_cast<char>(0xD1);
                out[i + 1] = static_cast<char>(tail - 0x20);
            } else if (tail == 0x81) {  // Ё -> ё
                out[i] = static_cast<char>(0xD1);
                out[i + 1] = static_cast<char>(0x91);
            }
        }
    }
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

int FindStation(const std::string& name) {
    if (name.empty()) {
        return -1;
    }
    std::string wanted = LowerUtf8(name);
    for (int i = 0; i < kRadioStationCount; ++i) {
        std::string candidate = LowerUtf8(kRadioStations[i].name);
        if (candidate.find(wanted) != std::string::npos ||
            wanted.find(candidate) != std::string::npos) {
            return i;
        }
    }
    return -1;
}

// httpd_query_key_value returns the value still percent-encoded, so «Ретро» arrives as
// %D0%A0%D0%B5... and never matches the station table without this.
std::string UrlDecode(const char* value) {
    std::string out;
    for (size_t i = 0; value[i] != '\0'; ++i) {
        char ch = value[i];
        if (ch == '%' && std::isxdigit(static_cast<unsigned char>(value[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(value[i + 2]))) {
            int byte = 0;
            for (int k = 1; k <= 2; ++k) {
                char digit = value[i + k];
                int nibble = (digit >= '0' && digit <= '9') ? digit - '0'
                             : (digit >= 'a' && digit <= 'f') ? digit - 'a' + 10
                                                             : digit - 'A' + 10;
                byte = byte * 16 + nibble;
            }
            out.push_back(static_cast<char>(byte));
            i += 2;
        } else if (ch == '+') {
            out.push_back(' ');
        } else {
            out.push_back(ch);
        }
    }
    return out;
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
    wave_mode_ = false;
    radio_mode_ = false;
    stop_requested_ = false;
    if (xTaskCreate(WorkerEntry, "music_player", 24576, this, 3, &task_handle_) != pdPASS) {
        task_handle_ = nullptr;
        error = "task create failed";
        return false;
    }
    return true;
}

bool MusicTester::StartRadio(const std::string& name, std::string& error) {
    int index = FindStation(name);
    // An unrecognised name must not silently start whatever station happens to be current:
    // the caller (voice tool or /play?radio=) needs to hear that the station is unknown.
    if (index < 0 && !name.empty()) {
        error = "unknown station: " + name;
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_handle_ != nullptr) {
        if (radio_mode_) {
            // «включи Ретро» while the radio is already on is a switch, not an error: the worker
            // drops the current stream and re-reads the index, so the new station starts at once.
            if (index >= 0) {
                radio_index_ = index;
            }
            skip_requested_ = true;
            switch_requested_ = true;
            ESP_LOGI(TAG, "Radio switch: %s", kRadioStations[radio_index_].name);
            return true;
        }
        stop_requested_ = true;
        error = "already playing, stop requested";
        return false;
    }
    if (index >= 0) {
        radio_index_ = index;
    }
    radio_mode_ = true;
    playlist_mode_ = false;
    wave_mode_ = false;
    url_.clear();
    playlist_.clear();
    stop_requested_ = false;
    skip_requested_ = false;
    switch_requested_ = false;
    if (xTaskCreate(WorkerEntry, "music_player", 24576, this, 3, &task_handle_) != pdPASS) {
        task_handle_ = nullptr;
        error = "task create failed";
        return false;
    }
    ESP_LOGI(TAG, "Radio start: %s", kRadioStations[radio_index_].name);
    return true;
}

void MusicTester::NextStation() {
    std::lock_guard<std::mutex> lock(mutex_);
    skip_requested_ = true;
}

std::string MusicTester::CurrentStation() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (radio_index_ < 0 || radio_index_ >= kRadioStationCount) {
        return std::string();
    }
    return std::string(kRadioStations[radio_index_].name);
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
    wave_mode_ = false;
    radio_mode_ = false;
    stop_requested_ = false;
    if (xTaskCreate(WorkerEntry, "music_player", 24576, this, 3, &task_handle_) != pdPASS) {
        task_handle_ = nullptr;
        error = "task create failed";
        return false;
    }
    return true;
}

bool MusicTester::StartWave(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_handle_ != nullptr) {
        stop_requested_ = true;
        error = "already playing, stop requested";
        return false;
    }
    playlist_.clear();
    url_.clear();
    playlist_mode_ = false;
    wave_mode_ = true;
    radio_mode_ = false;
    stop_requested_ = false;
    if (xTaskCreate(WorkerEntry, "music_player", 24576, this, 3, &task_handle_) != pdPASS) {
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
    if (radio_mode_) {
        if (radio_index_ >= 0 && radio_index_ < kRadioStationCount) {
            return "playing radio: " + std::string(kRadioStations[radio_index_].name);
        }
        return "playing radio";
    }
    if (wave_mode_) {
        return "playing my wave";
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
    bool wave_mode = false;
    bool radio_mode = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = url_;
        playlist = playlist_;
        playlist_mode = playlist_mode_;
        wave_mode = wave_mode_;
        radio_mode = radio_mode_;
    }

    // A voice request arrives while the assistant is still saying "playing": wait for it to
    // fall silent instead of aborting the request.
    for (int attempt = 0; attempt < 150 && !StopRequested() && AssistantBusy(); attempt++) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (StopRequested()) {
        std::lock_guard<std::mutex> lock(mutex_);
        task_handle_ = nullptr;
        stop_requested_ = false;
        ESP_LOGI(TAG, "Playback cancelled before start");
        return;
    }

    if (radio_mode) {
        PlayRadio();
    } else if (wave_mode) {
        YandexMusic client;
        PlayWave(client);
    } else if (playlist_mode) {
        YandexMusic client;
        std::vector<YmTrack> tracks;
        std::string error;
        if (client.ResolvePlaylist(playlist, tracks, error)) {
            ESP_LOGI(TAG, "Playlist has %u tracks", static_cast<unsigned>(tracks.size()));
            for (const auto& track : tracks) {
                if (StopRequested() || AssistantBusy()) {
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

// Internet radio: each station plays until it fails or the listener asks for the next one,
// then the player moves down the list and wraps around — the stream never ends by itself.
void MusicTester::PlayRadio() {
    while (!StopRequested() && !AssistantBusy()) {
        std::string station_name;
        std::string station_url;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (radio_index_ < 0 || radio_index_ >= kRadioStationCount) {
                radio_index_ = 0;
            }
            station_name = kRadioStations[radio_index_].name;
            station_url = kRadioStations[radio_index_].url;
            skip_requested_ = false;
        }
        ESP_LOGI(TAG, "Radio: %s", station_name.c_str());
        StreamOne(station_url);
        if (StopRequested() || AssistantBusy()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // A named request already moved radio_index_ to the wanted station, so the loop must
            // not step past it; only «next» and a dead stream advance the list.
            if (switch_requested_) {
                switch_requested_ = false;
            } else {
                radio_index_ = (radio_index_ + 1) % kRadioStationCount;
            }
        }
    }
}

// «Моя волна»: keeps asking the rotor for the next batch (passing the previous batchId so the
// tracks do not repeat) and reports trackStarted/trackFinished so the station adapts.
void MusicTester::PlayWave(YandexMusic& client) {
    std::string error;
    while (!StopRequested() && !AssistantBusy()) {
        std::vector<YmTrack> batch;
        if (!client.FetchWaveBatch(batch, error)) {
            ESP_LOGE(TAG, "Wave batch failed: %s", error.c_str());
            return;
        }

        bool interrupted = false;
        for (const auto& track : batch) {
            if (StopRequested() || AssistantBusy()) {
                interrupted = true;
                break;
            }
            std::string stream_url;
            if (!client.ResolveTrackUrl(track.id, stream_url, error)) {
                ESP_LOGW(TAG, "Skip %s: %s", track.title.c_str(), error.c_str());
                continue;
            }
            ESP_LOGI(TAG, "Wave track: %s", track.title.c_str());
            client.SendWaveFeedback("trackStarted", track.id);
            StreamOne(stream_url);
            client.SendWaveFeedback("trackFinished", track.id);
        }
        if (interrupted) {
            return;
        }
    }
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
    ESP_LOGI(TAG, "Opus frame %d bytes (%d samples), out buffer %d bytes", enc_frame_size,
             enc_frame_size / static_cast<int>(sizeof(int16_t)), enc_out_size);

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
        if (AssistantBusy()) {
            ESP_LOGI(TAG, "Assistant is active, stopping music");
            break;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (skip_requested_) {
                ESP_LOGI(TAG, "Next station requested");
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

esp_err_t PlayHandler(httpd_req_t* req) {
    char query[768] = {};
    char value[512] = {};
    std::string error;
    bool started = false;
    bool radio_started = false;

    if (httpd_req_get_url_query_len(req) + 1 <= sizeof(query) &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "radio", value, sizeof(value)) == ESP_OK) {
            std::string station = UrlDecode(value);
            ESP_LOGI(TAG, "Play request for station \"%s\"", station.c_str());
            started = MusicTester::GetInstance().StartRadio(station, error);
            radio_started = started;
        } else if (httpd_query_key_value(query, "url", value, sizeof(value)) == ESP_OK) {
            started = MusicTester::GetInstance().StartUrl(value, error);
        }
    } else {
        started = MusicTester::GetInstance().StartRadio("", error);
        radio_started = started;
    }

    httpd_resp_set_type(req, "text/plain");
    if (started) {
        if (radio_started) {
            std::string name = MusicTester::GetInstance().CurrentStation();
            httpd_resp_sendstr(req, ("ok: playing " + name).c_str());
        } else {
            httpd_resp_sendstr(req, "ok: playing");
        }
    } else if (!error.empty()) {
        httpd_resp_sendstr(req, ("error: " + error).c_str());
    } else {
        httpd_resp_sendstr(req, "usage: /play?radio=NAME | /play?url=URL | /radio?next=1 | /status | /vol?value=NN");
    }
    return ESP_OK;
}

esp_err_t RadioHandler(httpd_req_t* req) {
    MusicTester::GetInstance().NextStation();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok: next station");
    return ESP_OK;
}

esp_err_t StatusHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, MusicTester::GetInstance().Status().c_str());
    return ESP_OK;
}

esp_err_t VolumeHandler(httpd_req_t* req) {
    char query[128] = {};
    char value[16] = {};
    int level = -1;
    if (httpd_req_get_url_query_len(req) + 1 <= sizeof(query) &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "value", value, sizeof(value)) == ESP_OK) {
        level = std::atoi(value);
    }
    if (level < 0) {
        level = 0;
    }
    if (level > 100) {
        level = 100;
    }
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        codec->SetOutputVolume(level);
        ESP_LOGI(TAG, "Volume set to %d over LAN", level);
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, ("ok: volume " + std::to_string(level)).c_str());
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
    httpd_uri_t play = {};
    play.uri = "/play";
    play.method = HTTP_GET;
    play.handler = PlayHandler;
    httpd_register_uri_handler(g_token_server, &play);

    httpd_uri_t radio = {};
    radio.uri = "/radio";
    radio.method = HTTP_GET;
    radio.handler = RadioHandler;
    httpd_register_uri_handler(g_token_server, &radio);

    httpd_uri_t status = {};
    status.uri = "/status";
    status.method = HTTP_GET;
    status.handler = StatusHandler;
    httpd_register_uri_handler(g_token_server, &status);

    httpd_uri_t volume = {};
    volume.uri = "/vol";
    volume.method = HTTP_GET;
    volume.handler = VolumeHandler;
    httpd_register_uri_handler(g_token_server, &volume);

    ESP_LOGI(TAG, "Music HTTP control on port 8080: /play?radio=NAME, /radio?next=1, /play?url=URL, /status, /vol?value=NN");
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
    if (xTaskCreate(TokenServerTask, "ym_token", 4096, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGW(TAG, "Music HTTP task could not be created");
    }
}

void InitializeMusicTester() {
    esp_audio_dec_register_default();

    StartTokenProvisioning();

    auto& server = McpServer::GetInstance();

    server.AddTool(
        "self.music.play_radio",
        "Play internet radio on the device speaker. Pass the station name the user asked for "
        "(«Европа Плюс», «Ретро FM», «Дорожное», «Наше радио», «SomaFM Groove Salad», "
        "«SomaFM Deep Space One»), or an empty string to start with the first one. "
        "Stops any previous playback. An unknown name is rejected with an error.",
        PropertyList({Property("station", kPropertyTypeString, std::string(""))}),
        [](const PropertyList& properties) -> ToolResult {
            std::string station = properties["station"].value<std::string>();
            std::string error;
            if (!MusicTester::GetInstance().StartRadio(station, error)) {
                return std::unexpected(error);
            }
            return "playing " + MusicTester::GetInstance().CurrentStation();
        });

    server.AddTool("self.music.next_station", "Switch internet radio to the next station.",
                   PropertyList(), [](const PropertyList&) -> ToolResult {
                       MusicTester::GetInstance().NextStation();
                       return std::string("switching");
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

    ESP_LOGI(TAG, "Music tools registered");
}

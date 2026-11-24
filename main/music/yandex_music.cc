#include "yandex_music.h"

#include <cstdio>
#include <cstring>
#include <strings.h>
#include <vector>

#include <cJSON.h>
#include <esp_log.h>

#include "board.h"
#include "settings.h"

#define TAG "YandexMusic"

namespace {

constexpr const char* kApiBase = "https://api.music.yandex.net";
constexpr const char* kSignSalt = "XGRlBW9FXlekgbPrRHuSiA";
constexpr int kLikesKind = 3;

// --- MD5 (RFC 1321), self-contained: IDF 6 may not expose the legacy mbedtls MD5 API ---

const uint32_t kMd5K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
};

const uint32_t kMd5Shift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

inline uint32_t Rotl(uint32_t value, uint32_t bits) {
    return (value << bits) | (value >> (32 - bits));
}

void Md5Block(uint32_t state[4], const uint8_t block[64]) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++) {
        m[i] = static_cast<uint32_t>(block[i * 4]) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + Rotl(a + f + kMd5K[i] + m[g], kMd5Shift[i]);
        a = tmp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

std::string Md5Hex(const std::string& input) {
    uint32_t state[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    const uint8_t* data = reinterpret_cast<const uint8_t*>(input.data());
    size_t length = input.size();

    size_t full_blocks = length / 64;
    for (size_t i = 0; i < full_blocks; i++) {
        Md5Block(state, data + i * 64);
    }

    uint8_t tail[128] = {};
    size_t rest = length - full_blocks * 64;
    std::memcpy(tail, data + full_blocks * 64, rest);
    tail[rest] = 0x80;
    size_t tail_len = rest < 56 ? 64 : 128;
    uint64_t bits = static_cast<uint64_t>(length) * 8;
    for (int i = 0; i < 8; i++) {
        tail[tail_len - 8 + i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xff);
    }
    Md5Block(state, tail);
    if (tail_len == 128) {
        Md5Block(state, tail + 64);
    }

    char hex[33];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            std::snprintf(hex + (i * 8) + (j * 2), 3, "%02x", (state[i] >> (8 * j)) & 0xff);
        }
    }
    return std::string(hex, 32);
}

// --- helpers ---

std::string Lower(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    return haystack.find(needle) != std::string::npos;
}

std::string GetStringField(const cJSON* object, const char* name) {
    const cJSON* item = cJSON_GetObjectItem(object, name);
    if (cJSON_IsString(item) && item->valuestring != nullptr) {
        return std::string(item->valuestring);
    }
    return std::string();
}

std::string ExtractTag(const std::string& xml, const char* tag) {
    std::string open = std::string("<") + tag + ">";
    std::string close = std::string("</") + tag + ">";
    size_t start = xml.find(open);
    if (start == std::string::npos) {
        return std::string();
    }
    start += open.size();
    size_t end = xml.find(close, start);
    if (end == std::string::npos) {
        return std::string();
    }
    return xml.substr(start, end - start);
}

cJSON* ParseJson(const std::string& body) {
    return cJSON_ParseWithLength(body.c_str(), body.size());
}

}  // namespace

std::string YandexMusic::GetToken() {
    Settings settings("music", true);
    return settings.GetString("ym_token", "");
}

void YandexMusic::SetToken(const std::string& token) {
    Settings settings("music", true);
    settings.SetString("ym_token", token);
}

bool YandexMusic::Get(const std::string& url, std::string& body, std::string& error) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (!http) {
        error = "no http client";
        return false;
    }
    http->SetTimeout(20000);
    http->SetHeader("Authorization", "OAuth " + GetToken());
    http->SetHeader("Accept", "application/json");
    http->SetHeader("Accept-Encoding", "identity");

    auto opened = http->Open("GET", url);
    if (!opened) {
        error = "open failed: " + opened.error().ToString();
        http->Close();
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status) {
        error = "status failed: " + status.error().ToString();
        http->Close();
        return false;
    }
    if (*status < 200 || *status >= 300) {
        error = "http " + std::to_string(*status);
        http->Close();
        return false;
    }

    char buffer[1024];
    body.clear();
    while (true) {
        auto read = http->Read(buffer, sizeof(buffer));
        if (!read) {
            error = "read failed: " + read.error().ToString();
            http->Close();
            return false;
        }
        if (*read == 0) {
            break;
        }
        body.append(buffer, *read);
    }
    http->Close();
    return true;
}

bool YandexMusic::EnsureUid(std::string& error) {
    if (uid_ > 0) {
        return true;
    }
    std::string body;
    if (!Get(std::string(kApiBase) + "/account/status", body, error)) {
        return false;
    }
    cJSON* root = ParseJson(body);
    if (root == nullptr) {
        error = "account/status: bad json";
        return false;
    }
    const cJSON* result = cJSON_GetObjectItem(root, "result");
    const cJSON* account = cJSON_GetObjectItem(result, "account");
    const cJSON* uid = cJSON_GetObjectItem(account, "uid");
    if (cJSON_IsNumber(uid)) {
        uid_ = static_cast<long>(uid->valuedouble);
    }
    cJSON_Delete(root);
    if (uid_ <= 0) {
        error = "account/status: no uid (bad token?)";
        return false;
    }
    ESP_LOGI(TAG, "uid=%ld", uid_);
    return true;
}

bool YandexMusic::ResolvePlaylist(const std::string& name, std::vector<YmTrack>& tracks,
                                  std::string& error) {
    tracks.clear();
    if (GetToken().empty()) {
        error = "no yandex token provisioned";
        return false;
    }
    if (!EnsureUid(error)) {
        return false;
    }

    int kind = -1;
    std::string title;
    std::string wanted = Lower(name);

    bool likes_requested = wanted.empty() || Contains(wanted, "мне нрав") ||
                           Contains(wanted, "любим") || Contains(wanted, "моя музыка") ||
                           Contains(wanted, "likes") || Contains(wanted, "favorite");

    if (!likes_requested) {
        std::string body;
        std::string list_error;
        if (Get(std::string(kApiBase) + "/users/" + std::to_string(uid_) + "/playlists/list", body,
                list_error)) {
            cJSON* root = ParseJson(body);
            const cJSON* playlists = cJSON_GetObjectItem(root, "result");
            if (cJSON_IsArray(playlists)) {
                const cJSON* item = nullptr;
                cJSON_ArrayForEach(item, playlists) {
                    std::string item_title = Lower(GetStringField(item, "title"));
                    const cJSON* item_kind = cJSON_GetObjectItem(item, "kind");
                    if (Contains(item_title, wanted) && cJSON_IsNumber(item_kind)) {
                        kind = item_kind->valueint;
                        title = GetStringField(item, "title");
                        break;
                    }
                }
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "playlists/list failed: %s", list_error.c_str());
        }
        if (kind < 0) {
            ESP_LOGW(TAG, "playlist '%s' not found, falling back to likes", name.c_str());
        }
    }

    if (kind < 0) {
        kind = kLikesKind;
        title = "likes";
    }

    std::string url = std::string(kApiBase) + "/users/" + std::to_string(uid_) + "/playlists/" +
                      std::to_string(kind) + "?rich-tracks=false";
    std::string body;
    if (!Get(url, body, error)) {
        return false;
    }

    cJSON* root = ParseJson(body);
    if (root == nullptr) {
        error = "playlist: bad json";
        return false;
    }
    const cJSON* result = cJSON_GetObjectItem(root, "result");
    const cJSON* list = cJSON_GetObjectItem(result, "tracks");
    if (cJSON_IsArray(list)) {
        const cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, list) {
            const cJSON* track = cJSON_GetObjectItem(entry, "track");
            if (!cJSON_IsObject(track)) {
                track = entry;
            }
            std::string id = GetStringField(track, "realId");
            if (id.empty()) {
                id = GetStringField(track, "id");
            }
            if (id.empty()) {
                continue;
            }
            YmTrack item;
            item.id = id;
            item.title = GetStringField(track, "title");
            tracks.push_back(item);
        }
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG, "playlist kind=%d title=%s tracks=%u", kind, title.c_str(),
             static_cast<unsigned>(tracks.size()));
    if (tracks.empty()) {
        error = "playlist is empty";
        return false;
    }
    return true;
}

bool YandexMusic::ResolveTrackUrl(const std::string& track_id, std::string& url,
                                  std::string& error) {
    std::string body;
    if (!Get(std::string(kApiBase) + "/tracks/" + track_id + "/download-info", body, error)) {
        return false;
    }

    cJSON* root = ParseJson(body);
    if (root == nullptr) {
        error = "download-info: bad json";
        return false;
    }

    std::string info_url;
    std::string codec = "mp3";
    int best_bitrate = -1;
    const cJSON* variants = cJSON_GetObjectItem(root, "result");
    if (cJSON_IsArray(variants)) {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, variants) {
            std::string item_codec = GetStringField(item, "codec");
            const cJSON* bitrate = cJSON_GetObjectItem(item, "bitrateInKbps");
            std::string item_url = GetStringField(item, "downloadInfoUrl");
            if (item_url.empty()) {
                continue;
            }
            int value = cJSON_IsNumber(bitrate) ? bitrate->valueint : 0;
            bool better_mp3 = item_codec == "mp3" && best_bitrate < 1000 && value > best_bitrate;
            bool first_any = best_bitrate < 0;
            if (better_mp3 || first_any) {
                info_url = item_url;
                codec = item_codec;
                best_bitrate = value;
            }
        }
    }
    cJSON_Delete(root);

    if (info_url.empty()) {
        error = "download-info: no variants";
        return false;
    }

    std::string xml;
    std::string xml_error;
    if (!Get(info_url, xml, xml_error)) {
        error = "download-info xml: " + xml_error;
        return false;
    }

    std::string host = ExtractTag(xml, "host");
    std::string path = ExtractTag(xml, "path");
    std::string ts = ExtractTag(xml, "ts");
    std::string signature = ExtractTag(xml, "s");
    if (host.empty() || path.empty() || ts.empty() || signature.empty()) {
        error = "download-info xml: missing fields";
        return false;
    }

    std::string sign = Md5Hex(std::string(kSignSalt) + path.substr(1) + signature);
    url = "https://" + host + "/get-" + codec + "/" + sign + "/" + ts + path;
    ESP_LOGI(TAG, "resolved %s -> %s (%d kbps)", track_id.c_str(), codec.c_str(), best_bitrate);
    return true;
}

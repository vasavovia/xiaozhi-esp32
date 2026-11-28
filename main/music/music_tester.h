#ifndef MUSIC_TESTER_H
#define MUSIC_TESTER_H

#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class YandexMusic;

// Playback path: HTTP(S) -> MP3 -> PCM -> Opus -> AudioService.
// Three entry points: a direct URL, a Yandex Music playlist by name, or «Моя волна» radio.
class MusicTester {
public:
    static MusicTester& GetInstance();

    bool StartUrl(const std::string& url, std::string& error);
    bool StartPlaylist(const std::string& playlist, std::string& error);
    bool StartWave(std::string& error);
    void Stop();
    bool IsPlaying();
    std::string Status();

private:
    MusicTester() = default;
    static void WorkerEntry(void* arg);
    void WorkerTask();
    void PlayWave(YandexMusic& client);
    void StreamOne(const std::string& url);
    bool StopRequested();

    std::mutex mutex_;
    TaskHandle_t task_handle_ = nullptr;
    bool stop_requested_ = false;
    bool playlist_mode_ = false;
    bool wave_mode_ = false;
    std::string url_;
    std::string playlist_;
};

void InitializeMusicTester();

// Serves /ym?token=... on port 8080 while no token is stored, so the Yandex
// token can be provisioned from another machine on the same LAN.
void StartTokenProvisioning();

#endif  // MUSIC_TESTER_H

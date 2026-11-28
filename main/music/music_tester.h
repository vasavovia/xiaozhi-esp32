#ifndef MUSIC_TESTER_H
#define MUSIC_TESTER_H

#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class YandexMusic;

// Playback path: HTTP(S) -> MP3 -> PCM -> Opus -> AudioService.
// Main entry point is internet radio; a direct URL and the Yandex helpers are still compiled.
class MusicTester {
public:
    static MusicTester& GetInstance();

    bool StartUrl(const std::string& url, std::string& error);
    bool StartRadio(const std::string& name, std::string& error);
    void NextStation();
    std::string CurrentStation();
    bool StartPlaylist(const std::string& playlist, std::string& error);
    bool StartWave(std::string& error);
    void Stop();
    bool IsPlaying();
    std::string Status();

private:
    MusicTester() = default;
    static void WorkerEntry(void* arg);
    void WorkerTask();
    void PlayRadio();
    void PlayWave(YandexMusic& client);
    void StreamOne(const std::string& url);
    bool StopRequested();

    std::mutex mutex_;
    TaskHandle_t task_handle_ = nullptr;
    bool stop_requested_ = false;
    bool skip_requested_ = false;
    bool playlist_mode_ = false;
    bool wave_mode_ = false;
    bool radio_mode_ = false;
    int radio_index_ = 0;
    std::string url_;
    std::string playlist_;
};

void InitializeMusicTester();

// Serves the music control endpoints on port 8080 once the station has an IP.
void StartTokenProvisioning();

#endif  // MUSIC_TESTER_H

#ifndef MUSIC_TESTER_H
#define MUSIC_TESTER_H

#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Minimal internet-radio playback path: HTTP(S) -> MP3 -> PCM -> Opus -> AudioService.
// MCP tools let the assistant start and stop a stream by voice.
class MusicTester {
public:
    static MusicTester& GetInstance();

    bool Start(const std::string& url, std::string& error);
    void Stop();
    bool IsPlaying();
    std::string Status();

private:
    MusicTester() = default;
    static void WorkerEntry(void* arg);
    void WorkerTask();

    std::mutex mutex_;
    TaskHandle_t task_handle_ = nullptr;
    bool stop_requested_ = false;
    std::string url_;
};

void InitializeMusicTester();

#endif  // MUSIC_TESTER_H

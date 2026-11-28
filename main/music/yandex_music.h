#ifndef YANDEX_MUSIC_H
#define YANDEX_MUSIC_H

#include <string>
#include <vector>

struct YmTrack {
    std::string id;
    std::string title;
};

// Minimal client for the unofficial Yandex Music API, verified 2026-11-24 and 2026-11-28:
//   GET /account/status                       -> result.account.uid
//   GET /users/<uid>/playlists/list           -> playlists (kind, title)
//   GET /users/<uid>/playlists/<kind>?rich-tracks=false -> tracks[].track.id (UUID)
//   GET /tracks/<id>/download-info            -> variants (codec, bitrateInKbps, downloadInfoUrl)
//   GET <downloadInfoUrl>                     -> XML host/path/ts/s -> signed /get-mp3/ URL
//   GET /rotor/station/user:onyourwave/tracks?settings2=true[&batchId=...] -> radio batch
//   POST /rotor/station/user:onyourwave/feedback {type,trackId,batchId,timestamp}
class YandexMusic {
public:
    // Playlist name as spoken by the user: "мне нравится"/"likes"/"моя музыка",
    // or a substring of a playlist title, or "" for the likes playlist.
    bool ResolvePlaylist(const std::string& name, std::vector<YmTrack>& tracks, std::string& error);
    bool ResolveTrackUrl(const std::string& track_id, std::string& url, std::string& error);

    // «Моя волна»: each call returns the next batch and advances the stored batch id.
    bool FetchWaveBatch(std::vector<YmTrack>& tracks, std::string& error);
    void SendWaveFeedback(const std::string& type, const std::string& track_id);

    static std::string GetToken();
    static void SetToken(const std::string& token);

private:
    bool EnsureUid(std::string& error);
    bool Get(const std::string& url, std::string& body, std::string& error);
    bool PostJson(const std::string& url, const std::string& body, std::string& response,
                  std::string& error);

    long uid_ = 0;
    std::string wave_batch_id_;
};

#endif  // YANDEX_MUSIC_H

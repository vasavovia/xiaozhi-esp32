#ifndef YANDEX_MUSIC_H
#define YANDEX_MUSIC_H

#include <string>
#include <vector>

struct YmTrack {
    std::string id;
    std::string title;
};

// Minimal client for the unofficial Yandex Music API, verified 2026-11-24:
//   GET /account/status                       -> result.account.uid
//   GET /users/<uid>/playlists/list           -> playlists (kind, title)
//   GET /users/<uid>/playlists/<kind>?rich-tracks=false -> tracks[].track.id (UUID)
//   GET /tracks/<id>/download-info            -> variants (codec, bitrateInKbps, downloadInfoUrl)
//   GET <downloadInfoUrl>                     -> XML host/path/ts/s -> signed /get-mp3/ URL
class YandexMusic {
public:
    // Playlist name as spoken by the user: "мне нравится"/"likes"/"моя музыка",
    // or a substring of a playlist title, or "" for the likes playlist.
    bool ResolvePlaylist(const std::string& name, std::vector<YmTrack>& tracks, std::string& error);
    bool ResolveTrackUrl(const std::string& track_id, std::string& url, std::string& error);

    static std::string GetToken();
    static void SetToken(const std::string& token);

private:
    bool EnsureUid(std::string& error);
    bool Get(const std::string& url, std::string& body, std::string& error);

    long uid_ = 0;
};

#endif  // YANDEX_MUSIC_H

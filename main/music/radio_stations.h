#ifndef RADIO_STATIONS_H
#define RADIO_STATIONS_H

// Streams verified on 2026-11-28 to answer with raw MP3 (audio/mpeg, MPEG frame sync ff fb),
// which is what the on-board decoder can play. AAC/HLS stations were rejected.
//
// name  — what the user says and what /status reports.
// panel — the same station laid out for the 80 px HUD column; the line breaks are explicit so
//         the 20 px font never has to fit a whole long name on one line, and «Дорожное» is
//         hyphenated because it cannot be broken at a space.
struct RadioStation {
    const char* name;
    const char* panel;
    const char* url;
};

static const RadioStation kRadioStations[] = {
    {"\xd0\x95\xd0\xb2\xd1\x80\xd0\xbe\xd0\xbf\xd0\xb0 \xd0\x9f\xd0\xbb\xd1\x8e\xd1\x81",
     "\xd0\x95\xd0\xb2\xd1\x80\xd0\xbe\xd0\xbf\xd0\xb0\n\xd0\x9f\xd0\xbb\xd1\x8e\xd1\x81",
     "https://ep128.hostingradio.ru:8030/ep128"},
    {"\xd0\xa0\xd0\xb5\xd1\x82\xd1\x80\xd0\xbe FM", "\xd0\xa0\xd0\xb5\xd1\x82\xd1\x80\xd0\xbe\nFM",
     "https://retroserver.streamr.ru:8043/retro256.mp3"},
    {"\xd0\x94\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb6\xd0\xbd\xd0\xbe\xd0\xb5",
     "\xd0\x94\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb6-\n\xd0\xbd\xd0\xbe\xd0\xb5",
     "https://dorognoe.hostingradio.ru:8000/dorognoe"},
    {"\xd0\x9d\xd0\xb0\xd1\x88\xd0\xb5 \xd1\x80\xd0\xb0\xd0\xb4\xd0\xb8\xd0\xbe",
     "\xd0\x9d\xd0\xb0\xd1\x88\xd0\xb5\n\xd1\x80\xd0\xb0\xd0\xb4\xd0\xb8\xd0\xbe",
     "https://nashe1.hostingradio.ru:80/nashe-128.mp3"},
    {"SomaFM Groove Salad", "SomaFM\nGroove", "https://ice1.somafm.com/groovesalad-128-mp3"},
    {"SomaFM Deep Space One", "Deep\nSpace", "https://ice1.somafm.com/deepspaceone-128-mp3"},
};

static constexpr int kRadioStationCount =
    static_cast<int>(sizeof(kRadioStations) / sizeof(kRadioStations[0]));

#endif  // RADIO_STATIONS_H

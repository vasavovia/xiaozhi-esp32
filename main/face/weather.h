#ifndef WEATHER_H
#define WEATHER_H

// Fetches the current weather for Yekaterinburg from open-meteo (keyless) once an hour
// and keeps the last value in memory for the on-screen dashboard.
void InitializeWeather();

// Returns false until the first successful fetch. Wind is in metres per second.
bool GetWeather(int& temp_c, int& wmo_code, float& wind_mps);

#endif  // WEATHER_H

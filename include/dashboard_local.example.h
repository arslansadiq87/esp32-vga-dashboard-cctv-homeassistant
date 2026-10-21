#pragma once
#include "dashboard_config.h"

// Example only. Copy to dashboard_local.h and fill in your own values.
// Never commit dashboard_local.h: it contains Wi-Fi and HA credentials.
static const DashboardConfig DASHBOARD_CONFIG = {
    "YOUR_WIFI_SSID",
    "YOUR_WIFI_PASSWORD",
    "http://YOUR_HOME_ASSISTANT_HOST:8123",
    "YOUR_LONG_LIVED_ACCESS_TOKEN",
    nullptr,
    "YOUR_POSIX_TIME_ZONE",
    "sun.sun",
    {
        {"CARD 1", "YOUR_ENTITY_ID_1", "W"},
        {"CARD 2", "YOUR_ENTITY_ID_2", "W"},
        {"CARD 3", "YOUR_ENTITY_ID_3", "W"},
        {"CARD 4", "YOUR_ENTITY_ID_4", "%"},
        {"CARD 5", "YOUR_ENTITY_ID_5", "W"},
        {"CARD 6", "YOUR_ENTITY_ID_6", "W"},
        {"CARD 7", "YOUR_ENTITY_ID_7", "W"},
        {"CARD 8", "YOUR_ENTITY_ID_8", "%"},
        {"CARD 9", "YOUR_ENTITY_ID_9", "W"},
        {"CARD 10", "YOUR_ENTITY_ID_10", "W"},
        {"CARD 11", "YOUR_ENTITY_ID_11", "V"},
        {"CARD 12", "YOUR_ENTITY_ID_12", "W"},
        {"CARD 13", "YOUR_ENTITY_ID_13", "ppm"},
        {"CARD 14", "YOUR_ENTITY_ID_14", "%"},
        {"CARD 15", "YOUR_ENTITY_ID_15", ""},
        {"CARD 16", "YOUR_ENTITY_ID_16", ""},
    },
    "YOUR_ROOM_TEMPERATURE_ENTITY",
    5
};

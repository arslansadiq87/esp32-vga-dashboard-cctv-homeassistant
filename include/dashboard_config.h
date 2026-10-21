#pragma once

#define DASHBOARD_CARD_COUNT 16

struct DashboardCardConfig {
    const char *label;
    const char *entityId;
    const char *unit;
};

struct DashboardConfig {
    const char *wifiSsid;
    const char *wifiPassword;
    const char *haUrl;
    const char *haToken;
    const char *haCaCert;
    const char *timeZone;
    const char *sunEntity;
    DashboardCardConfig cards[DASHBOARD_CARD_COUNT];
    const char *roomTemperature;
    unsigned pollSeconds;
};

#if __has_include("dashboard_local.h")
#include "dashboard_local.h"
#else
static const DashboardConfig DASHBOARD_CONFIG = {};
#endif

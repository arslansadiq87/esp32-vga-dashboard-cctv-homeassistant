#pragma once

#include "dashboard_config.h"
#include <stdint.h>

struct Reading {
    float value = 0;
    bool valid = false;
};

struct CardData {
    Reading numeric;
    bool textValid = false;
    char text[32] = {};
};

struct DashboardData {
    CardData cards[DASHBOARD_CARD_COUNT];
    Reading roomTemperature;
    char sunrise[8] = "--:--";
    char sunset[8] = "--:--";
    bool sunTimesValid = false;
    bool haConnected = false;
    bool configMissing = false;
    uint32_t updatedMs = 0;
    uint32_t lastAttemptMs = 0;
    uint32_t requestDurationMs = 0;
};

bool dashboardConfigReady();
bool dashboardStartPolling();
bool dashboardTakeSnapshot(DashboardData &out);
bool dashboardTryBeginRender();
void dashboardEndRender();

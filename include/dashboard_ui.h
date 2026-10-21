#pragma once

#include "dashboard_data.h"
#include <lvgl.h>
#include <stdint.h>

void dashboardBuildUi();
bool dashboardUpdateUi(const DashboardData &data);
void dashboardUiSetCameraFrame(const uint16_t *pixels, uint16_t width, uint16_t height);
void dashboardUiSetCameraStatus(const char *status);

#pragma once

#include <Arduino.h>

bool cameraViewBegin();
bool cameraViewRefreshIfDue();
const uint16_t *cameraViewPixels();
uint16_t cameraViewWidth();
uint16_t cameraViewHeight();
const char *cameraViewStatus();

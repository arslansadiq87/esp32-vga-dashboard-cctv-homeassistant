#include "dashboard_data.h"
#include "dashboard_config.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cmath>
#include <memory>
#include <time.h>
#include <cstring>

static QueueHandle_t updates = nullptr;
static SemaphoreHandle_t apiRenderMutex = nullptr;

bool dashboardTryBeginRender() {
    return !apiRenderMutex || xSemaphoreTake(apiRenderMutex, 0) == pdTRUE;
}

void dashboardEndRender() {
    if (apiRenderMutex) xSemaphoreGive(apiRenderMutex);
}

static bool configured(const char *s) {
    return s && s[0] && strncmp(s, "YOUR_", 5) != 0;
}

bool dashboardConfigReady() {
    return configured(DASHBOARD_CONFIG.wifiSsid) &&
           configured(DASHBOARD_CONFIG.haUrl) &&
           configured(DASHBOARD_CONFIG.haToken) &&
           (!String(DASHBOARD_CONFIG.haUrl).startsWith("https://") ||
            configured(DASHBOARD_CONFIG.haCaCert));
}

// Restrict path input to ordinary HA entity IDs; prevents malformed URLs.
static bool validEntityId(const char *id) {
    if (!configured(id)) return false;
    for (const char *p = id; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '.')) return false;
    }
    return strchr(id, '.') != nullptr;
}

static bool fetchEntity(const char *id, StaticJsonDocument<768> &json,
                        bool &reachable, bool logDetails) {
    if (!validEntityId(id)) {
        if (logDetails) Serial.printf("HA entity ID missing or invalid: %s\n", id ? id : "(not configured)");
        return false;
    }
    String base(DASHBOARD_CONFIG.haUrl);
    while (base.endsWith("/")) base.remove(base.length() - 1);
    const bool secure = base.startsWith("https://");
    if (!secure && !base.startsWith("http://")) return false;
    std::unique_ptr<WiFiClient> client;
    if (secure) {
        if (!configured(DASHBOARD_CONFIG.haCaCert)) return false;
        auto tls = std::make_unique<WiFiClientSecure>();
        tls->setCACert(DASHBOARD_CONFIG.haCaCert);
        client = std::move(tls);
    } else client = std::make_unique<WiFiClient>();
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    if (apiRenderMutex) xSemaphoreTake(apiRenderMutex, portMAX_DELAY);
    const uint32_t started = millis();
    if (!http.begin(*client, base + "/api/states/" + id)) {
        if (logDetails) Serial.printf("HA %s: request setup failed\n", id);
        if (apiRenderMutex) xSemaphoreGive(apiRenderMutex);
        return false;
    }
    http.addHeader("Authorization", String("Bearer ") + DASHBOARD_CONFIG.haToken);
    http.addHeader("Accept", "application/json");
    const int response = http.GET();
    bool ok = false;
    if (response == HTTP_CODE_OK) {
        StaticJsonDocument<256> filter;
        filter["state"] = true;
        filter["attributes"]["unit_of_measurement"] = true;
        filter["attributes"]["current_temperature"] = true;
        filter["attributes"]["temperature"] = true;
        filter["attributes"]["temperature_unit"] = true;
        filter["attributes"]["next_rising"] = true;
        filter["attributes"]["next_setting"] = true;
        ok = !deserializeJson(json, http.getStream(), DeserializationOption::Filter(filter));
    }
    if (logDetails)
        Serial.printf("HA %s: HTTP %d, %lu ms, JSON %s\n", id, response,
                      static_cast<unsigned long>(millis() - started), ok ? "OK" : "INVALID");
    if (ok) reachable = true;
    http.end();
    if (apiRenderMutex) xSemaphoreGive(apiRenderMutex);
    // Space individual requests; VGA can redraw between HTTP transactions.
    vTaskDelay(pdMS_TO_TICKS(250));
    return ok;
}

static Reading numberFrom(const char *text, const char *unit, char kind) {
    Reading out;
    if (!text || !text[0] || strcmp(text, "unknown") == 0 ||
        strcmp(text, "unavailable") == 0) return out;
    char *end = nullptr;
    const float raw = strtof(text, &end);
    while (end && *end == ' ') ++end;
    if (end == text || !end || *end || !std::isfinite(raw)) return out;
    unit = unit ? unit : "";
    float value = raw;
    if (kind == 'W') {
        if (strcmp(unit, "kW") == 0) value *= 1000.0f;
        else if (unit[0] && strcmp(unit, "W") != 0) return out;
    } else if (kind == '%') {
        if (unit[0] && strcmp(unit, "%") != 0) return out;
        if (value < 0 || value > 100) return out;
    } else if (kind == 'P') {
        if (unit[0] && strcmp(unit, "ppm") != 0) return out;
        if (value < 0) return out;
    } else if (kind == 'C') {
        if (strcmp(unit, "\xC2\xB0" "F") == 0 || strcmp(unit, "F") == 0)
            value = (value - 32.0f) * (5.0f / 9.0f);
        else if (unit[0] && strcmp(unit, "\xC2\xB0" "C") != 0 && strcmp(unit, "C") != 0)
            return out;
    } else if (kind != 'N') return out;
    if (!std::isfinite(value) || fabsf(value) > 999999.0f) return out;
    out.value = value;
    out.valid = true;
    return out;
}

static Reading fetchNumber(const char *id, char kind, bool &reachable, bool logDetails) {
    StaticJsonDocument<768> json;
    if (!fetchEntity(id, json, reachable, logDetails)) return {};
    Reading result = numberFrom(json["state"] | "",
                                json["attributes"]["unit_of_measurement"] | "", kind);
    if (logDetails)
        Serial.printf("HA value %s: %s %.2f %c\n", id,
                      result.valid ? "VALID" : "UNAVAILABLE", result.value, kind);
    return result;
}

static char numericKindForUnit(const char *unit) {
    if (!unit) return 'N';
    if (strcmp(unit, "W") == 0) return 'W';
    if (strcmp(unit, "%") == 0) return '%';
    if (strcmp(unit, "ppm") == 0) return 'P';
    if (strcmp(unit, "C") == 0 || strcmp(unit, "\xC2\xB0" "C") == 0) return 'C';
    return 'N';
}

// Home Assistant sun timestamps are ISO-8601 UTC. Convert them through the
// configured local timezone used by configTzTime(), without assuming a zone.
static bool formatSunTime(const char *iso, char *out, size_t outSize) {
    if (!iso || !iso[0] || !out || outSize < 6) return false;
    int year, month, day, hour, minute, second = 0;
    const int fields = sscanf(iso, "%d-%d-%dT%d:%d:%d",
                              &year, &month, &day, &hour, &minute, &second);
    if (fields < 5 || year < 2000 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 60)
        return false;
    time_t now = time(nullptr);
    struct tm utcNow;
    if (now < 1700000000 || !gmtime_r(&now, &utcNow)) return false;
    const time_t utcAsLocal = mktime(&utcNow);
    if (utcAsLocal == static_cast<time_t>(-1)) return false;
    const long localOffset = static_cast<long>(difftime(now, utcAsLocal));

    struct tm utcEvent = {};
    utcEvent.tm_year = year - 1900;
    utcEvent.tm_mon = month - 1;
    utcEvent.tm_mday = day;
    utcEvent.tm_hour = hour;
    utcEvent.tm_min = minute;
    utcEvent.tm_sec = second;
    const time_t eventAsLocal = mktime(&utcEvent);
    if (eventAsLocal == static_cast<time_t>(-1)) return false;
    const time_t eventUtc = eventAsLocal + localOffset;
    struct tm localEvent;
    if (!localtime_r(&eventUtc, &localEvent)) return false;
    return strftime(out, outSize, "%H:%M", &localEvent) != 0;
}

static DashboardData pollHomeAssistant(bool logDetails) {
    DashboardData d;
    bool reachable = false;
    for (size_t i = 0; i < DASHBOARD_CARD_COUNT; ++i) {
        const DashboardCardConfig &cfg = DASHBOARD_CONFIG.cards[i];
        CardData &card = d.cards[i];
        if (!configured(cfg.entityId)) continue;
        StaticJsonDocument<768> json;
        if (!fetchEntity(cfg.entityId, json, reachable, logDetails)) continue;
        const char *state = json["state"] | "";
        const char *haUnit = json["attributes"]["unit_of_measurement"] | "";
        card.numeric = numberFrom(state, haUnit, numericKindForUnit(cfg.unit));
        if (!card.numeric.valid && state[0] && strcmp(state, "unknown") != 0 &&
            strcmp(state, "unavailable") != 0) {
            card.textValid = true;
            size_t n = 0;
            for (; state[n] && n + 1 < sizeof(card.text); ++n) {
                const char c = state[n];
                card.text[n] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
            }
            card.text[n] = '\0';
        }
        if (logDetails)
            Serial.printf("HA card %u [%s] entity=%s: %s %.2f %s\n",
                          static_cast<unsigned>(i + 1), cfg.label ? cfg.label : "(unlabeled)",
                          cfg.entityId, card.numeric.valid ? "VALID" :
                          card.textValid ? card.text : "UNAVAILABLE",
                          card.numeric.value, cfg.unit ? cfg.unit : "");
    }
    if (configured(DASHBOARD_CONFIG.roomTemperature))
        d.roomTemperature = fetchNumber(DASHBOARD_CONFIG.roomTemperature, 'C', reachable, logDetails);
    if (configured(DASHBOARD_CONFIG.sunEntity)) {
        StaticJsonDocument<768> json;
        if (fetchEntity(DASHBOARD_CONFIG.sunEntity, json, reachable, logDetails)) {
            const char *nextRise = json["attributes"]["next_rising"] | "";
            const char *nextSet = json["attributes"]["next_setting"] | "";
            const bool riseOk = formatSunTime(nextRise, d.sunrise, sizeof(d.sunrise));
            const bool setOk = formatSunTime(nextSet, d.sunset, sizeof(d.sunset));
            d.sunTimesValid = riseOk && setOk;
            if (logDetails)
                Serial.printf("HA sun %s: sunrise=%s sunset=%s\n",
                              DASHBOARD_CONFIG.sunEntity,
                              riseOk ? d.sunrise : "unavailable",
                              setOk ? d.sunset : "unavailable");
        }
    }
    d.haConnected = reachable;
    d.lastAttemptMs = millis();
    return d;
}

static void pollingTask(void *) {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    bool timeConfigured = false;
    bool wasWifiConnected = false;
    bool wasHaConnected = false;
    bool hasLoggedDetails = false;
    uint32_t lastDetailsMs = 0;
    uint32_t lastSuccessfulMs = 0;
    uint32_t lastWifiPendingLogMs = 0;
    for (;;) {
        const uint32_t pollStarted = millis();
        if (WiFi.status() != WL_CONNECTED) {
            if (wasWifiConnected) Serial.println("Wi-Fi disconnected; reconnecting in background.");
            wasWifiConnected = false;
            WiFi.begin(DASHBOARD_CONFIG.wifiSsid, DASHBOARD_CONFIG.wifiPassword);
            const uint32_t started = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - started < 10000)
                vTaskDelay(pdMS_TO_TICKS(250));
        }
        const bool wifiConnected = WiFi.status() == WL_CONNECTED;
        if (wifiConnected && !wasWifiConnected)
            Serial.printf("Wi-Fi connected, IP: %s\n", WiFi.localIP().toString().c_str());
        else if (!wifiConnected &&
                 (!lastWifiPendingLogMs || millis() - lastWifiPendingLogMs >= 30000)) {
            Serial.println("Wi-Fi connection pending; dashboard remains active.");
            lastWifiPendingLogMs = millis();
        }
        wasWifiConnected = wifiConnected;
        DashboardData next;
        const bool logDetails = !hasLoggedDetails || millis() - lastDetailsMs >= 60000;
        if (wifiConnected) {
            if (!timeConfigured && configured(DASHBOARD_CONFIG.timeZone)) {
                configTzTime(DASHBOARD_CONFIG.timeZone, "pool.ntp.org", "time.nist.gov");
                timeConfigured = true;
            }
            next = pollHomeAssistant(logDetails);
            hasLoggedDetails = true;
            if (logDetails) lastDetailsMs = millis();
        }
        bool validData = next.roomTemperature.valid;
        for (const CardData &card : next.cards)
            validData |= card.numeric.valid || card.textValid;
        if (validData) lastSuccessfulMs = millis();
        next.updatedMs = lastSuccessfulMs;
        next.lastAttemptMs = millis();
        next.requestDurationMs = millis() - pollStarted;
        if (next.haConnected != wasHaConnected)
            Serial.printf("Home Assistant API: %s\n", next.haConnected ? "CONNECTED" : "OFFLINE");
        wasHaConnected = next.haConnected;
        if (logDetails && wifiConnected)
            Serial.printf("HA update: poll=%lu ms; last_success=%lu ms; heap=%u; PSRAM=%u\n",
                          static_cast<unsigned long>(next.requestDurationMs),
                          static_cast<unsigned long>(lastSuccessfulMs),
                          ESP.getFreeHeap(), ESP.getFreePsram());
        xQueueOverwrite(updates, &next);
        const unsigned seconds = DASHBOARD_CONFIG.pollSeconds < 5 ? 5 :
                                 min(DASHBOARD_CONFIG.pollSeconds, 300U);
        const uint32_t intervalMs = seconds * 1000U;
        const uint32_t elapsed = millis() - pollStarted;
        vTaskDelay(pdMS_TO_TICKS(elapsed < intervalMs ? intervalMs - elapsed : 250));
    }
}

bool dashboardStartPolling() {
    if (!dashboardConfigReady()) return false;
    updates = xQueueCreate(1, sizeof(DashboardData));
    apiRenderMutex = xSemaphoreCreateMutex();
    return updates && apiRenderMutex &&
           xTaskCreatePinnedToCore(pollingTask, "ha_poll", 8192,
                                              nullptr, 1, nullptr, 0) == pdPASS;
}

bool dashboardTakeSnapshot(DashboardData &out) {
    return updates && xQueueReceive(updates, &out, 0) == pdTRUE;
}

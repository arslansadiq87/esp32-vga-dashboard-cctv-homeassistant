#include "dashboard_ui.h"

#include "dashboard_config.h"
#include <Arduino.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {
namespace C {
constexpr uint32_t BLACK = 0x000000;
constexpr uint32_t PANEL = 0x101719;
constexpr uint32_t PANEL_ALT = 0x141A1D;
constexpr uint32_t BORDER = 0x35433F;
constexpr uint32_t DIM = 0x81958D;
constexpr uint32_t WHITE = 0xE7F2EA;
constexpr uint32_t GREEN = 0xA5FF4D;
constexpr uint32_t CYAN = 0x52F5D2;
constexpr uint32_t AMBER = 0xFFCC54;
constexpr uint32_t MAGENTA = 0xFF62C8;
constexpr uint32_t BLUE = 0x69BFFF;
constexpr uint32_t PURPLE = 0xBC91FF;
}

struct CardWidget { lv_obj_t *value = nullptr; };
CardWidget cards[DASHBOARD_CARD_COUNT];
lv_obj_t *clockLabel, *dateLabel, *sunLabel, *headerStatus;
lv_obj_t *footerTemp, *footerStatus, *footerAge, *cameraStatus;
lv_obj_t *cameraImage;
lv_img_dsc_t cameraDescriptor = {};
const uint32_t accents[DASHBOARD_CARD_COUNT] = {
    C::AMBER, C::CYAN, C::GREEN, C::PURPLE,
    C::BLUE, C::CYAN, C::AMBER, C::GREEN,
    C::PURPLE, C::BLUE, C::MAGENTA, C::CYAN,
    C::GREEN, C::AMBER, C::BLUE, C::WHITE
};

lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, int x, int y,
                    int width, const lv_font_t *font, uint32_t color) {
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(obj, width, LV_SIZE_CONTENT);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    return obj;
}

lv_obj_t *makePanel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t fill) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, 4, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(C::BORDER), 0);
    return obj;
}

void setAccentBar(lv_obj_t *box, uint32_t color, int height) {
    lv_obj_t *bar = lv_obj_create(box);
    lv_obj_set_pos(bar, 0, 8);
    lv_obj_set_size(bar, 3, height);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
}

bool setText(lv_obj_t *obj, const char *text) {
    if (!obj || strcmp(lv_label_get_text(obj), text) == 0) return false;
    lv_label_set_text(obj, text);
    return true;
}

bool setTextColor(lv_obj_t *obj, uint32_t color) {
    if (!obj) return false;
    const lv_color_t next = lv_color_hex(color);
    if (lv_obj_get_style_text_color(obj, 0).full == next.full) return false;
    lv_obj_set_style_text_color(obj, next, 0);
    return true;
}

void formatValue(char *out, size_t size, const CardData &data, const char *unit) {
    unit = unit ? unit : "";
    if (!data.numeric.valid) {
        snprintf(out, size, "%s", data.textValid ? data.text : "--");
        return;
    }
    const float nearest = roundf(data.numeric.value);
    const bool fractional = fabsf(data.numeric.value - nearest) >= 0.05f;
    if (fractional)
        snprintf(out, size, "%.1f%s%s", data.numeric.value,
                 unit[0] == '%' ? "" : (unit[0] ? " " : ""), unit);
    else
        snprintf(out, size, "%ld%s%s", lroundf(data.numeric.value),
                 unit[0] == '%' ? "" : (unit[0] ? " " : ""), unit);
}

CardWidget makeCard(lv_obj_t *screen, int x, int y, int w, int h,
                    const char *title, uint32_t accent, bool primary) {
    CardWidget widget;
    lv_obj_t *box = makePanel(screen, x, y, w, h, primary ? C::PANEL : C::PANEL_ALT);
    setAccentBar(box, accent, h - 16);
    makeLabel(box, title, 11, primary ? 8 : 5, w - 18,
              primary ? &lv_font_montserrat_12 : &lv_font_montserrat_10, C::DIM);
    widget.value = makeLabel(box, "--", 11, primary ? 31 : 23, w - 18,
                             primary ? &lv_font_montserrat_22 : &lv_font_montserrat_16,
                             accent);
    return widget;
}
} // namespace

void dashboardBuildUi() {
    lv_obj_t *screen = lv_scr_act();
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(C::BLACK), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    makeLabel(screen, "UTILITY ROOM", 14, 8, 330,
              &lv_font_montserrat_24, C::GREEN);
    makeLabel(screen, "RETRO HOME SYSTEM  /  VGA-01", 16, 37, 290,
              &lv_font_montserrat_10, C::DIM);
    headerStatus = makeLabel(screen, "HA OFFLINE", 16, 51, 260,
                             &lv_font_montserrat_10, C::AMBER);

    clockLabel = makeLabel(screen, "--:--", 410, 3, 216,
                           &lv_font_montserrat_32, C::CYAN);
    lv_obj_set_style_text_align(clockLabel, LV_TEXT_ALIGN_RIGHT, 0);
    dateLabel = makeLabel(screen, "---, -- ---", 410, 39, 216,
                          &lv_font_montserrat_14, C::WHITE);
    lv_obj_set_style_text_align(dateLabel, LV_TEXT_ALIGN_RIGHT, 0);
    sunLabel = makeLabel(screen, "SUN --:-- / --:--", 410, 58, 216,
                         &lv_font_montserrat_10, C::AMBER);
    lv_obj_set_style_text_align(sunLabel, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *topRule = lv_obj_create(screen);
    lv_obj_set_pos(topRule, 14, 75);
    lv_obj_set_size(topRule, 612, 1);
    lv_obj_set_style_pad_all(topRule, 0, 0);
    lv_obj_set_style_border_width(topRule, 0, 0);
    lv_obj_set_style_bg_color(topRule, lv_color_hex(C::BORDER), 0);

    const int primaryX[2] = {14, 210};
    const int primaryY[2] = {83, 169};
    for (int i = 0; i < 4; ++i) {
        const DashboardCardConfig &cfg = DASHBOARD_CONFIG.cards[i];
        cards[i] = makeCard(screen, primaryX[i % 2], primaryY[i / 2], 186, 78,
                            cfg.label ? cfg.label : "CARD", accents[i], true);
    }

    lv_obj_t *camera = makePanel(screen, 406, 83, 220, 164, C::PANEL);
    setAccentBar(camera, C::CYAN, 148);
    makeLabel(camera, "CAMERA / CH 01", 10, 5, 195,
              &lv_font_montserrat_10, C::CYAN);
    cameraImage = lv_img_create(camera);
    lv_obj_set_pos(cameraImage, 10, 22);
    lv_obj_set_size(cameraImage, 200, 136);
    lv_obj_set_style_bg_color(cameraImage, lv_color_hex(0x050809), 0);
    lv_obj_set_style_bg_opa(cameraImage, LV_OPA_COVER, 0);
    lv_obj_set_style_img_recolor_opa(cameraImage, LV_OPA_TRANSP, 0);
    cameraStatus = makeLabel(camera, "WAIT WIFI", 12, 61, 196,
                             &lv_font_montserrat_12, C::AMBER);
    lv_obj_set_style_text_align(cameraStatus, LV_TEXT_ALIGN_CENTER, 0);

    const int smallX[4] = {14, 168, 322, 476};
    const int smallY[3] = {258, 319, 380};
    for (int i = 4; i < DASHBOARD_CARD_COUNT; ++i) {
        const int n = i - 4;
        const DashboardCardConfig &cfg = DASHBOARD_CONFIG.cards[i];
        cards[i] = makeCard(screen, smallX[n % 4], smallY[n / 4], 146, 54,
                            cfg.label ? cfg.label : "CARD", accents[i], false);
    }

    lv_obj_t *rule = lv_obj_create(screen);
    lv_obj_set_pos(rule, 14, 444);
    lv_obj_set_size(rule, 612, 1);
    lv_obj_set_style_pad_all(rule, 0, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(C::BORDER), 0);
    footerTemp = makeLabel(screen, "ROOM --.- C", 14, 452, 180,
                           &lv_font_montserrat_12, C::WHITE);
    footerStatus = makeLabel(screen, "HA OFFLINE", 222, 452, 196,
                             &lv_font_montserrat_12, C::AMBER);
    lv_obj_set_style_text_align(footerStatus, LV_TEXT_ALIGN_CENTER, 0);
    footerAge = makeLabel(screen, "NO UPDATE", 440, 452, 186,
                          &lv_font_montserrat_12, C::DIM);
    lv_obj_set_style_text_align(footerAge, LV_TEXT_ALIGN_RIGHT, 0);
}

bool dashboardUpdateUi(const DashboardData &data) {
    bool changed = false;
    char text[64];
    for (size_t i = 0; i < DASHBOARD_CARD_COUNT; ++i) {
        const DashboardCardConfig &cfg = DASHBOARD_CONFIG.cards[i];
        formatValue(text, sizeof(text), data.cards[i], cfg.unit);
        changed |= setText(cards[i].value, text);
    }

    const uint32_t age = data.updatedMs ? (millis() - data.updatedMs) / 1000UL : 0;
    const uint32_t staleAfter = DASHBOARD_CONFIG.pollSeconds < 5 ? 15U :
                                DASHBOARD_CONFIG.pollSeconds * 3U;
    const bool stale = data.updatedMs && age > staleAfter;
    const char *haState = data.configMissing ? "CONFIG REQUIRED" :
                          !data.haConnected ? "HA OFFLINE" :
                          stale ? "HA DATA STALE" : "HA CONNECTED";
    const uint32_t haColor = data.haConnected && !stale ? C::GREEN : C::AMBER;
    changed |= setText(headerStatus, haState);
    changed |= setText(footerStatus, haState);
    changed |= setTextColor(headerStatus, haColor);
    changed |= setTextColor(footerStatus, haColor);

    if (data.roomTemperature.valid)
        snprintf(text, sizeof(text), "ROOM %.1f C", data.roomTemperature.value);
    else
        snprintf(text, sizeof(text), "ROOM --.- C");
    changed |= setText(footerTemp, text);
    if (data.updatedMs) snprintf(text, sizeof(text), "UPDATED %lus", (age / 5U) * 5U);
    else snprintf(text, sizeof(text), "NO UPDATE");
    changed |= setText(footerAge, text);

    const time_t now = time(nullptr);
    struct tm local = {};
    if (now > 1700000000 && localtime_r(&now, &local))
        strftime(text, sizeof(text), "%H:%M", &local);
    else
        snprintf(text, sizeof(text), "--:--");
    changed |= setText(clockLabel, text);
    if (now > 1700000000 && localtime_r(&now, &local))
        strftime(text, sizeof(text), "%a, %d %b", &local);
    else
        snprintf(text, sizeof(text), "DATE UNAVAILABLE");
    changed |= setText(dateLabel, text);
    snprintf(text, sizeof(text), "SUN %s / %s",
             data.sunrise[0] ? data.sunrise : "--:--",
             data.sunset[0] ? data.sunset : "--:--");
    changed |= setText(sunLabel, text);
    return changed;
}

void dashboardUiSetCameraFrame(const uint16_t *pixels, uint16_t width, uint16_t height) {
    if (!pixels || !cameraImage || !width || !height) return;
    cameraDescriptor.header.always_zero = 0;
    cameraDescriptor.header.w = width;
    cameraDescriptor.header.h = height;
    cameraDescriptor.header.cf = LV_IMG_CF_TRUE_COLOR;
    cameraDescriptor.data_size = static_cast<uint32_t>(width) * height * sizeof(uint16_t);
    cameraDescriptor.data = reinterpret_cast<const uint8_t *>(pixels);
    lv_img_set_src(cameraImage, &cameraDescriptor);
    lv_obj_set_pos(cameraImage, 10, 22);
    lv_obj_add_flag(cameraStatus, LV_OBJ_FLAG_HIDDEN);
}

void dashboardUiSetCameraStatus(const char *status) {
    if (!cameraStatus || !status) return;
    setText(cameraStatus, status);
    if (strncmp(status, "CAMERA ONLINE", 13) == 0) {
        setTextColor(cameraStatus, C::GREEN);
        lv_obj_add_flag(cameraStatus, LV_OBJ_FLAG_HIDDEN);
    } else {
        setTextColor(cameraStatus, C::AMBER);
        lv_obj_clear_flag(cameraStatus, LV_OBJ_FLAG_HIDDEN);
    }
}

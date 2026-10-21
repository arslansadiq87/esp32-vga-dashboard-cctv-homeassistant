#include <Arduino.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_heap_caps.h>
#include <soc/lcd_cam_struct.h>
#include <sdkconfig.h>
#include <lvgl.h>
#include "dashboard_data.h"
#include "dashboard_ui.h"
#include "camera_view.h"
#include <cstring>

// Standard RGB565 word: B0..B4, G0..G5, R0..R4.
// These are LCD data lanes, NOT a change to the physical RGB wiring.
static constexpr int VGA_DATA_PINS[16] = {
    15, 16, 17, 18, 21, 9, 10, 11, 12, 13, 14, 4, 5, 6, 7, 8
};
static_assert(VGA_DATA_PINS[0] == 15 && VGA_DATA_PINS[4] == 21,
              "Blue LSB/MSB pin mismatch");
static_assert(VGA_DATA_PINS[5] == 9 && VGA_DATA_PINS[10] == 14,
              "Green LSB/MSB pin mismatch");
static_assert(VGA_DATA_PINS[11] == 4 && VGA_DATA_PINS[15] == 8,
              "Red LSB/MSB pin mismatch");
static esp_lcd_panel_handle_t panel = nullptr;
static uint16_t *framebuffers[2] = {nullptr, nullptr};
static uint16_t *displayedFramebuffer = nullptr;
static TaskHandle_t displayTask = nullptr;
static bool scanoutStarted = false;

// Conservative VGA timing while investigating horizontal DMA displacement.
static constexpr int SCREEN_W = 640;
static constexpr int SCREEN_H = 480;
// A larger bounce chunk gives the S3 more time between DMA refill interrupts,
// reducing sensitivity to Wi-Fi/PSRAM contention without changing VGA timing.
static constexpr int BOUNCE_LINES = 40;
static lv_disp_draw_buf_t drawBuffer;
static lv_disp_drv_t displayDriver;
static lv_disp_t *display = nullptr;

static_assert(LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0,
              "The VGA adapter requires native-endian LVGL RGB565 pixels.");

// No byte swap or color-field conversion: the GPIO matrix matches RGB565.
static constexpr uint16_t toDacPixel(uint16_t rgb565) { return rgb565; }

static bool IRAM_ATTR onFrameBufferComplete(esp_lcd_panel_handle_t,
                                             const esp_lcd_rgb_panel_event_data_t *, void *) {
    BaseType_t taskWoken = pdFALSE;
    if (displayTask)
        vTaskNotifyGiveFromISR(displayTask, &taskWoken);
    return taskWoken == pdTRUE;
}

static void waitForFrameBufferComplete(lv_disp_drv_t *drv) {
    // The RGB driver's bounce-buffer callback signals only after it has
    // finished reading the current framebuffer. LVGL must not reuse that
    // framebuffer before this point.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    lv_disp_flush_ready(drv);
}

[[noreturn]] static void freezeDashboardAfterVgaError() {
    // Keep LVGL from swapping/reusing either framebuffer after a failed page
    // flip. The panel continues scanning its current stable framebuffer.
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}

static void flushDisplay(lv_disp_drv_t *drv, const lv_area_t *area,
                         lv_color_t *colorMap) {
    // In direct mode LVGL has already drawn into one of the two full-size
    // framebuffers. For split refreshes, only the last flush presents it.
    if (!lv_disp_flush_is_last(drv)) {
        lv_disp_flush_ready(drv);
        return;
    }

    const esp_err_t err = esp_lcd_panel_draw_bitmap(
        panel, 0, 0, SCREEN_W, SCREEN_H, colorMap);
    if (err != ESP_OK) {
        Serial.printf("VGA framebuffer flip failed: %s\n", esp_err_to_name(err));
        freezeDashboardAfterVgaError();
    }
    displayedFramebuffer = reinterpret_cast<uint16_t *>(colorMap);

    if (scanoutStarted) {
        // Discard a completion notification for the previous frame if it
        // raced with the page flip. The next notification means DMA has
        // finished the framebuffer that LVGL is about to reuse.
        ulTaskNotifyTake(pdTRUE, 0);
        return; // waitForFrameBufferComplete() signals flush-ready
    }

    // Initial drawing happens before scanout starts, so the buffer is safe
    // to release synchronously.
    lv_disp_flush_ready(drv);
}

static bool checkFramebufferPixel(const char *name, int x, int y, uint32_t rgb888) {
    const uint16_t expected = toDacPixel(lv_color_hex(rgb888).full);
    const uint16_t actual = displayedFramebuffer[y * SCREEN_W + x];
    Serial.printf("Framebuffer %s (%d,%d): expected 0x%04X, stored 0x%04X: %s\n",
                  name, x, y, expected, actual, actual == expected ? "PASS" : "FAIL");
    return actual == expected;
}

static bool lcdCheck(esp_err_t error, const char *operation) {
    if (error == ESP_OK) return true;
    Serial.printf("VGA %s FAILED: %s (0x%X)\n", operation,
                  esp_err_to_name(error), static_cast<unsigned>(error));
    return false;
}

static bool initializeVgaPanel() {
    esp_lcd_rgb_panel_config_t config = {};
    // Integer clock: 240 MHz / 5 / 2 = 24 MHz. No fractional modulation.
    // Same 800x525 totals yield 57.143 Hz; this is not standard 59.94 Hz.
    config.clk_src = LCD_CLK_SRC_PLL240M;
    config.timings.pclk_hz = 24000000;
    config.timings.h_res = SCREEN_W;
    config.timings.v_res = SCREEN_H;
    config.timings.hsync_pulse_width = 96;
    config.timings.hsync_back_porch = 48;
    config.timings.hsync_front_porch = 16;
    config.timings.vsync_pulse_width = 2;
    config.timings.vsync_back_porch = 33;
    config.timings.vsync_front_porch = 10;
    // Standard 640x480 timing: negative sync pulses, idle high.
    config.timings.flags.hsync_idle_low = false;
    config.timings.flags.vsync_idle_low = false;
    config.data_width = 16;
    config.bits_per_pixel = 16;
    // Keep the known working framebuffer configuration and internal-RAM bounce path.
    config.num_fbs = 2;
    config.bounce_buffer_size_px = SCREEN_W * BOUNCE_LINES;
    config.dma_burst_size = 64;
    config.hsync_gpio_num = 1;
    config.vsync_gpio_num = 2;
    config.de_gpio_num = -1;
    config.pclk_gpio_num = -1;
    config.disp_gpio_num = -1;
    for (int bit = 0; bit < 16; ++bit) config.data_gpio_nums[bit] = VGA_DATA_PINS[bit];
    config.flags.fb_in_psram = true;
    if (!lcdCheck(esp_lcd_new_rgb_panel(&config, &panel), "allocation")) return false;
    void *first = nullptr;
    void *second = nullptr;
    if (!lcdCheck(esp_lcd_rgb_panel_get_frame_buffer(panel, 2, &first, &second), "get framebuffers")) return false;
    framebuffers[0] = static_cast<uint16_t *>(first);
    framebuffers[1] = static_cast<uint16_t *>(second);
    memset(framebuffers[0], 0, SCREEN_W * SCREEN_H * sizeof(uint16_t));
    memset(framebuffers[1], 0, SCREEN_W * SCREEN_H * sizeof(uint16_t));
    esp_lcd_rgb_panel_event_callbacks_t callbacks = {};
    callbacks.on_frame_buf_complete = onFrameBufferComplete;
    if (!lcdCheck(esp_lcd_rgb_panel_register_event_callbacks(panel, &callbacks, nullptr),
                  "register frame callback")) return false;
    Serial.println("Espressif RGB LCD driver allocated; scanout held until dashboard is rendered.");
    Serial.printf("Two PSRAM framebuffers: %u bytes each; two internal DMA bounce buffers: %u bytes (%d lines each)\n",
                  SCREEN_W * SCREEN_H * sizeof(uint16_t),
                  2U * SCREEN_W * BOUNCE_LINES * sizeof(uint16_t), BOUNCE_LINES);
    return true;
}

void setup() {
    Serial.begin(115200);
    const uint32_t serialWaitStarted = millis();
    while (!Serial && millis() - serialWaitStarted < 2000) delay(10);
    Serial.println();
    Serial.println("ESP32-S3 Retro VGA Dashboard | HA + Dahua camera | VSYNC-DBUF-640-v2");
    Serial.printf("Chip: %s rev %u | Flash: %u MB\n", ESP.getChipModel(),
                  ESP.getChipRevision(), ESP.getFlashChipSize() / (1024U * 1024U));
    Serial.printf("PSRAM: %s (%u bytes, free %u bytes)\n",
                  ESP.getPsramSize() ? "detected" : "NOT detected",
                  ESP.getPsramSize(), ESP.getFreePsram());
    if (!ESP.getPsramSize()) {
        Serial.println("VGA initialization aborted: PSRAM is required.");
        return;
    }

    const uint32_t fbBytes = SCREEN_W * SCREEN_H * sizeof(uint16_t);
    Serial.printf("Initializing VGA %dx%d RGB565; double framebuffers %u bytes each in PSRAM\n",
                  SCREEN_W, SCREEN_H, static_cast<unsigned>(fbBytes));
    Serial.println("VGA integer clock target: 24.000 MHz; H=30.000 kHz, V=57.143 Hz.");
    Serial.println("Timing: H=96/48/640/16 total 800; V=2/33/480/10 total 525; negative H/V sync.");
    Serial.printf("RGB DMA: %d-line bounce buffers (%u bytes total), data-cache line %d bytes\n",
                  BOUNCE_LINES,
                  2U * SCREEN_W * BOUNCE_LINES * sizeof(uint16_t),
                  CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE);
#if CONFIG_SPIRAM_XIP_FROM_PSRAM
    Serial.println("PSRAM XIP: enabled");
#else
    Serial.println("PSRAM XIP: disabled");
#endif
#if CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP
    Serial.println("Wi-Fi/LWIP PSRAM allocations: enabled");
#else
    Serial.println("Wi-Fi/LWIP PSRAM allocations: disabled (internal RAM)");
#endif
    Serial.println("RGB pins: R=4,5,6,7,8 G=9,10,11,12,13,14 B=15,16,17,18,21; HSYNC=1 VSYNC=2");
    if (!initializeVgaPanel()) return;

    Serial.printf("VGA buffers allocated; PSRAM free: %u bytes\n", ESP.getFreePsram());
    lv_init();
    lv_disp_draw_buf_init(&drawBuffer,
                          reinterpret_cast<lv_color_t *>(framebuffers[0]),
                          reinterpret_cast<lv_color_t *>(framebuffers[1]),
                          SCREEN_W * SCREEN_H);
    lv_disp_drv_init(&displayDriver);
    displayDriver.hor_res = SCREEN_W;
    displayDriver.ver_res = SCREEN_H;
    displayDriver.flush_cb = flushDisplay;
    displayDriver.draw_buf = &drawBuffer;
    displayDriver.direct_mode = 1;
    displayDriver.wait_cb = waitForFrameBufferComplete;
    display = lv_disp_drv_register(&displayDriver);
    if (!display) {
        Serial.println("LVGL display registration failed.");
        return;
    }
    cameraViewBegin();
    const bool liveConfigured = dashboardConfigReady();
    DashboardData initial;
    initial.configMissing = !liveConfigured;
    dashboardBuildUi();
    dashboardUiSetCameraStatus(cameraViewStatus());
    dashboardUpdateUi(initial);
    lv_obj_update_layout(lv_scr_act());
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(display);
    const bool backgroundOk = checkFramebufferPixel("black background", 0, 0, 0x000000);
    const bool cardOk = checkFramebufferPixel("card background", 190, 150, 0x101719);
    if (!backgroundOk || !cardOk) {
        Serial.println("VGA start aborted: rendered pixels differ from the intended palette.");
        return;
    }
    // Start scanout only after LVGL has rendered the complete initial screen
    // into one of the PSRAM framebuffers.
    displayTask = xTaskGetCurrentTaskHandle();
    if (!lcdCheck(esp_lcd_panel_reset(panel), "reset")) return;
    if (!lcdCheck(esp_lcd_panel_init(panel), "start scanout")) return;
    scanoutStarted = true;
    const unsigned divN = LCD_CAM.lcd_clock.lcd_clkm_div_num;
    const unsigned divA = LCD_CAM.lcd_clock.lcd_clkm_div_a;
    const unsigned divB = LCD_CAM.lcd_clock.lcd_clkm_div_b;
    const unsigned pixelDiv = LCD_CAM.lcd_clock.lcd_clk_equ_sysclk ? 1U :
                              LCD_CAM.lcd_clock.lcd_clkcnt_n + 1U;
    Serial.printf("LCD clock readback: source=%u N=%u A=%u B=%u pixel_div=%u; integer divider %s\n",
                  static_cast<unsigned>(LCD_CAM.lcd_clock.lcd_clk_sel),
                  divN, divA, divB, pixelDiv,
                  divN == 5 && divB == 0 && pixelDiv == 2 ? "PASS" : "FAIL");
    Serial.println("Dashboard rendered; LVGL direct-mode double buffering with RGB frame-complete synchronization. Fonts: Montserrat 12/14/16/24/30.");
    Serial.println(liveConfigured ? "HA mode configured; starting poll task." :
                                  "HA configuration missing; displaying unavailable values. Fill dashboard_local.h.");
    if (liveConfigured && !dashboardStartPolling())
        Serial.println("HA polling task creation FAILED; displaying unavailable placeholders.");
    Serial.printf("Available PSRAM: %u bytes; internal heap: %u bytes\n",
                  ESP.getFreePsram(), ESP.getFreeHeap());
    Serial.println("VGA output initialized; physical image still requires monitor verification.");
}

void loop() {
    static uint32_t lastTick = millis();
    static uint32_t lastUi = 0;
    static char lastCameraStatus[32] = {};
    static bool cameraImageDirty = false;
    static DashboardData visible = [] {
        DashboardData d;
        d.configMissing = !dashboardConfigReady();
        return d;
    }();
    const uint32_t now = millis();
    lv_tick_inc(now - lastTick);
    lastTick = now;

    if (cameraViewRefreshIfDue()) cameraImageDirty = true;
    const bool cameraStatusChanged = strncmp(lastCameraStatus, cameraViewStatus(),
                                               sizeof(lastCameraStatus)) != 0;
    if (cameraStatusChanged)
        strlcpy(lastCameraStatus, cameraViewStatus(), sizeof(lastCameraStatus));

    DashboardData incoming;
    const bool newData = dashboardTakeSnapshot(incoming);
    if (newData) visible = incoming;
    if ((newData || cameraImageDirty || cameraStatusChanged || now - lastUi >= 1000) &&
        dashboardTryBeginRender()) {
        lastUi = now;
        bool changed = dashboardUpdateUi(visible);
        if (cameraImageDirty) {
            dashboardUiSetCameraFrame(cameraViewPixels(), cameraViewWidth(), cameraViewHeight());
            cameraImageDirty = false;
            changed = true;
        }
        if (cameraStatusChanged) {
            dashboardUiSetCameraStatus(cameraViewStatus());
            changed = true;
        }
        if (changed) lv_refr_now(display);
        dashboardEndRender();
    }
    delay(20);
}

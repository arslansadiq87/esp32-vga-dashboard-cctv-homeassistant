#include "camera_view.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <JPEGDEC.h>
#include <MD5Builder.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>

#include "camera_local.h"

#ifndef DVR_HTTP_PORT
#define DVR_HTTP_PORT 80
#endif
#ifndef DVR_CHANNEL
#define DVR_CHANNEL 1
#endif
#ifndef DVR_SNAPSHOT_INTERVAL_MS
#define DVR_SNAPSHOT_INTERVAL_MS 1200
#endif

namespace {
constexpr size_t kMaxJpegBytes = 512 * 1024;
constexpr uint16_t kImageMaxWidth = 200;
constexpr uint16_t kImageMaxHeight = 136;
constexpr uint32_t kHttpTimeoutMs = 6000;
constexpr uint32_t kFailureRetryMs = 3000;

WiFiClient client;
JPEGDEC jpeg;
uint8_t *jpegBuffer = nullptr;
uint16_t *rgb565Buffer = nullptr;
uint16_t jpegDecodedWidth = 0;
uint16_t jpegDecodedHeight = 0;
uint16_t targetWidth = 0;
uint16_t targetHeight = 0;
uint16_t targetLeft = 0;
uint16_t targetTop = 0;
uint32_t lastAttemptMs = 0;
uint32_t retryDelayMs = DVR_SNAPSHOT_INTERVAL_MS;
size_t jpegBytes = 0;
bool ready = false;
char statusText[24] = "WAIT WIFI";

String md5Hex(const String &value) {
    MD5Builder md5;
    md5.begin();
    md5.add(reinterpret_cast<const uint8_t *>(value.c_str()), value.length());
    md5.calculate();
    return md5.toString();
}

String headerValue(const String &headers, const char *name) {
    String wanted(name);
    wanted.toLowerCase();
    int pos = 0;
    while (pos < headers.length()) {
        int end = headers.indexOf("\r\n", pos);
        if (end < 0) end = headers.length();
        const String line = headers.substring(pos, end);
        String lower = line;
        lower.toLowerCase();
        if (lower.startsWith(wanted + ":")) {
            const int colon = line.indexOf(':');
            String value = line.substring(colon + 1);
            value.trim();
            return value;
        }
        if (end == headers.length()) break;
        pos = end + 2;
    }
    return String();
}

String digestField(const String &challenge, const char *key) {
    const String needle = String(key) + "=\"";
    const int start = challenge.indexOf(needle);
    if (start < 0) return String();
    const int valueStart = start + needle.length();
    const int end = challenge.indexOf('"', valueStart);
    return end < 0 ? String() : challenge.substring(valueStart, end);
}

String makeDigest(const String &challenge, const String &uri) {
    const String realm = digestField(challenge, "realm");
    const String nonce = digestField(challenge, "nonce");
    const String qopOptions = digestField(challenge, "qop");
    const String opaque = digestField(challenge, "opaque");
    if (realm.isEmpty() || nonce.isEmpty()) return String();

    const bool qopAuth = qopOptions.indexOf("auth") >= 0;
    char random[17] = {};
    snprintf(random, sizeof(random), "%08lx%08lx",
             static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(esp_random()));
    const String cnonce(random);
    const String nc = "00000001";
    const String ha1 = md5Hex(String(DVR_USERNAME) + ":" + realm + ":" + DVR_PASSWORD);
    const String ha2 = md5Hex(String("GET:") + uri);
    const String response = qopAuth
        ? md5Hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":auth:" + ha2)
        : md5Hex(ha1 + ":" + nonce + ":" + ha2);
    String auth = String("Digest username=\"") + DVR_USERNAME + "\", realm=\"" + realm +
        "\", nonce=\"" + nonce + "\", uri=\"" + uri + "\", response=\"" + response +
        "\", algorithm=MD5";
    if (qopAuth) auth += String(", qop=auth, nc=") + nc + ", cnonce=\"" + cnonce + "\"";
    if (opaque.length()) auth += String(", opaque=\"") + opaque + "\"";
    return auth;
}

bool readHeaders(String &headers) {
    headers = "";
    const uint32_t started = millis();
    while (millis() - started < kHttpTimeoutMs) {
        while (client.available()) {
            headers += static_cast<char>(client.read());
            if (headers.endsWith("\r\n\r\n")) return true;
            if (headers.length() > 8192) return false;
        }
        if (!client.connected() && !client.available()) return false;
        delay(1);
    }
    return false;
}

int responseStatus(const String &headers) {
    const int space = headers.indexOf(' ');
    return space < 0 ? 0 : headers.substring(space + 1, space + 4).toInt();
}

bool request(const String &uri, const String &auth, String &headers) {
    client.stop();
    client.setTimeout(kHttpTimeoutMs / 1000);
    if (!client.connect(DVR_HOST, DVR_HTTP_PORT, kHttpTimeoutMs)) return false;
    client.printf("GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: VGA-Dashboard/1.0\r\nConnection: close\r\nAccept: image/jpeg,*/*\r\n",
                  uri.c_str(), DVR_HOST);
    if (auth.length()) client.printf("Authorization: %s\r\n", auth.c_str());
    client.print("\r\n");
    return readHeaders(headers);
}

bool fetchSnapshot() {
    jpegBytes = 0;
    const String uri = String("/cgi-bin/snapshot.cgi?channel=") + String(DVR_CHANNEL);
    String headers;
    if (!request(uri, "", headers)) {
        strlcpy(statusText, "DVR NO RESPONSE", sizeof(statusText));
        client.stop();
        return false;
    }

    int code = responseStatus(headers);
    if (code == 401) {
        const String auth = makeDigest(headerValue(headers, "WWW-Authenticate"), uri);
        client.stop();
        if (!auth.length() || !request(uri, auth, headers)) {
            strlcpy(statusText, "DVR AUTH ERROR", sizeof(statusText));
            client.stop();
            return false;
        }
        code = responseStatus(headers);
    }
    if (code != 200) {
        snprintf(statusText, sizeof(statusText), "HTTP ERROR %d", code);
        client.stop();
        return false;
    }

    const int contentLength = headerValue(headers, "Content-Length").toInt();
    if (contentLength > static_cast<int>(kMaxJpegBytes)) {
        strlcpy(statusText, "JPEG TOO LARGE", sizeof(statusText));
        client.stop();
        return false;
    }
    uint32_t lastByteMs = millis();
    while ((client.connected() || client.available()) &&
           (contentLength <= 0 || jpegBytes < static_cast<size_t>(contentLength))) {
        const int available = client.available();
        if (available > 0) {
            const size_t room = kMaxJpegBytes - jpegBytes;
            const size_t amount = std::min(static_cast<size_t>(available), room);
            const int count = client.readBytes(jpegBuffer + jpegBytes, amount);
            if (count <= 0) break;
            jpegBytes += static_cast<size_t>(count);
            lastByteMs = millis();
            if (jpegBytes == kMaxJpegBytes) break;
        } else {
            if (millis() - lastByteMs > kHttpTimeoutMs) break;
            delay(1);
        }
    }
    client.stop();
    if ((contentLength > 0 && jpegBytes != static_cast<size_t>(contentLength)) ||
        jpegBytes < 4 || jpegBuffer[0] != 0xFF || jpegBuffer[1] != 0xD8) {
        strlcpy(statusText, "BAD JPEG FRAME", sizeof(statusText));
        return false;
    }
    return true;
}

int onJpegBlock(JPEGDRAW *draw) {
    if (!draw || !draw->pPixels || !rgb565Buffer) return 0;
    for (int y = 0; y < draw->iHeight; ++y) {
        const uint16_t sy = static_cast<uint16_t>(draw->y + y);
        if (sy >= jpegDecodedHeight) continue;
        const uint16_t ty = static_cast<uint16_t>(targetTop +
            (static_cast<uint32_t>(sy) * targetHeight) / jpegDecodedHeight);
        const uint16_t *source = draw->pPixels + static_cast<size_t>(y) * draw->iWidth;
        for (int x = 0; x < draw->iWidthUsed; ++x) {
            const uint16_t sx = static_cast<uint16_t>(draw->x + x);
            if (sx >= jpegDecodedWidth) continue;
            const uint16_t tx = static_cast<uint16_t>(targetLeft+
                (static_cast<uint32_t>(sx) * targetWidth) / jpegDecodedWidth);
            rgb565Buffer[static_cast<size_t>(ty) * kImageMaxWidth + tx] = source[x];
        }
    }
    return 1;
}

int chooseScale(int width, int height, uint16_t &outW, uint16_t &outH) {
    const int divisors[] = {1, 2, 4, 8};
    const int modes[] = {0, JPEG_SCALE_HALF, JPEG_SCALE_QUARTER, JPEG_SCALE_EIGHTH};
    for (int i = 0; i < 4; ++i) {
        const int w = std::max(1, width / divisors[i]);
        const int h = std::max(1, height / divisors[i]);
        if (w <= 352 && h <= 288) {
            outW = static_cast<uint16_t>(w);
            outH = static_cast<uint16_t>(h);
            return modes[i];
        }
    }
    outW = static_cast<uint16_t>(std::max(1, width / 8));
    outH = static_cast<uint16_t>(std::max(1, height / 8));
    return JPEG_SCALE_EIGHTH;
}

bool decodeSnapshot() {
    if (!jpeg.openRAM(jpegBuffer, static_cast<int>(jpegBytes), onJpegBlock)) {
        strlcpy(statusText, "JPEG DECODE ERROR", sizeof(statusText));
        return false;
    }
    const int scale = chooseScale(jpeg.getWidth(), jpeg.getHeight(),
                                  jpegDecodedWidth, jpegDecodedHeight);
    const float fitX = static_cast<float>(kImageMaxWidth) / jpegDecodedWidth;
    const float fitY = static_cast<float>(kImageMaxHeight) / jpegDecodedHeight;
    const float fit = std::min(fitX, fitY);
    targetWidth = std::max<uint16_t>(1, static_cast<uint16_t>(jpegDecodedWidth * fit));
    targetHeight = std::max<uint16_t>(1, static_cast<uint16_t>(jpegDecodedHeight * fit));
    targetLeft = (kImageMaxWidth - targetWidth) / 2;
    targetTop = (kImageMaxHeight - targetHeight) / 2;
    memset(rgb565Buffer, 0, kImageMaxWidth * kImageMaxHeight * sizeof(uint16_t));
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    const bool ok = jpeg.decode(0, 0, scale) == 1;
    jpeg.close();
    if (!ok) {
        strlcpy(statusText, "JPEG DECODE ERROR", sizeof(statusText));
        return false;
    }
    ready = true;
    strlcpy(statusText, "CAMERA ONLINE", sizeof(statusText));
    return true;
}
} // namespace

bool cameraViewBegin() {
    jpegBuffer = static_cast<uint8_t *>(heap_caps_malloc(kMaxJpegBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    rgb565Buffer = static_cast<uint16_t *>(heap_caps_malloc(
        kImageMaxWidth * kImageMaxHeight * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!jpegBuffer || !rgb565Buffer) {
        strlcpy(statusText, "CAMERA NO MEMORY", sizeof(statusText));
        Serial.println("Camera buffers could not be allocated in PSRAM.");
        return false;
    }
    memset(rgb565Buffer, 0, kImageMaxWidth * kImageMaxHeight * sizeof(uint16_t));
    Serial.printf("Camera image staging: %u bytes RGB565; JPEG buffer: %u bytes; PSRAM free %u\n",
                  static_cast<unsigned>(kImageMaxWidth * kImageMaxHeight * 2),
                  static_cast<unsigned>(kMaxJpegBytes), ESP.getFreePsram());
    return true;
}

bool cameraViewRefreshIfDue() {
    const uint32_t now = millis();
    if (now - lastAttemptMs < retryDelayMs) return false;
    lastAttemptMs = now;
    if (!DVR_HOST[0] || !DVR_USERNAME[0]) {
        strlcpy(statusText, "CAMERA NOT SET", sizeof(statusText));
        retryDelayMs = 10000;
        return false;
    }
    if (WiFi.status() != WL_CONNECTED) {
        strlcpy(statusText, "WAIT WIFI", sizeof(statusText));
        retryDelayMs = 1000;
        return false;
    }
    const uint32_t started = millis();
    const bool ok = fetchSnapshot() && decodeSnapshot();
    retryDelayMs = ok ? DVR_SNAPSHOT_INTERVAL_MS : kFailureRetryMs;
    if (ok) {
        Serial.printf("Camera snapshot: %u bytes, %ux%u RGB565, %lu ms\n",
                      static_cast<unsigned>(jpegBytes), targetWidth, targetHeight,
                      static_cast<unsigned long>(millis() - started));
    } else {
        Serial.printf("Camera snapshot unavailable: %s\n", statusText);
    }
    return ok;
}

const uint16_t *cameraViewPixels() { return rgb565Buffer; }
uint16_t cameraViewWidth() { return kImageMaxWidth; }
uint16_t cameraViewHeight() { return kImageMaxHeight; }
const char *cameraViewStatus() { return statusText; }

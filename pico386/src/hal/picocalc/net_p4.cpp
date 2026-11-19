// net_p4.cpp -- the P4's Wi-Fi for the PC's network (pc_net.cpp): Arduino's WiFi, which reaches
// the board's ESP32-C6 through ESP-Hosted over SDIO. lwIP's sockets then work as anywhere.
//
// With no network named in the settings it joins the one the loader was set up with.
// PicoMultiLoader keeps it on the card, in /picomultiloader.cfg (wifi_ssid=, wifi_pass=); the older
// esp32loader kept it in NVS, namespace "loader" (same flash, same nvs partition).
// Started from app_setup when the network is on, before any PC: see radioOn() for the RAM.
#include "../../../board.h"

#if defined(BOARD_PICOCALC_P4)

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <stdio.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <port/esp/freertos/include/port_esp_hosted_host_config.h>   // before the table: it sets which slots it has
#include <esp_hosted_os_abstraction.h>
#include <esp_private/esp_cache_private.h>
#include <esp_memory_utils.h>
#include <esp_rom_sys.h>
#include "../hal.h"

static bool s_started, s_radio, s_noRoom;
static String s_ssid, s_pass;
static uint32_t s_beganAt;
static int s_retries;

bool hal_net_available() { return true; }

// The first WiFi.mode() starts ESP-Hosted, whose SDIO transport takes two blocks of DMA RAM
// for its buffers (40 KB and 47 KB) and asserts when it can't have them: the device rebooted. On
// the menu there are about 47 KB of internal RAM, in pieces of 8 KB at most. So:
//  * those buffers go to PSRAM, which the P4's DMA reaches (the newer esp-hosted does the same with
//    CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM; Arduino's 2.12.8 has no such option, but its
//    allocator is a public, writable table);
//  * the panel's 64 KB of bounce buffers move to PSRAM too, since the rest of ESP-Hosted (tasks,
//    the SDMMC driver's buffers, lwIP) still needs about 56 KB of internal RAM;
//  * and with less than kNeedInternal left it refuses instead of asserting.
// A PC started after that gets less internal RAM for its tables (hal_alloc_fast keeps 48 KB free).
bool p4DisplayFreeInternal();            // display_p4.cpp

// The SDMMC driver refuses a block transfer (ESP_ERR_INVALID_ARG) unless the buffer's address and
// length are multiples of the cache line: 128 bytes in PSRAM (the L2's), 64 in internal RAM.
// ESP-Hosted aligns to 64 and pads every transfer to whole 512-byte blocks, and it sends from
// buffers it did not allocate (its 1600-byte pool blocks, lwIP's frames), so a write could come
// from anywhere. Opening a web page failed to send ("Failed to send data: 258 66 66") and the
// driver then restarted the device ("Unrecoverable host sdio state"). So:
//  * what it allocates for DMA in PSRAM is aligned to, and sized in, whole PSRAM lines;
//  * a block write from a misaligned buffer is copied to an aligned one first (sdioWrite), and one
//    the driver still refuses is tried once more from that copy.
// The hooks are slots in g_hosted_osi_funcs, whose layout depends on the port's config header:
// without it the slots past the GPIO ones are 3 off, and "write_block" was really "read_reg".
static size_t s_line = 128, s_lineInt = 64;   // PSRAM's and internal RAM's cache lines
static inline size_t roundUp(size_t n, size_t a) { return (n + a - 1) / a * a; }
static void *dmaAlloc(size_t size, size_t align) {
  void *p = heap_caps_aligned_alloc(align > s_line ? align : s_line, roundUp(size, s_line),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_aligned_alloc(align, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}

extern "C" int hosted_sdio_write_block(void *ctx, uint32_t reg, uint8_t *data, uint16_t size, bool lock_required);
static int (*s_write)(void *, uint32_t, uint8_t *, uint16_t, bool);
static uint8_t *s_txBounce;                   // only ESP-Hosted's write task sends
static size_t s_txCap;
static uint32_t s_bounced, s_retried;
static inline bool misaligned(const void *p, size_t n) {
  const size_t a = esp_ptr_external_ram(p) ? s_line : s_lineInt;
  return ((uintptr_t)p | n) & (a - 1);
}
// A data write's address is the end of the slave's window less the bytes left, and the length is
// padded to whole blocks: only those bytes are the packet's, and the rest may lie past its buffer.
static const uint32_t kCmd53End = 0x1f800;    // ESP_SLAVE_CMD53_END_ADDR
static int bouncedWrite(void *ctx, uint32_t reg, uint8_t *data, uint16_t size, bool lock) {
  if (size > s_txCap) {
    heap_caps_free(s_txBounce);
    s_txCap = roundUp(size, 512);
    s_txBounce = (uint8_t *)heap_caps_aligned_alloc(s_line, s_txCap, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_txBounce) s_txBounce = (uint8_t *)heap_caps_aligned_alloc(s_line, s_txCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_txBounce) {
      s_txCap = 0;
      return -1;
    }
  }
  const size_t n = reg < kCmd53End && kCmd53End - reg < size ? kCmd53End - reg : size;
  memcpy(s_txBounce, data, n);
  memset(s_txBounce + n, 0, size - n);
  return s_write(ctx, reg, s_txBounce, size, lock);
}
static int sdioWrite(void *ctx, uint32_t reg, uint8_t *data, uint16_t size, bool lock) {
  if (size < 512) return s_write(ctx, reg, data, size, lock);  // byte mode: the driver copies those itself
  if (misaligned(data, size)) {
    s_bounced++;
    const int r = bouncedWrite(ctx, reg, data, size, lock);
    if (r != -1) return r;
  }
  int r = s_write(ctx, reg, data, size, lock);
  if (r) {
    s_retried++;
    const int r2 = bouncedWrite(ctx, reg, data, size, lock);
    esp_rom_printf("wifi: SDIO write %p+%u refused (%d), copied: %d\n", data, (unsigned)size, r, r2);
    if (r2 != -1) r = r2;
  }
  return r;
}
static void hookHosted() {
  size_t line = 0;
  if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &line) == ESP_OK && line) s_line = line;
  line = 0;
  if (esp_cache_get_alignment(0, &line) == ESP_OK) s_lineInt = line ? line : 4;
  g_hosted_osi_funcs._h_malloc_align = dmaAlloc;
  if (s_write) return;
  if (g_hosted_osi_funcs._h_sdio_write_block != hosted_sdio_write_block) {
    hal_log("wifi: ESP-Hosted's table is not as expected; SDIO writes not hooked");
    return;
  }
  s_write = g_hosted_osi_funcs._h_sdio_write_block;
  g_hosted_osi_funcs._h_sdio_write_block = sdioWrite;
}
static const size_t kNeedInternal = 72 * 1024;
static bool radioOn() {
  if (s_radio) return true;
  if (!p4DisplayFreeInternal()) hal_log("wifi: the panel's buffers stay in internal RAM");
  hookHosted();
  const size_t freeI = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (freeI < kNeedInternal) {
    hal_log("wifi: only %u KB of internal RAM, not starting the radio", (unsigned)(freeI >> 10));
    s_noRoom = true;
    return false;
  }
  if (!WiFi.mode(WIFI_STA)) return false;
  s_radio = true;
  s_noRoom = false;
  hal_log("wifi: radio on, internal RAM %u KB free (was %u KB)",
          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10), (unsigned)(freeI >> 10));
  return true;
}

// The loader's network: PicoMultiLoader's card config, else esp32loader's NVS.
static void loaderNetwork(String &id, String &pw) {
  char path[96];
  snprintf(path, sizeof path, "%s/picomultiloader.cfg", hal_sd_root());
  if (FILE *f = fopen(path, "r")) {
    char line[200];
    while (fgets(line, sizeof line, f)) {
      line[strcspn(line, "\r\n")] = 0;
      if (!strncmp(line, "wifi_ssid=", 10)) id = line + 10;
      else if (!strncmp(line, "wifi_pass=", 10)) pw = line + 10;
    }
    fclose(f);
    if (!id.isEmpty()) return;
  }
  Preferences p;
  if (p.begin("loader", true)) {
    id = p.getString("wifi_ssid", "");
    pw = p.getString("wifi_pass", "");
    p.end();
  }
}

void hal_net_begin(const char *ssid, const char *pass) {
  String id = ssid ? ssid : "", pw = pass ? pass : "";
  if (id.isEmpty()) loaderNetwork(id, pw);
  if (s_started && id == s_ssid && pw == s_pass) return;
  s_ssid = id;
  s_pass = pw;
  if (id.isEmpty()) {
    hal_log("wifi: no network set (settings or the loader's)");
    hal_net_end();
    return;
  }
  if (!radioOn()) return;
  static bool logged;
  if (!logged) {
    logged = true;
    WiFi.onEvent([](arduino_event_id_t e, arduino_event_info_t info) {
      if (e == ARDUINO_EVENT_WIFI_STA_GOT_IP) hal_log("wifi: up, %s", WiFi.localIP().toString().c_str());
      else if (e == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
        hal_log("wifi: disconnected, reason %d", info.wifi_sta_disconnected.reason);
    });
  }
  WiFi.setAutoReconnect(true);
  WiFi.disconnect();
  WiFi.begin(id.c_str(), pw.c_str());
  s_started = true;
  s_beganAt = millis();
  s_retries = 0;
  hal_log("wifi: joining %s", id.c_str());
}

void hal_net_end() {
  if (!s_started) return;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  s_started = s_radio = false;
}

int hal_net_state(char *info, size_t n) {
  if (!s_started) {
    if (n) snprintf(info, n, s_noRoom ? "no internal RAM for Wi-Fi" : s_ssid.isEmpty() ? "no network set" : "off");
    return HAL_NET_OFF;
  }
  const wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {
    if (n) snprintf(info, n, "%s, %s", s_ssid.c_str(), WiFi.localIP().toString().c_str());
    return HAL_NET_UP;
  }
  // One cold start in two, the join sat there with no event at all (not even a failure, which
  // the auto-reconnect would have retried). The menu asks every second, so it starts it again.
  if (millis() - s_beganAt > 15000) {
    hal_log("wifi: no answer from %s, trying again", s_ssid.c_str());
    WiFi.disconnect();
    WiFi.begin(s_ssid.c_str(), s_pass.c_str());
    s_beganAt = millis();
    s_retries++;
  }
  if (st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED || s_retries) {
    if (n) snprintf(info, n, "%s: %s", s_ssid.c_str(),
                    st == WL_NO_SSID_AVAIL ? "not found" : st == WL_CONNECT_FAILED ? "refused" : "no answer, trying");
    return HAL_NET_FAILED;
  }
  if (n) snprintf(info, n, "joining %s", s_ssid.c_str());
  return HAL_NET_CONNECTING;
}

// -1: the radio could not start (not enough internal RAM).
int hal_net_scan(char (*names)[33], int max) {
  if (!radioOn()) return -1;
  const int found = WiFi.scanNetworks();
  int n = 0;
  for (int i = 0; i < found && n < max; i++) {          // the driver sorts them strongest first
    String id = WiFi.SSID(i);
    if (id.isEmpty()) continue;
    bool dup = false;
    for (int k = 0; k < n && !dup; k++) dup = id == names[k];
    if (dup) continue;
    strncpy(names[n], id.c_str(), 32);
    names[n][32] = 0;
    n++;
  }
  WiFi.scanDelete();
  return n;
}

#endif // BOARD_PICOCALC_P4

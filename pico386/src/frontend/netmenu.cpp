// netmenu.cpp -- Settings > Network: the PC's network card, the Wi-Fi it goes out on, and the
// web proxy its HTTP goes through (pc_net.cpp). Protoweb (https://protoweb.org) serves the web
// of the 1990s to period browsers, at a simulated line speed picked by the port.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "netmenu.h"
#include "ui.h"
#include "settings.h"
#include "../hal/hal.h"
#include "../pc/pc_net.h"

#if defined(PICO386_LOWMEM)
// The RP2350 has no radio, and no SRAM to spare for this menu.
void netApply() {}
void netMenu() {}
#else

namespace {

const char *const kProxyNames[NET_PROXY_COUNT] = { "Off", "Protoweb", "Protoweb 2", "Custom" };

// wayback.protoweb.org's ports: the full speed (a T1), then the simulated modems and lines.
struct Speed { const char *name; int port; };
const Speed kSpeeds[] = {
  { "Full (T1)", 7851 }, { "9600", 7809 }, { "14.4k", 7814 }, { "19.2k", 7819 }, { "28.8k", 7828 },
  { "33.6k", 7833 }, { "56k", 7856 }, { "ISDN 64k", 7864 }, { "ISDN 128k", 7128 }, { "DSL 256k", 7256 },
};
constexpr int kNumSpeeds = sizeof(kSpeeds) / sizeof(kSpeeds[0]);

void proxyOf(std::string &host, int &port) {
  host.clear();
  port = 0;
  switch (g_settings.proxy) {
    case NET_PROXY_PROTOWEB:
      host = "wayback.protoweb.org";
      port = kSpeeds[g_settings.proxySpeed < kNumSpeeds ? g_settings.proxySpeed : 0].port;
      break;
    case NET_PROXY_PROTOWEB2:
      host = "wayback2.protoweb.org";
      port = 7851;
      break;
    case NET_PROXY_CUSTOM: {
      const std::string &c = g_settings.proxyCustom;
      const size_t colon = c.find_last_of(':');
      host = c.substr(0, colon);
      port = colon == std::string::npos ? 8080 : atoi(c.c_str() + colon + 1);
      break;
    }
  }
}

// Picks the Wi-Fi network: a scan, esp32loader's network, or a typed name; then its password.
void chooseWifi() {
  ui::drawBackground();
  ui::message("Wi-Fi", "Looking for networks...");
  static char names[16][33];
  int found = hal_net_scan(names, 16);
  if (found < 0) {
    ui::message("Wi-Fi", "Not enough internal RAM to start the radio", 2500);
    found = 0;
  }
  ui::Item items[18];
  int n = 0;
  items[n++] = { "Loader's network", nullptr, ui::ICON_GEAR, "The one esp32loader joins" };
  for (int i = 0; i < found; i++) items[n++] = { names[i], nullptr, ui::ICON_CHART };
  items[n++] = { "Type a name", nullptr, ui::ICON_PAD };
  ui::Screen scr;
  scr.title = "Wi-Fi network";
  scr.subtitle = found ? nullptr : "No networks found";
  scr.hints = "Enter|Choose;Esc|Back";
  int sel = 0;
  const uint8_t k = ui::list(scr, items, n, &sel);
  if (k != KEY_ENTER) return;
  if (sel == 0) {
    g_settings.wifiSsid.clear();
    g_settings.wifiPass.clear();
    return;
  }
  char buf[65];
  if (sel == n - 1) {
    snprintf(buf, sizeof buf, "%s", g_settings.wifiSsid.c_str());
    if (!ui::editText("Wi-Fi network", "Network name (SSID)", buf, 33)) return;
    g_settings.wifiSsid = buf;
  } else {
    g_settings.wifiSsid = names[sel - 1];
  }
  buf[0] = 0;
  // Esc: an open network
  g_settings.wifiPass = ui::editText("Wi-Fi password", "Password (Esc: none)", buf, sizeof buf) ? buf : "";
}

void editCustomProxy() {
  char buf[128];
  snprintf(buf, sizeof buf, "%s", g_settings.proxyCustom.c_str());
  if (ui::editText("Proxy", "host:port", buf, sizeof buf)) g_settings.proxyCustom = buf;
}

} // namespace

void netApply() {
  if (!hal_net_available()) return;
  std::string host;
  int port;
  proxyOf(host, port);
  pcnet_configure(g_settings.netOn, host.c_str(), port);
  if (g_settings.netOn) hal_net_begin(g_settings.wifiSsid.c_str(), g_settings.wifiPass.c_str());
  else hal_net_end();
}

void netMenu() {
  enum { R_CARD, R_WIFI, R_PASS, R_STATUS, R_PROXY, R_SPEED, R_CUSTOM, R_TRAFFIC };
  int sel = 0;
  ui::Screen scr;
  scr.title = "Network";
  scr.subtitle = "NE2000 at I/O 300h, IRQ 9; the guest gets its address by DHCP";
  scr.hints = "<>|Change;Enter|Edit;Esc|Back";
  scr.refreshMs = 1000;                 // Status and Traffic change on their own
  for (;;) {
    ui::Item items[8];
    int row[8];
    int n = 0;
    char state[64], traffic[64], custom[64];
    row[n] = R_CARD;
    items[n++] = { "Network card", g_settings.netOn ? "On" : "Off", ui::ICON_GEAR,
                   "Fitted when a machine starts", -1, true };
    row[n] = R_WIFI;
    items[n++] = { "Wi-Fi", g_settings.wifiSsid.empty() ? "Loader's" : g_settings.wifiSsid.c_str(), ui::ICON_CHART };
    if (!g_settings.wifiSsid.empty()) {
      row[n] = R_PASS;
      items[n++] = { "Password", g_settings.wifiPass.empty() ? "None" : "********", ui::ICON_PAD };
    }
    hal_net_state(state, sizeof state);
    row[n] = R_STATUS;
    items[n++] = { "Status", nullptr, ui::ICON_PLAY, state };
    row[n] = R_PROXY;
    items[n++] = { "Web proxy", kProxyNames[g_settings.proxy], ui::ICON_FOLDER,
                   g_settings.proxy ? "The guest's port 80 goes through it" : nullptr, -1, true };
    if (g_settings.proxy == NET_PROXY_PROTOWEB) {
      if (g_settings.proxySpeed >= kNumSpeeds) g_settings.proxySpeed = 0;
      row[n] = R_SPEED;
      items[n++] = { "Line speed", kSpeeds[g_settings.proxySpeed].name, ui::ICON_SKIP, nullptr, -1, true };
    }
    if (g_settings.proxy == NET_PROXY_CUSTOM) {
      snprintf(custom, sizeof custom, "%s", g_settings.proxyCustom.empty() ? "(none)" : g_settings.proxyCustom.c_str());
      row[n] = R_CUSTOM;
      items[n++] = { "Proxy address", custom, ui::ICON_PAD };
    }
    pcnet_status(traffic, sizeof traffic);
    row[n] = R_TRAFFIC;
    items[n++] = { "Traffic", traffic, ui::ICON_CHART };
    const uint8_t k = ui::list(scr, items, n, &sel);
    if (k == 0) continue;               // a second passed: draw them again
    if (k == KEY_ESC || k == KEY_BACKSPACE) break;
    const int d = k == KEY_LEFT ? -1 : 1;
    bool changed = true;
    switch (row[sel]) {
      case R_CARD: g_settings.netOn = !g_settings.netOn; break;
      case R_WIFI: if (k == KEY_ENTER) chooseWifi(); else changed = false; break;
      case R_PASS:
        if (k == KEY_ENTER) {
          char buf[65] = "";
          if (ui::editText("Wi-Fi password", "Password", buf, sizeof buf)) g_settings.wifiPass = buf;
        } else changed = false;
        break;
      case R_PROXY: g_settings.proxy = (g_settings.proxy + d + NET_PROXY_COUNT) % NET_PROXY_COUNT; break;
      case R_SPEED: g_settings.proxySpeed = (g_settings.proxySpeed + d + kNumSpeeds) % kNumSpeeds; break;
      case R_CUSTOM: if (k == KEY_ENTER) editCustomProxy(); else changed = false; break;
      default: changed = false; break;   // Status, Traffic: any key shows them again
    }
    if (changed) netApply();
  }
  settingsSave();
}

#endif

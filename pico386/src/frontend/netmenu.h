// netmenu.h -- Settings > Network (netmenu.cpp).
#pragma once

void netMenu();
// Hands the settings to pc_net and starts or stops the Wi-Fi (app start, and every change).
// false: the network is on but the radio had no internal RAM, so machines get no card.
bool netApply();
bool netBlocked();

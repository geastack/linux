#pragma once
/* Sailfish OS on-screen keyboard (Maliit) client; see main/sailfish_keyboard.cpp. */

void sailfish_keyboard_init();
// Runs pending Maliit D-Bus work; call before reading SDL events each frame.
void sailfish_keyboard_pump();
// Shows the keyboard while an input is focused (-1: none) and hides it after.
void sailfish_keyboard_update(int activeInputId, bool hiddenText);
void sailfish_keyboard_shutdown();

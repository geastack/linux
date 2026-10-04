/* targets/sailfish-os/include/sailfish_keyboard.h
 * Sailfish system keyboard (Maliit) bridge.
 *
 * libmaliit-glib is not on Harbour's allowed-library list. This bridge talks
 * to the same server through GIO, which is allowed (libgio-2.0.so.0):
 *   session bus org.maliit.server /org/maliit/server/address
 *     interface org.maliit.Server.Address, property "address"
 *   peer bus /com/meego/inputmethod/uiserver1
 *     interface com.meego.inputmethod.uiserver1
 *   peer bus /com/meego/inputmethod/inputcontext
 *     interface com.meego.inputmethod.inputcontext1
 * See targets/sailfish-os/README.md.
 */
#pragma once

typedef bool (*SailfishKeyboardAppendFn)(const char *utf8);
typedef bool (*SailfishKeyboardActionFn)(void);
typedef void (*SailfishKeyboardAreaFn)(int x, int y, int width, int height);

/* append/backspace/enter are the same helpers the SDL keyboard uses.
 * area is called when Maliit reports the keyboard rectangle (including a
 * zero height once it is hidden). Failure to reach Maliit is logged and
 * ignored so emulator and desktop SDL input still work. */
void sailfish_keyboard_init(SailfishKeyboardAppendFn append_text,
                             SailfishKeyboardActionFn backspace,
                             SailfishKeyboardActionFn enter,
                             SailfishKeyboardActionFn focus_lost,
                             SailfishKeyboardAreaFn area);

void sailfish_keyboard_pump(void);

/* active_input_id < 0 hides the keyboard. input_type is the focused
 * element's type attribute; "password" sets Maliit hiddenText and
 * disables prediction. */
void sailfish_keyboard_update(int active_input_id, const char *input_type);

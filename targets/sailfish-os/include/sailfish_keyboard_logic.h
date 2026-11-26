#pragma once
/* Pure keyboard helpers for the Sailfish OS Maliit client:
 * - Qt -> SDL keycode mapping for non-text special keys
 * - UTF-8 chunk splitting capped at SDL_TextInputEvent::text capacity (31 bytes)
 * Extracted into a header so unit tests can verify edge cases without D-Bus/Maliit. */

#include <SDL2/SDL_keycode.h>

#include <cstddef>
#include <cstring>

namespace gea::sailfish::keyboard {

// Qt key codes the Sailfish keyboard sends for keys that are not text.
inline SDL_Keycode sdlKeyForQt(int key)
{
	switch (key) {
	case 0x01000000: return SDLK_ESCAPE;
	case 0x01000001: return SDLK_TAB;
	case 0x01000003: return SDLK_BACKSPACE;
	case 0x01000004: return SDLK_RETURN;
	case 0x01000005: return SDLK_KP_ENTER;
	case 0x01000007: return SDLK_DELETE;
	case 0x01000010: return SDLK_HOME;
	case 0x01000011: return SDLK_END;
	case 0x01000012: return SDLK_LEFT;
	case 0x01000013: return SDLK_UP;
	case 0x01000014: return SDLK_RIGHT;
	case 0x01000015: return SDLK_DOWN;
	default: return SDLK_UNKNOWN;
	}
}

// Splits utf8 into slices of at most maxBytes without splitting a multi-byte
// UTF-8 sequence across slices. Stops cleanly on malformed leading continuation
// runs.
template <typename Fn>
inline void splitUtf8Chunks(const char *utf8, std::size_t maxBytes, Fn &&emit)
{
	if (!utf8 || maxBytes == 0) return;
	std::size_t length = std::strlen(utf8);
	while (length > 0) {
		std::size_t take = length < maxBytes ? length : maxBytes;
		while (take < length && take > 0 && (static_cast<unsigned char>(utf8[take]) & 0xc0) == 0x80) {
			--take;
		}
		if (take == 0) return;
		emit(utf8, take);
		utf8 += take;
		length -= take;
	}
}

}  // namespace gea::sailfish::keyboard

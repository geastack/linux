// Native unit tests for targets/sailfish-os/include/sailfish_keyboard_logic.h.
// Exercises Qt -> SDL key mapping and UTF-8 31-byte SDL_TEXTINPUT chunking
// across ASCII, Turkish multi-byte characters, 4-byte emoji, and malformed input.

#include "sailfish_keyboard_logic.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using gea::sailfish::keyboard::sdlKeyForQt;
using gea::sailfish::keyboard::splitUtf8Chunks;

namespace {

bool isValidUtf8(const std::string &s)
{
	std::size_t i = 0;
	while (i < s.size()) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		std::size_t len = 0;
		if ((c & 0x80) == 0) len = 1;
		else if ((c & 0xe0) == 0xc0) len = 2;
		else if ((c & 0xf0) == 0xe0) len = 3;
		else if ((c & 0xf8) == 0xf0) len = 4;
		else return false;
		if (i + len > s.size()) return false;
		for (std::size_t k = 1; k < len; ++k) {
			if ((static_cast<unsigned char>(s[i + k]) & 0xc0) != 0x80) return false;
		}
		i += len;
	}
	return true;
}

std::vector<std::string> chunkAll(const char *input, std::size_t cap = 31)
{
	std::vector<std::string> out;
	splitUtf8Chunks(input, cap, [&](const char *ptr, std::size_t len) {
		assert(len > 0 && len <= cap);
		out.emplace_back(ptr, len);
	});
	return out;
}

}  // namespace

int main()
{
	// 1. Qt special key mapping.
	assert(sdlKeyForQt(0x01000000) == SDLK_ESCAPE);
	assert(sdlKeyForQt(0x01000001) == SDLK_TAB);
	assert(sdlKeyForQt(0x01000003) == SDLK_BACKSPACE);
	assert(sdlKeyForQt(0x01000004) == SDLK_RETURN);
	assert(sdlKeyForQt(0x01000005) == SDLK_KP_ENTER);
	assert(sdlKeyForQt(0x01000007) == SDLK_DELETE);
	assert(sdlKeyForQt(0x01000010) == SDLK_HOME);
	assert(sdlKeyForQt(0x01000011) == SDLK_END);
	assert(sdlKeyForQt(0x01000012) == SDLK_LEFT);
	assert(sdlKeyForQt(0x01000013) == SDLK_UP);
	assert(sdlKeyForQt(0x01000014) == SDLK_RIGHT);
	assert(sdlKeyForQt(0x01000015) == SDLK_DOWN);
	assert(sdlKeyForQt(0x12345678) == SDLK_UNKNOWN);

	// 2. Empty & null input.
	assert(chunkAll("").empty());
	assert(chunkAll(nullptr).empty());

	// 3. Long ASCII string (> 80 bytes).
	{
		const std::string ascii(80, 'A');
		const auto chunks = chunkAll(ascii.c_str());
		assert(chunks.size() == 3);
		assert(chunks[0].size() == 31 && chunks[1].size() == 31 && chunks[2].size() == 18);
		assert(chunks[0] + chunks[1] + chunks[2] == ascii);
	}

	// 4. Turkish multi-byte UTF-8 boundary: "a" (1 byte) + 20x "ğ" (2 bytes each) = 41 bytes.
	// Byte 31 falls right inside a 2-byte "ğ", so chunk 0 MUST back off to 29 bytes (1 + 14*2)
	// or 31? Wait: byte 0 is 'a', then indices 1..2, 3..4, ..., 29..30, 31..32 -> index 31 is a
	// leading byte (1+15*2 = 31). Adding "ab" makes index 31 a continuation byte (2+14*2 = 30).
	{
		std::string tr = "ab";
		for (int i = 0; i < 20; ++i) tr += "ğ";
		const auto chunks = chunkAll(tr.c_str());
		assert(chunks.size() == 2);
		std::string joined;
		for (const auto &c : chunks) {
			assert(isValidUtf8(c));
			joined += c;
		}
		assert(joined == tr);
	}

	// 5. 4-byte emoji crossing the 31-byte boundary ("abc" + 10x "🚀" = 43 bytes).
	{
		std::string emojis = "abc";
		for (int i = 0; i < 10; ++i) emojis += "🚀";
		const auto chunks = chunkAll(emojis.c_str());
		assert(chunks.size() == 2);
		std::string joined;
		for (const auto &c : chunks) {
			assert(isValidUtf8(c));
			joined += c;
		}
		assert(joined == emojis);
	}

	// 6. Malformed continuation bytes longer than capacity terminate cleanly (no infinite loop).
	{
		std::string bad(40, '\x80');
		const auto chunks = chunkAll(bad.c_str());
		assert(chunks.empty());
	}

	std::puts("PASS sailfish_keyboard_logic: Qt key map, UTF-8 31-byte Turkish/emoji splitting, malformed run termination");
	return 0;
}

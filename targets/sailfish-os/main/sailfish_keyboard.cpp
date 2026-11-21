/* targets/sailfish-os/main/sailfish_keyboard.cpp
 * Sailfish OS on-screen keyboard (Maliit) client.
 *
 * SDL's Wayland backend does not talk to the Sailfish keyboard, so focusing an
 * <input> would never raise it. This client asks the Maliit server to show the
 * keyboard while an input is focused and turns what the keyboard sends into
 * ordinary SDL events: committed text becomes SDL_TEXTINPUT and special keys
 * (Backspace, Enter, arrows, ...) become SDL_KEYDOWN/SDL_KEYUP. Everything then
 * takes the same path as a hardware keyboard (the shared keyboard Producer),
 * including UTF-8 text, Enter commits and whole-character Backspace.
 *
 * Maliit talks D-Bus through GLib, so the default GLib main context is pumped
 * once per frame before SDL events are read. Without a Maliit server (for
 * example on a desktop session) the client logs once and stays inert. */

#include "sailfish_keyboard.h"

#include <SDL2/SDL.h>
#include <maliit-glib/maliitbus.h>
#include <maliit-glib/maliitinputmethod.h>

#include <cstdio>
#include <cstring>

extern "C" Uint32 sailfish_display_window_id();

namespace {

MaliitInputMethod *g_input_method = nullptr;
MaliitServer *g_server = nullptr;
MaliitContext *g_context = nullptr;
bool g_visible = false;
int g_focused_input = -1;

// Qt key codes the Sailfish keyboard sends for keys that are not text.
SDL_Keycode sdlKeyForQt(int key)
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

void pushKey(Uint32 type, SDL_Keycode sym)
{
	SDL_Event event{};
	event.type = type;
	event.key.timestamp = SDL_GetTicks();
	event.key.windowID = sailfish_display_window_id();
	event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
	event.key.keysym.sym = sym;
	event.key.keysym.scancode = SDL_GetScancodeFromKey(sym);
	SDL_PushEvent(&event);
}

// SDL_TEXTINPUT holds at most 31 bytes; split longer commits (pasted text,
// whole words) on UTF-8 character boundaries.
void pushText(const char *utf8)
{
	const std::size_t capacity = sizeof(SDL_TextInputEvent::text) - 1;
	std::size_t length = std::strlen(utf8);
	while (length) {
		std::size_t take = length < capacity ? length : capacity;
		while (take < length && take && (static_cast<unsigned char>(utf8[take]) & 0xc0) == 0x80) --take;
		if (!take) return;  // malformed: a continuation run longer than an event
		SDL_Event event{};
		event.type = SDL_TEXTINPUT;
		event.text.timestamp = SDL_GetTicks();
		event.text.windowID = sailfish_display_window_id();
		std::memcpy(event.text.text, utf8, take);
		SDL_PushEvent(&event);
		utf8 += take;
		length -= take;
	}
}

gboolean onCommitString(MaliitContext *context, GDBusMethodInvocation *invocation,
                        const gchar *text, gint, gint, gint, gpointer)
{
	if (text && *text) pushText(text);
	maliit_context_complete_commit_string(context, invocation);
	return TRUE;
}

// QEvent::KeyRelease is 7; anything else is treated as a press.
gboolean onKeyEvent(MaliitContext *context, GDBusMethodInvocation *invocation,
                    gint type, gint key, gint, const gchar *text, gboolean, gint, guchar, gpointer)
{
	const SDL_Keycode sym = sdlKeyForQt(key);
	if (sym != SDLK_UNKNOWN) pushKey(type == 7 ? SDL_KEYUP : SDL_KEYDOWN, sym);
	else if (type != 7 && text && *text) pushText(text);
	maliit_context_complete_key_event(context, invocation);
	return TRUE;
}

void updateWidgetInformation(bool focused, bool hiddenText)
{
	if (!g_server) return;
	GVariantBuilder builder;
	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&builder, "{sv}", "focusState", g_variant_new_boolean(focused));
	g_variant_builder_add(&builder, "{sv}", "hiddenText", g_variant_new_boolean(hiddenText));
	// Pre-edit text is not rendered by the target, so the keyboard must commit
	// every key at once instead of holding words back for prediction.
	g_variant_builder_add(&builder, "{sv}", "predictionEnabled", g_variant_new_boolean(false));
	g_variant_builder_add(&builder, "{sv}", "correctionEnabled", g_variant_new_boolean(false));
	GVariant *state = g_variant_ref_sink(g_variant_builder_end(&builder));
	GError *error = nullptr;
	if (!maliit_server_call_update_widget_information_sync(g_server, state, TRUE, nullptr, &error)) {
		std::fprintf(stderr, "[sailfish keyboard] widget update failed: %s\n", error ? error->message : "unknown");
		if (error) g_error_free(error);
	}
	g_variant_unref(state);
}

}  // namespace

void sailfish_keyboard_init()
{
	GError *error = nullptr;
	g_context = maliit_get_context_sync(nullptr, &error);
	if (!g_context) {
		std::fprintf(stderr, "[sailfish keyboard] Maliit unavailable: %s\n", error ? error->message : "unknown");
		if (error) g_error_free(error);
		return;
	}
	g_server = maliit_get_server_sync(nullptr, &error);
	if (!g_server) {
		std::fprintf(stderr, "[sailfish keyboard] Maliit server unavailable: %s\n", error ? error->message : "unknown");
		if (error) g_error_free(error);
		g_object_unref(g_context);
		g_context = nullptr;
		return;
	}
	g_signal_connect(g_context, "handle-commit-string", G_CALLBACK(onCommitString), nullptr);
	g_signal_connect(g_context, "handle-key-event", G_CALLBACK(onKeyEvent), nullptr);
	g_input_method = maliit_input_method_new();
	std::fprintf(stderr, "[sailfish keyboard] Maliit ready\n");
}

void sailfish_keyboard_pump()
{
	if (!g_context) return;
	while (g_main_context_iteration(nullptr, FALSE)) {}
}

void sailfish_keyboard_update(int activeInputId, bool hiddenText)
{
	if (!g_input_method) return;
	const bool show = activeInputId >= 0;
	if (show == g_visible && activeInputId == g_focused_input) return;
	g_focused_input = activeInputId;
	updateWidgetInformation(show, hiddenText);
	if (show == g_visible) return;
	g_visible = show;
	if (show) maliit_input_method_show(g_input_method);
	else maliit_input_method_hide(g_input_method);
	std::fprintf(stderr, "[sailfish keyboard] %s (input %d)\n", show ? "show" : "hide", activeInputId);
}

void sailfish_keyboard_shutdown()
{
	if (g_input_method && g_visible) maliit_input_method_hide(g_input_method);
	g_visible = false;
	g_focused_input = -1;
	if (g_input_method) g_object_unref(g_input_method);
	if (g_server) g_object_unref(g_server);
	if (g_context) g_object_unref(g_context);
	g_input_method = nullptr;
	g_server = nullptr;
	g_context = nullptr;
}

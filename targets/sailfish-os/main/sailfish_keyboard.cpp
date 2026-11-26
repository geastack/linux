/* targets/sailfish-os/main/sailfish_keyboard.cpp
 * Sailfish OS on-screen keyboard (Maliit) client via Harbour-allowed gio-2.0 (GDBus).
 *
 * SDL's Wayland backend does not talk to the Sailfish keyboard, so focusing an
 * <input> would never raise it. This client talks directly to the Maliit server
 * over its peer-to-peer D-Bus socket using GLib/GIO (allowed by Jolla Harbour,
 * unlike libmaliit-glib):
 *   1. Read the peer-to-peer D-Bus address from MALIIT_SERVER_ADDRESS or the
 *      session bus property org.maliit.server /org/maliit/server/address
 *      (org.maliit.Server.Address.address).
 *   2. Connect directly and export /com/meego/inputmethod/inputcontext
 *      (com.meego.inputmethod.inputcontext1) to receive commitString, keyEvent,
 *      imInitiatedHide, and activationLostEvent.
 *   3. Call /com/meego/inputmethod/uiserver1 (com.meego.inputmethod.uiserver1)
 *      methods activateContext, updateWidgetInformation, showInputMethod, and
 *      hideInputMethod.
 *
 * Committed text becomes SDL_TEXTINPUT and special keys become SDL_KEYDOWN /
 * SDL_KEYUP so everything takes the exact same path as a hardware keyboard. */

#include "sailfish_keyboard.h"
#include "sailfish_keyboard_logic.h"

#include <SDL2/SDL.h>
#include <gio/gio.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" Uint32 sailfish_display_window_id();

namespace {

GDBusConnection *g_conn = nullptr;
GDBusNodeInfo *g_node_info = nullptr;
guint g_object_id = 0;
bool g_visible = false;
bool g_hidden_text = false;
int g_focused_input = -1;

constexpr const char kInputContextXml[] = R"xml(
<node>
  <interface name='com.meego.inputmethod.inputcontext1'>
    <method name='activationLostEvent'/>
    <method name='imInitiatedHide'/>
    <method name='commitString'>
      <arg type='s' name='string' direction='in'/>
      <arg type='i' name='replaceStart' direction='in'/>
      <arg type='i' name='replaceLength' direction='in'/>
      <arg type='i' name='cursorPos' direction='in'/>
    </method>
    <method name='updatePreedit'>
      <arg type='s' name='string' direction='in'/>
      <arg type='a(iia{sv})' name='format' direction='in'/>
      <arg type='i' name='replaceStart' direction='in'/>
      <arg type='i' name='replaceLength' direction='in'/>
      <arg type='i' name='cursorPos' direction='in'/>
    </method>
    <method name='keyEvent'>
      <arg type='i' name='type' direction='in'/>
      <arg type='i' name='key' direction='in'/>
      <arg type='i' name='modifiers' direction='in'/>
      <arg type='s' name='text' direction='in'/>
      <arg type='b' name='autoRepeat' direction='in'/>
      <arg type='i' name='count' direction='in'/>
      <arg type='y' name='requestType' direction='in'/>
    </method>
    <method name='updateInputMethodArea'>
      <arg type='i' name='x' direction='in'/>
      <arg type='i' name='y' direction='in'/>
      <arg type='i' name='width' direction='in'/>
      <arg type='i' name='height' direction='in'/>
    </method>
    <method name='setGlobalCorrectionEnabled'>
      <arg type='b' name='enabled' direction='in'/>
    </method>
    <method name='setRedirectKeys'>
      <arg type='b' name='enabled' direction='in'/>
    </method>
    <method name='setDetectableAutoRepeat'>
      <arg type='b' name='enabled' direction='in'/>
    </method>
    <method name='setSelection'>
      <arg type='i' name='start' direction='in'/>
      <arg type='i' name='length' direction='in'/>
    </method>
    <method name='setLanguage'>
      <arg type='s' name='language' direction='in'/>
    </method>
  </interface>
</node>
)xml";

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
	gea::sailfish::keyboard::splitUtf8Chunks(utf8, capacity, [](const char *chunk, std::size_t take) {
		SDL_Event event{};
		event.type = SDL_TEXTINPUT;
		event.text.timestamp = SDL_GetTicks();
		event.text.windowID = sailfish_display_window_id();
		std::memcpy(event.text.text, chunk, take);
		SDL_PushEvent(&event);
	});
}

void onInputContextMethodCall(GDBusConnection *connection, const gchar *sender,
                              const gchar *object_path, const gchar *interface_name,
                              const gchar *method_name, GVariant *parameters,
                              GDBusMethodInvocation *invocation, gpointer user_data)
{
	(void)connection;
	(void)sender;
	(void)object_path;
	(void)interface_name;
	(void)user_data;

	if (std::strcmp(method_name, "commitString") == 0) {
		const gchar *text = nullptr;
		gint replace_start = 0, replace_length = 0, cursor_pos = 0;
		g_variant_get(parameters, "(&siii)", &text, &replace_start, &replace_length, &cursor_pos);
		if (text && *text) pushText(text);
	} else if (std::strcmp(method_name, "keyEvent") == 0) {
		gint type = 0, key = 0, modifiers = 0, count = 0;
		const gchar *text = nullptr;
		gboolean auto_repeat = FALSE;
		guchar request_type = 0;
		g_variant_get(parameters, "(iii&sbiy)", &type, &key, &modifiers, &text, &auto_repeat, &count, &request_type);
		const SDL_Keycode sym = gea::sailfish::keyboard::sdlKeyForQt(key);
		// QEvent::KeyRelease is 7; anything else is treated as a press.
		if (sym != SDLK_UNKNOWN) pushKey(type == 7 ? SDL_KEYUP : SDL_KEYDOWN, sym);
		else if (type != 7 && text && *text) pushText(text);
	} else if (std::strcmp(method_name, "imInitiatedHide") == 0 ||
	           std::strcmp(method_name, "activationLostEvent") == 0) {
		g_visible = false;
	}

	g_dbus_method_invocation_return_value(invocation, nullptr);
}

const GDBusInterfaceVTable kInputContextVTable = {
	onInputContextMethodCall,
	nullptr,
	nullptr,
	{nullptr}
};

std::string resolveMaliitAddress()
{
	if (const char *env = std::getenv("MALIIT_SERVER_ADDRESS"); env && *env) {
		return env;
	}

	GError *error = nullptr;
	GDBusConnection *session = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
	if (!session) {
		std::fprintf(stderr, "[sailfish keyboard] session D-Bus unavailable: %s\n", error ? error->message : "unknown");
		if (error) g_error_free(error);
		return "";
	}

	GVariant *reply = g_dbus_connection_call_sync(
		session,
		"org.maliit.server",
		"/org/maliit/server/address",
		"org.freedesktop.DBus.Properties",
		"Get",
		g_variant_new("(ss)", "org.maliit.Server.Address", "address"),
		G_VARIANT_TYPE("(v)"),
		G_DBUS_CALL_FLAGS_NONE,
		1000,
		nullptr,
		&error);
	g_object_unref(session);

	if (!reply) {
		std::fprintf(stderr, "[sailfish keyboard] Maliit unavailable: %s\n", error ? error->message : "unknown");
		if (error) g_error_free(error);
		return "";
	}

	GVariant *inner = nullptr;
	g_variant_get(reply, "(v)", &inner);
	std::string address;
	if (inner && g_variant_is_of_type(inner, G_VARIANT_TYPE_STRING)) {
		const gchar *str = g_variant_get_string(inner, nullptr);
		if (str) address = str;
	}
	if (inner) g_variant_unref(inner);
	g_variant_unref(reply);
	return address;
}

bool callServer(const char *method, GVariant *params)
{
	if (!g_conn) {
		if (params) g_variant_unref(g_variant_ref_sink(params));
		return false;
	}
	GError *error = nullptr;
	GVariant *reply = g_dbus_connection_call_sync(
		g_conn,
		nullptr,
		"/com/meego/inputmethod/uiserver1",
		"com.meego.inputmethod.uiserver1",
		method,
		params,
		nullptr,
		G_DBUS_CALL_FLAGS_NONE,
		1000,
		nullptr,
		&error);
	if (!reply) {
		std::fprintf(stderr, "[sailfish keyboard] %s failed: %s\n", method, error ? error->message : "unknown");
		if (error) g_error_free(error);
		return false;
	}
	g_variant_unref(reply);
	return true;
}

void updateWidgetInformation(bool focused, bool hiddenText, bool focusChanged)
{
	GVariantBuilder builder;
	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&builder, "{sv}", "focusState", g_variant_new_boolean(focused ? TRUE : FALSE));
	g_variant_builder_add(&builder, "{sv}", "hiddenText", g_variant_new_boolean(hiddenText ? TRUE : FALSE));
	// Pre-edit text is not rendered by the target, so the keyboard must commit
	// every key at once instead of holding words back for prediction.
	g_variant_builder_add(&builder, "{sv}", "predictionEnabled", g_variant_new_boolean(FALSE));
	g_variant_builder_add(&builder, "{sv}", "correctionEnabled", g_variant_new_boolean(FALSE));
	GVariant *state = g_variant_builder_end(&builder);  // floating, consumed by @a{sv}
	callServer("updateWidgetInformation", g_variant_new("(@a{sv}b)", state, focusChanged ? TRUE : FALSE));
}

}  // namespace

void sailfish_keyboard_init()
{
	const std::string address = resolveMaliitAddress();
	if (address.empty()) return;

	GError *error = nullptr;
	g_conn = g_dbus_connection_new_for_address_sync(
		address.c_str(),
		G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
		nullptr,
		nullptr,
		&error);
	if (!g_conn) {
		std::fprintf(stderr, "[sailfish keyboard] Maliit server unavailable: %s\n", error ? error->message : "unknown");
		if (error) g_error_free(error);
		return;
	}

	g_node_info = g_dbus_node_info_new_for_xml(kInputContextXml, &error);
	if (!g_node_info || !g_node_info->interfaces || !g_node_info->interfaces[0]) {
		std::fprintf(stderr, "[sailfish keyboard] invalid Maliit introspection XML: %s\n", error ? error->message : "unknown");
		if (error) g_error_free(error);
		sailfish_keyboard_shutdown();
		return;
	}

	g_object_id = g_dbus_connection_register_object(
		g_conn,
		"/com/meego/inputmethod/inputcontext",
		g_node_info->interfaces[0],
		&kInputContextVTable,
		nullptr,
		nullptr,
		&error);
	if (g_object_id == 0) {
		std::fprintf(stderr, "[sailfish keyboard] Maliit context registration failed: %s\n", error ? error->message : "unknown");
		if (error) g_error_free(error);
		sailfish_keyboard_shutdown();
		return;
	}

	std::fprintf(stderr, "[sailfish keyboard] Maliit ready (GDBus)\n");
}

void sailfish_keyboard_pump()
{
	if (!g_conn) return;
	while (g_main_context_iteration(nullptr, FALSE)) {}
}

void sailfish_keyboard_update(int activeInputId, bool hiddenText)
{
	if (!g_conn) return;
	const bool show = activeInputId >= 0;
	if (show == g_visible && activeInputId == g_focused_input && (!show || hiddenText == g_hidden_text)) return;
	const bool focusChanged = activeInputId != g_focused_input;
	const bool wasVisible = g_visible;
	g_focused_input = activeInputId;
	g_hidden_text = hiddenText;
	if (show && !wasVisible) callServer("activateContext", nullptr);
	updateWidgetInformation(show, hiddenText, focusChanged);
	if (show == wasVisible) return;
	g_visible = show;
	if (show) callServer("showInputMethod", nullptr);
	else callServer("hideInputMethod", nullptr);
	std::fprintf(stderr, "[sailfish keyboard] %s (input %d)\n", show ? "show" : "hide", activeInputId);
}

void sailfish_keyboard_shutdown()
{
	if (g_conn && g_visible) callServer("hideInputMethod", nullptr);
	g_visible = false;
	g_hidden_text = false;
	g_focused_input = -1;
	if (g_conn && g_object_id != 0) {
		g_dbus_connection_unregister_object(g_conn, g_object_id);
		g_object_id = 0;
	}
	if (g_node_info) {
		g_dbus_node_info_unref(g_node_info);
		g_node_info = nullptr;
	}
	if (g_conn) {
		g_object_unref(g_conn);
		g_conn = nullptr;
	}
}

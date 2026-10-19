/* targets/sailfish-os/main/sailfish_keyboard.cpp
 * Shows and hides the Sailfish system keyboard and forwards its commit
 * string, Backspace, and Enter into the shared input helpers.
 *
 * Harbour does not allow linking libmaliit-glib (it is absent from
 * sdk-harbour-rpmvalidator allowed_libraries.conf and from
 * https://docs.sailfishos.org/Develop/Apps/Harbour/Allowed_APIs/). GIO is
 * allowed, so the bridge uses the Maliit D-Bus API directly. The Gea
 * on-screen keyboard stays compiled out (GEA_EMBEDDED_ENABLE_VIRTUAL_KEYBOARD=0);
 * SDL text input remains for the emulator and a hardware keyboard.
 */

#include "sailfish_keyboard.h"

#include <gio/gio.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr int kQtKeyRelease = 7;
constexpr int kQtKeyBackspace = 0x01000003;
constexpr int kQtKeyReturn = 0x01000004;
constexpr int kQtKeyEnter = 0x01000005;

const char *kContextXml =
    "<node>"
    "  <interface name='com.meego.inputmethod.inputcontext1'>"
    "    <method name='activationLostEvent'/>"
    "    <method name='imInitiatedHide'/>"
    "    <method name='commitString'>"
    "      <arg type='s' direction='in'/>"
    "      <arg type='i' direction='in'/>"
    "      <arg type='i' direction='in'/>"
    "      <arg type='i' direction='in'/>"
    "    </method>"
    "    <method name='updatePreedit'>"
    "      <arg type='s' direction='in'/>"
    "      <arg type='a(iii)' direction='in'/>"
    "      <arg type='i' direction='in'/>"
    "      <arg type='i' direction='in'/>"
    "      <arg type='i' direction='in'/>"
    "    </method>"
    "    <method name='keyEvent'>"
    "      <arg type='i' direction='in'/>"
    "      <arg type='i' direction='in'/>"
    "      <arg type='i' direction='in'/>"
    "      <arg type='s' direction='in'/>"
    "      <arg type='b' direction='in'/>"
    "      <arg type='i' direction='in'/>"
    "      <arg type='y' direction='in'/>"
    "    </method>"
    "    <method name='updateInputMethodArea'>"
    "      <arg type='i' direction='in'/>"
    "      <arg type='i' direction='in'/>"
    "      <arg type='i' direction='in'/>"
    "      <arg type='i' direction='in'/>"
    "    </method>"
    "    <method name='setGlobalCorrectionEnabled'><arg type='b' direction='in'/></method>"
    "    <method name='preeditRectangle'>"
    "      <arg type='b' direction='out'/>"
    "      <arg type='i' direction='out'/>"
    "      <arg type='i' direction='out'/>"
    "      <arg type='i' direction='out'/>"
    "      <arg type='i' direction='out'/>"
    "    </method>"
    "    <method name='setRedirectKeys'><arg type='b' direction='in'/></method>"
    "    <method name='setDetectableAutoRepeat'><arg type='b' direction='in'/></method>"
    "    <method name='setSelection'>"
    "      <arg type='i' direction='in'/>"
    "      <arg type='i' direction='in'/>"
    "    </method>"
    "    <method name='selection'>"
    "      <arg type='b' direction='out'/>"
    "      <arg type='s' direction='out'/>"
    "    </method>"
    "    <method name='setLanguage'><arg type='s' direction='in'/></method>"
    "    <method name='pluginSettingsLoaded'>"
    "      <arg type='a(sssia(ssibva{sv}))' direction='in'/>"
    "    </method>"
    "  </interface>"
    "</node>";

SailfishKeyboardAppendFn g_append = nullptr;
SailfishKeyboardActionFn g_backspace = nullptr;
SailfishKeyboardActionFn g_enter = nullptr;
SailfishKeyboardActionFn g_focus_lost = nullptr;
SailfishKeyboardAreaFn g_area = nullptr;

GDBusConnection *g_peer = nullptr;
GDBusNodeInfo *g_node_info = nullptr;
bool g_visible = false;
int g_focused_id = -2;
bool g_hidden_text = false;
int g_area_x = 0;
int g_area_y = 0;
int g_area_w = 0;
int g_area_h = -1;

void complete_empty(GDBusMethodInvocation *invocation)
{
	g_dbus_method_invocation_return_value(invocation, nullptr);
}

void on_method_call(GDBusConnection *, const gchar *, const gchar *, const gchar *,
                    const gchar *method, GVariant *params, GDBusMethodInvocation *invocation, gpointer)
{
	if (std::strcmp(method, "commitString") == 0) {
		const char *text = nullptr;
		gint replacement_start = 0;
		gint replacement_length = 0;
		gint cursor = 0;
		g_variant_get(params, "(siii)", &text, &replacement_start, &replacement_length, &cursor);
		if (text && g_append) g_append(text);
		complete_empty(invocation);
		return;
	}
	if (std::strcmp(method, "keyEvent") == 0) {
		gint type = 0;
		gint key = 0;
		gint modifiers = 0;
		const char *text = nullptr;
		gboolean auto_repeat = FALSE;
		gint count = 0;
		guchar scan = 0;
		g_variant_get(params, "(iiisbiy)", &type, &key, &modifiers, &text, &auto_repeat, &count, &scan);
		(void)modifiers;
		(void)text;
		(void)auto_repeat;
		(void)count;
		(void)scan;
		// KeyRelease would delete or submit twice. Any other type is a press.
		// QEvent::KeyRelease is 7. Acting on it as well would delete or submit twice.
		// KeyPress is 6; any other non-release type is treated as a press.
		if (type != kQtKeyRelease) {
			if (key == kQtKeyBackspace && g_backspace) g_backspace();
			else if ((key == kQtKeyReturn || key == kQtKeyEnter) && g_enter) g_enter();
		}
		complete_empty(invocation);
		return;
	}
	if (std::strcmp(method, "updateInputMethodArea") == 0) {
		gint x = 0, y = 0, width = 0, height = 0;
		g_variant_get(params, "(iiii)", &x, &y, &width, &height);
		if (x != g_area_x || y != g_area_y || width != g_area_w || height != g_area_h) {
			g_area_x = x;
			g_area_y = y;
			g_area_w = width;
			g_area_h = height;
			if (g_area) g_area(x, y, width, height);
		}
		complete_empty(invocation);
		return;
	}
	if (std::strcmp(method, "imInitiatedHide") == 0) {
		g_visible = false;
		g_focused_id = -1;
		if (g_focus_lost) g_focus_lost();
		complete_empty(invocation);
		return;
	}
	if (std::strcmp(method, "preeditRectangle") == 0) {
		g_dbus_method_invocation_return_value(invocation, g_variant_new("(biiii)", FALSE, 0, 0, 0, 0));
		return;
	}
	if (std::strcmp(method, "selection") == 0) {
		g_dbus_method_invocation_return_value(invocation, g_variant_new("(bs)", FALSE, ""));
		return;
	}
	complete_empty(invocation);
}

const GDBusInterfaceVTable kVTable = { on_method_call, nullptr, nullptr };

bool call_server(const char *method, GVariant *args)
{
	if (!g_peer) {
		if (args) {
			if (g_variant_is_floating(args)) g_variant_ref_sink(args);
			g_variant_unref(args);
		}
		return false;
	}
	GError *error = nullptr;
	GVariant *reply = g_dbus_connection_call_sync(
	    g_peer, nullptr, "/com/meego/inputmethod/uiserver1",
	    "com.meego.inputmethod.uiserver1", method, args, nullptr,
	    G_DBUS_CALL_FLAGS_NONE, 500, nullptr, &error);
	if (!reply) {
		std::fprintf(stderr, "[sailfish keyboard] %s failed: %s\n", method, error ? error->message : "unknown");
		if (error) g_error_free(error);
		return false;
	}
	g_variant_unref(reply);
	return true;
}

void update_widget(bool focused, bool hidden, bool focus_changed)
{
	GVariantBuilder builder;
	g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&builder, "{sv}", "focusState", g_variant_new_boolean(focused));
	g_variant_builder_add(&builder, "{sv}", "hiddenText", g_variant_new_boolean(hidden));
	g_variant_builder_add(&builder, "{sv}", "predictionEnabled", g_variant_new_boolean(!hidden));
	GVariant *state = g_variant_builder_end(&builder);
	call_server("updateWidgetInformation", g_variant_new("(@a{sv}b)", state, focus_changed ? TRUE : FALSE));
}

}  // namespace

void sailfish_keyboard_init(SailfishKeyboardAppendFn append_text,
                             SailfishKeyboardActionFn backspace,
                             SailfishKeyboardActionFn enter,
                             SailfishKeyboardActionFn focus_lost,
                             SailfishKeyboardAreaFn area)
{
	g_append = append_text;
	g_backspace = backspace;
	g_enter = enter;
	g_focus_lost = focus_lost;
	g_area = area;

	const char *address = g_getenv("MALIIT_SERVER_ADDRESS");
	gchar *owned_address = nullptr;
	GError *error = nullptr;
	if (!address || !*address) {
		GDBusConnection *session = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
		if (!session) {
			std::fprintf(stderr, "[sailfish keyboard] session bus unavailable: %s\n",
			             error ? error->message : "unknown");
			if (error) g_error_free(error);
			return;
		}
		GVariant *reply = g_dbus_connection_call_sync(
		    session, "org.maliit.server", "/org/maliit/server/address",
		    "org.freedesktop.DBus.Properties", "Get",
		    g_variant_new("(ss)", "org.maliit.Server.Address", "address"),
		    G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
		g_object_unref(session);
		if (!reply) {
			std::fprintf(stderr, "[sailfish keyboard] Maliit address unavailable: %s\n",
			             error ? error->message : "unknown");
			if (error) g_error_free(error);
			return;
		}
		GVariant *inner = nullptr;
		g_variant_get(reply, "(v)", &inner);
		g_variant_unref(reply);
		if (!inner || !g_variant_is_of_type(inner, G_VARIANT_TYPE_STRING)) {
			if (inner) g_variant_unref(inner);
			std::fprintf(stderr, "[sailfish keyboard] Maliit address property is not a string\n");
			return;
		}
		owned_address = g_variant_dup_string(inner, nullptr);
		g_variant_unref(inner);
		address = owned_address;
	}

	g_peer = g_dbus_connection_new_for_address_sync(
	    address, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, nullptr, nullptr, &error);
	g_free(owned_address);
	if (!g_peer) {
		std::fprintf(stderr, "[sailfish keyboard] Maliit peer bus failed: %s\n",
		             error ? error->message : "unknown");
		if (error) g_error_free(error);
		return;
	}

	g_node_info = g_dbus_node_info_new_for_xml(kContextXml, &error);
	if (!g_node_info) {
		std::fprintf(stderr, "[sailfish keyboard] context interface failed: %s\n",
		             error ? error->message : "unknown");
		if (error) g_error_free(error);
		g_clear_object(&g_peer);
		return;
	}
	if (g_dbus_connection_register_object(g_peer, "/com/meego/inputmethod/inputcontext",
	                                      g_node_info->interfaces[0], &kVTable, nullptr, nullptr,
	                                      &error) == 0) {
		std::fprintf(stderr, "[sailfish keyboard] context export failed: %s\n",
		             error ? error->message : "unknown");
		if (error) g_error_free(error);
		g_clear_object(&g_peer);
		return;
	}
	std::fprintf(stderr, "[sailfish keyboard] Maliit ready\n");
}

void sailfish_keyboard_pump(void)
{
	if (!g_peer) return;
	while (g_main_context_iteration(nullptr, FALSE)) {}
}

void sailfish_keyboard_update(int active_input_id, const char *input_type)
{
	if (!g_peer) return;
	const bool show = active_input_id >= 0;
	const bool hidden = input_type && std::strcmp(input_type, "password") == 0;
	if (show == g_visible && active_input_id == g_focused_id && hidden == g_hidden_text) return;

	const bool focus_changed = active_input_id != g_focused_id;
	g_focused_id = active_input_id;
	g_hidden_text = hidden;
	update_widget(show, hidden, focus_changed);
	if (show == g_visible) return;

	call_server("activateContext", nullptr);
	if (show) call_server("showInputMethod", nullptr);
	else call_server("hideInputMethod", nullptr);
	g_visible = show;
	std::fprintf(stderr, "[sailfish keyboard] %s input=%d password=%d\n",
	             show ? "show" : "hide", active_input_id, hidden ? 1 : 0);
}

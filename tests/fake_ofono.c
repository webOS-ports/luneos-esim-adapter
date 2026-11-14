/* @@@LICENSE
*
* Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */

#include <pthread.h>
#include <string.h>

#include <gio/gio.h>

#include "fake_ofono.h"

static const char fake_ofono_root_xml[] =
	"<node>"
	" <interface name='org.ofono.Manager'>"
	"  <method name='GetModems'>"
	"   <arg type='a(oa{sv})' direction='out'/>"
	"  </method>"
	" </interface>"
	" <interface name='org.nemomobile.ofono.ModemManager'>"
	"  <method name='GetAvailableModems'>"
	"   <arg type='ao' direction='out'/>"
	"  </method>"
	" </interface>"
	"</node>";

static const char fake_ofono_modem_xml[] =
	"<node>"
	" <interface name='org.ofono.SimManager'>"
	"  <method name='GetProperties'>"
	"   <arg type='a{sv}' direction='out'/>"
	"  </method>"
	"  <method name='SetProperty'>"
	"   <arg type='s' direction='in'/>"
	"   <arg type='v' direction='in'/>"
	"  </method>"
	"  <signal name='PropertyChanged'>"
	"   <arg type='s'/>"
	"   <arg type='v'/>"
	"  </signal>"
	" </interface>"
	" <interface name='org.ofono.EuiccManager'>"
	"  <method name='ResetCard'/>"
	" </interface>"
	"</node>";

struct fake_ofono {
	GThread *thread;
	GMainContext *context;
	GMainLoop *loop;
	char *address;
	GDBusConnection *bus;	/* Used by the thread only */

	/*
	 * Everything below. pthread's rather than GMutex and GCond: glib
	 * builds those on raw futexes, which ThreadSanitizer can't see, and
	 * every access under them would look like a race.
	 */
	pthread_mutex_t lock;
	pthread_cond_t ready_cond;
	gboolean ready;
	gboolean euicc;
	guint slot_count;
	guint active_slot;
	char *reset_error;
	char *slot_error;
	char *busy_file;
	guint reset_calls;
	guint reset_calls_while_busy;
	guint slot_calls;
	guint last_slot;
};

static void fake_ofono_root_call(GDBusConnection *bus, const char *sender,
			const char *path, const char *interface,
			const char *method, GVariant *params,
			GDBusMethodInvocation *call, gpointer user_data)
{
	FakeOfono *ofono = user_data;

	if (!strcmp(method, "GetModems")) {
		GVariantBuilder modems, props;
		GVariantBuilder ifaces;
		gboolean euicc;

		pthread_mutex_lock(&ofono->lock);
		euicc = ofono->euicc;
		pthread_mutex_unlock(&ofono->lock);

		g_variant_builder_init(&ifaces, G_VARIANT_TYPE_STRING_ARRAY);
		g_variant_builder_add(&ifaces, "s", "org.ofono.SimManager");
		if (euicc)
			g_variant_builder_add(&ifaces, "s",
						"org.ofono.EuiccManager");

		g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
		g_variant_builder_add(&props, "{sv}", "Interfaces",
					g_variant_builder_end(&ifaces));

		g_variant_builder_init(&modems, G_VARIANT_TYPE("a(oa{sv})"));
		g_variant_builder_add(&modems, "(o@a{sv})", FAKE_OFONO_MODEM,
					g_variant_builder_end(&props));

		g_dbus_method_invocation_return_value(call,
				g_variant_new("(@a(oa{sv}))",
					g_variant_builder_end(&modems)));
	} else {
		const char * const paths[] = { "/ril_0", FAKE_OFONO_MODEM };

		g_dbus_method_invocation_return_value(call,
				g_variant_new("(@ao)", g_variant_new_objv(paths,
						G_N_ELEMENTS(paths))));
	}
}

static void fake_ofono_modem_call(GDBusConnection *bus, const char *sender,
			const char *path, const char *interface,
			const char *method, GVariant *params,
			GDBusMethodInvocation *call, gpointer user_data)
{
	FakeOfono *ofono = user_data;
	char *error = NULL;

	pthread_mutex_lock(&ofono->lock);
	if (!strcmp(method, "GetProperties")) {
		GVariantBuilder props;

		g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
		g_variant_builder_add(&props, "{sv}", "Present",
					g_variant_new_boolean(TRUE));
		g_variant_builder_add(&props, "{sv}", "CardIdentifier",
				g_variant_new_string("8931087626093906051"));
		if (ofono->slot_count) {
			g_variant_builder_add(&props, "{sv}", "CardSlotCount",
				g_variant_new_uint32(ofono->slot_count));
			g_variant_builder_add(&props, "{sv}", "ActiveCardSlot",
				g_variant_new_uint32(ofono->active_slot));
		}
		pthread_mutex_unlock(&ofono->lock);
		g_dbus_method_invocation_return_value(call,
				g_variant_new("(@a{sv})",
					g_variant_builder_end(&props)));
		return;
	}

	if (!strcmp(method, "SetProperty")) {
		const char *name;
		GVariant *value;

		g_variant_get(params, "(&sv)", &name, &value);
		g_assert_cmpstr(name, ==, "ActiveCardSlot");
		ofono->slot_calls++;
		ofono->last_slot = g_variant_get_uint32(value);
		g_variant_unref(value);
		error = g_strdup(ofono->slot_error);
	} else {
		g_assert_cmpstr(method, ==, "ResetCard");
		ofono->reset_calls++;
		if (ofono->busy_file &&
				g_file_test(ofono->busy_file, G_FILE_TEST_EXISTS))
			ofono->reset_calls_while_busy++;
		error = g_strdup(ofono->reset_error);
	}
	pthread_mutex_unlock(&ofono->lock);

	if (error) {
		g_dbus_method_invocation_return_dbus_error(call, error,
							"fake failure");
		g_free(error);
	} else {
		g_dbus_method_invocation_return_value(call, NULL);
	}
}

static const GDBusInterfaceVTable fake_ofono_root_vtable = {
	fake_ofono_root_call, NULL, NULL, { 0 }
};

static const GDBusInterfaceVTable fake_ofono_modem_vtable = {
	fake_ofono_modem_call, NULL, NULL, { 0 }
};

static void fake_ofono_register(FakeOfono *ofono, const char *path,
			const char *xml, const GDBusInterfaceVTable *vtable)
{
	GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, NULL);
	guint i;

	g_assert_nonnull(node);
	for (i = 0; node->interfaces[i]; i++)
		g_assert_cmpuint(g_dbus_connection_register_object(ofono->bus,
				path, node->interfaces[i], vtable, ofono,
				NULL, NULL), !=, 0);
	g_dbus_node_info_unref(node);
}

static gpointer fake_ofono_thread(gpointer data)
{
	FakeOfono *ofono = data;
	GVariant *reply;

	g_main_context_push_thread_default(ofono->context);

	ofono->bus = g_dbus_connection_new_for_address_sync(ofono->address,
			G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
			G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
			NULL, NULL, NULL);
	g_assert_nonnull(ofono->bus);

	fake_ofono_register(ofono, "/", fake_ofono_root_xml,
				&fake_ofono_root_vtable);
	fake_ofono_register(ofono, FAKE_OFONO_MODEM, fake_ofono_modem_xml,
				&fake_ofono_modem_vtable);

	/* DBUS_NAME_FLAG_DO_NOT_QUEUE; 1 is DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER */
	reply = g_dbus_connection_call_sync(ofono->bus, "org.freedesktop.DBus",
			"/org/freedesktop/DBus", "org.freedesktop.DBus",
			"RequestName", g_variant_new("(su)", "org.ofono", 4),
			G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1,
			NULL, NULL);
	g_assert_nonnull(reply);
	{
		guint32 result = 0;

		g_variant_get(reply, "(u)", &result);
		g_assert_cmpuint(result, ==, 1);
	}
	g_variant_unref(reply);

	pthread_mutex_lock(&ofono->lock);
	ofono->ready = TRUE;
	pthread_cond_signal(&ofono->ready_cond);
	pthread_mutex_unlock(&ofono->lock);

	g_main_loop_run(ofono->loop);

	g_dbus_connection_close_sync(ofono->bus, NULL, NULL);
	g_object_unref(ofono->bus);
	ofono->bus = NULL;

	g_main_context_pop_thread_default(ofono->context);
	return NULL;
}

static void fake_ofono_defaults(FakeOfono *ofono)
{
	ofono->euicc = TRUE;
	ofono->slot_count = 2;
	ofono->active_slot = 2;
	g_clear_pointer(&ofono->reset_error, g_free);
	g_clear_pointer(&ofono->slot_error, g_free);
	g_clear_pointer(&ofono->busy_file, g_free);
	ofono->reset_calls = 0;
	ofono->reset_calls_while_busy = 0;
	ofono->slot_calls = 0;
	ofono->last_slot = 0;
}

void fake_ofono_reset(FakeOfono *ofono)
{
	pthread_mutex_lock(&ofono->lock);
	fake_ofono_defaults(ofono);
	pthread_mutex_unlock(&ofono->lock);
}

FakeOfono *fake_ofono_start(const char *address)
{
	FakeOfono *ofono = g_new0(FakeOfono, 1);

	pthread_mutex_init(&ofono->lock, NULL);
	pthread_cond_init(&ofono->ready_cond, NULL);
	fake_ofono_defaults(ofono);
	ofono->address = g_strdup(address);
	ofono->context = g_main_context_new();
	ofono->loop = g_main_loop_new(ofono->context, FALSE);
	ofono->thread = g_thread_new("fake-ofono", fake_ofono_thread, ofono);

	pthread_mutex_lock(&ofono->lock);
	while (!ofono->ready)
		pthread_cond_wait(&ofono->ready_cond, &ofono->lock);
	pthread_mutex_unlock(&ofono->lock);

	return ofono;
}

static gboolean fake_ofono_quit(gpointer data)
{
	FakeOfono *ofono = data;

	g_main_loop_quit(ofono->loop);
	return G_SOURCE_REMOVE;
}

void fake_ofono_stop(FakeOfono *ofono)
{
	g_main_context_invoke(ofono->context, fake_ofono_quit, ofono);
	g_thread_join(ofono->thread);

	g_main_loop_unref(ofono->loop);
	g_main_context_unref(ofono->context);
	g_free(ofono->address);
	g_free(ofono->reset_error);
	g_free(ofono->slot_error);
	g_free(ofono->busy_file);
	pthread_cond_destroy(&ofono->ready_cond);
	pthread_mutex_destroy(&ofono->lock);
	g_free(ofono);
}

void fake_ofono_set_euicc(FakeOfono *ofono, gboolean euicc)
{
	pthread_mutex_lock(&ofono->lock);
	ofono->euicc = euicc;
	pthread_mutex_unlock(&ofono->lock);
}

void fake_ofono_set_slots(FakeOfono *ofono, guint count, guint active)
{
	pthread_mutex_lock(&ofono->lock);
	ofono->slot_count = count;
	ofono->active_slot = active;
	pthread_mutex_unlock(&ofono->lock);
}

static void fake_ofono_set_string(FakeOfono *ofono, char **field,
					const char *value)
{
	pthread_mutex_lock(&ofono->lock);
	g_free(*field);
	*field = g_strdup(value);
	pthread_mutex_unlock(&ofono->lock);
}

void fake_ofono_set_reset_error(FakeOfono *ofono, const char *error)
{
	fake_ofono_set_string(ofono, &ofono->reset_error, error);
}

void fake_ofono_set_slot_error(FakeOfono *ofono, const char *error)
{
	fake_ofono_set_string(ofono, &ofono->slot_error, error);
}

void fake_ofono_set_busy_file(FakeOfono *ofono, const char *path)
{
	fake_ofono_set_string(ofono, &ofono->busy_file, path);
}

static guint fake_ofono_get(FakeOfono *ofono, const guint *field)
{
	guint value;

	pthread_mutex_lock(&ofono->lock);
	value = *field;
	pthread_mutex_unlock(&ofono->lock);
	return value;
}

guint fake_ofono_reset_calls(FakeOfono *ofono)
{
	return fake_ofono_get(ofono, &ofono->reset_calls);
}

guint fake_ofono_reset_calls_while_busy(FakeOfono *ofono)
{
	return fake_ofono_get(ofono, &ofono->reset_calls_while_busy);
}

guint fake_ofono_slot_calls(FakeOfono *ofono)
{
	return fake_ofono_get(ofono, &ofono->slot_calls);
}

guint fake_ofono_last_slot(FakeOfono *ofono)
{
	return fake_ofono_get(ofono, &ofono->last_slot);
}

static gboolean fake_ofono_emit(gpointer data)
{
	FakeOfono *ofono = data;

	g_assert(g_dbus_connection_emit_signal(ofono->bus, NULL,
			FAKE_OFONO_MODEM, "org.ofono.SimManager",
			"PropertyChanged", g_variant_new("(sv)", "Present",
				g_variant_new_boolean(TRUE)), NULL));
	return G_SOURCE_REMOVE;
}

void fake_ofono_emit_property_changed(FakeOfono *ofono)
{
	g_main_context_invoke(ofono->context, fake_ofono_emit, ofono);
}

// vim:ts=4:sw=4:noexpandtab

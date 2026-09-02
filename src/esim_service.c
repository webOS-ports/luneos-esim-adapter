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

/*
 * com.webos.service.esim
 *
 * Two halves:
 *
 *   - Profile management (list, download, enable, delete) is SGP.22, which is
 *     lpac's job. See lpac_client.c.
 *   - Everything about the *card* - is there an eUICC, which slot is it in,
 *     which slot is live - is ofono's, over org.ofono.SimManager and the
 *     org.ofono.EuiccManager interface added for this work.
 *
 * The slot part matters more than it looks. On a phone whose eUICC sits in a
 * second physical slot behind a single logical modem (Pixel 3a, for one),
 * nothing on the eUICC is reachable until that slot is the active one, and a
 * profile stays unusable after enabling until the card is re-read.
 */

#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <gio/gio.h>
#include <luna-service2/lunaservice.h>
#include <pbnjson.h>

#include "esim_service.h"
#include "lpac_client.h"
#include "qr_scan.h"
#include "luna_service_utils.h"

#define ESIM_SERVICE_NAME		"com.webos.service.esim"

#define OFONO_SERVICE			"org.ofono"
#define OFONO_MANAGER_INTERFACE		"org.ofono.Manager"
#define OFONO_SIM_MANAGER_INTERFACE	"org.ofono.SimManager"
#define OFONO_EUICC_MANAGER_INTERFACE	"org.ofono.EuiccManager"

extern GMainLoop *event_loop;

struct esim_service {
	LSHandle *handle;
	GDBusConnection *bus;
	char *modem;			/* modem exposing EuiccManager */
	guint prop_watch;
};

struct esim_request {
	struct esim_service *service;
	LSHandle *handle;
	LSMessage *message;
};

static struct esim_request *esim_request_new(struct esim_service *service,
					LSHandle *handle, LSMessage *message)
{
	struct esim_request *req = g_new0(struct esim_request, 1);

	req->service = service;
	req->handle = handle;
	req->message = message;
	LSMessageRef(message);

	return req;
}

static void esim_request_free(struct esim_request *req)
{
	LSMessageUnref(req->message);
	g_free(req);
}

/*
 * Locating the modem. The interface only shows up on modems whose SIM driver
 * can do logical channels, so its presence is also the answer to "can this
 * device do eSIM at all".
 */
static char *esim_find_modem(GDBusConnection *bus)
{
	GVariant *reply, *modems, *props;
	GVariantIter iter;
	const char *path;
	char *found = NULL;

	reply = g_dbus_connection_call_sync(bus, OFONO_SERVICE, "/",
			OFONO_MANAGER_INTERFACE, "GetModems", NULL,
			G_VARIANT_TYPE("(a(oa{sv}))"), G_DBUS_CALL_FLAGS_NONE,
			10000, NULL, NULL);
	if (!reply)
		return NULL;

	modems = g_variant_get_child_value(reply, 0);
	g_variant_iter_init(&iter, modems);

	while (!found && g_variant_iter_next(&iter, "(&o@a{sv})", &path, &props)) {
		GVariant *ifaces = g_variant_lookup_value(props, "Interfaces",
						G_VARIANT_TYPE_STRING_ARRAY);

		if (ifaces) {
			GVariantIter names;
			const char *name;

			g_variant_iter_init(&names, ifaces);
			while (g_variant_iter_next(&names, "&s", &name)) {
				if (!strcmp(name, OFONO_EUICC_MANAGER_INTERFACE)) {
					found = g_strdup(path);
					break;
				}
			}
			g_variant_unref(ifaces);
		}

		g_variant_unref(props);
	}

	g_variant_unref(modems);
	g_variant_unref(reply);

	return found;
}

static jvalue_ref esim_build_status(struct esim_service *service)
{
	jvalue_ref reply = jobject_create();
	GVariant *props = NULL;

	g_free(service->modem);
	service->modem = service->bus ? esim_find_modem(service->bus) : NULL;

	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply, J_CSTR_TO_JVAL("available"),
			jboolean_create(service->modem != NULL));

	if (!service->modem)
		return reply;

	jobject_put(reply, J_CSTR_TO_JVAL("modemPath"),
			jstring_create(service->modem));

	props = g_dbus_connection_call_sync(service->bus, OFONO_SERVICE,
			service->modem, OFONO_SIM_MANAGER_INTERFACE,
			"GetProperties", NULL, G_VARIANT_TYPE("(a{sv})"),
			G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL);

	if (props) {
		GVariant *dict = g_variant_get_child_value(props, 0);
		guint32 u32 = 0;
		gboolean present = FALSE;
		const char *iccid = NULL;
		GVariant *v;

		if (g_variant_lookup(dict, "CardSlotCount", "u", &u32))
			jobject_put(reply, J_CSTR_TO_JVAL("slotCount"),
					jnumber_create_i32(u32));

		if (g_variant_lookup(dict, "ActiveCardSlot", "u", &u32))
			jobject_put(reply, J_CSTR_TO_JVAL("activeSlot"),
					jnumber_create_i32(u32));

		if (g_variant_lookup(dict, "Present", "b", &present))
			jobject_put(reply, J_CSTR_TO_JVAL("simPresent"),
					jboolean_create(present));

		v = g_variant_lookup_value(dict, "CardIdentifier",
						G_VARIANT_TYPE_STRING);
		if (v) {
			iccid = g_variant_get_string(v, NULL);
			jobject_put(reply, J_CSTR_TO_JVAL("iccid"),
					jstring_create(iccid));
			g_variant_unref(v);
		}

		g_variant_unref(dict);
		g_variant_unref(props);
	}

	return reply;
}

static bool esim_get_status(LSHandle *handle, LSMessage *message, void *user_data)
{
	struct esim_service *service = user_data;
	jvalue_ref reply;
	bool subscribed;

	subscribed = luna_service_check_for_subscription_and_process(handle,
								message);

	reply = esim_build_status(service);
	jobject_put(reply, J_CSTR_TO_JVAL("subscribed"),
			jboolean_create(subscribed));

	luna_service_message_validate_and_send(handle, message, reply);
	j_release(&reply);

	return true;
}

static void esim_post_status(struct esim_service *service)
{
	jvalue_ref reply = esim_build_status(service);

	luna_service_post_subscription(service->handle, "/", "getStatus", reply);
	j_release(&reply);
}

static void esim_sim_props_changed(GDBusConnection *bus, const char *sender,
			const char *path, const char *interface,
			const char *signal, GVariant *params, gpointer user_data)
{
	struct esim_service *service = user_data;

	/*
	 * Slot switches and profile enables both surface here, and both are
	 * things the UI has to notice - see the card-reset note at the top.
	 */
	esim_post_status(service);
}

/* --- lpac-backed methods ------------------------------------------------- */

static void esim_lpac_reply(int code, const char *message, jvalue_ref result,
							void *user_data)
{
	struct esim_request *req = user_data;
	jvalue_ref reply = jobject_create();

	if (code == 0) {
		jobject_put(reply, J_CSTR_TO_JVAL("returnValue"),
				jboolean_create(true));
		if (jis_valid(result))
			jobject_put(reply, J_CSTR_TO_JVAL("result"),
					jvalue_duplicate(result));
	} else {
		jobject_put(reply, J_CSTR_TO_JVAL("returnValue"),
				jboolean_create(false));
		jobject_put(reply, J_CSTR_TO_JVAL("errorCode"),
				jnumber_create_i32(code));
		jobject_put(reply, J_CSTR_TO_JVAL("errorText"),
				jstring_create(message ? message : "lpac failed"));
		/*
		 * lpac names the step that failed; the server's own word for
		 * why - "Invalid", "NoEligibleProfile" - arrives beside it as
		 * data. Without that the caller knows where it stopped but not
		 * what to tell the user about it.
		 */
		if (jis_valid(result))
			jobject_put(reply, J_CSTR_TO_JVAL("errorDetail"),
					jvalue_duplicate(result));
	}

	luna_service_message_validate_and_send(req->handle, req->message, reply);
	j_release(&reply);
	esim_request_free(req);
}

static void esim_lpac_progress(const char *step, void *user_data)
{
	struct esim_request *req = user_data;
	jvalue_ref update = jobject_create();

	/*
	 * A download is a dozen round trips to the SM-DP+ and takes long
	 * enough that a silent UI looks hung. Subscribers of the originating
	 * call get each step as it happens.
	 */
	jobject_put(update, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(update, J_CSTR_TO_JVAL("stage"), jstring_create(step));

	luna_service_post_subscription(req->handle, "/", "downloadProfile",
					update);
	j_release(&update);
}

static bool esim_run_lpac(struct esim_service *service, LSHandle *handle,
			LSMessage *message, const char * const *argv,
			bool with_progress)
{
	struct esim_request *req = esim_request_new(service, handle, message);

	if (!lpac_run(argv, esim_lpac_reply,
			with_progress ? esim_lpac_progress : NULL, req)) {
		luna_service_message_reply_custom_error(handle, message,
				"Could not run lpac");
		esim_request_free(req);
	}

	return true;
}

static bool esim_get_chip_info(LSHandle *handle, LSMessage *message,
							void *user_data)
{
	const char * const argv[] = { "chip", "info", NULL };

	return esim_run_lpac(user_data, handle, message, argv, false);
}

static bool esim_get_profiles(LSHandle *handle, LSMessage *message,
							void *user_data)
{
	const char * const argv[] = { "profile", "list", NULL };

	return esim_run_lpac(user_data, handle, message, argv, false);
}

/* Pulls a required string out of the request, or replies with an error. */
static char *esim_get_string_param(LSHandle *handle, LSMessage *message,
				jvalue_ref parsed, const char *name)
{
	raw_buffer buf;
	jvalue_ref value = jobject_get(parsed, j_cstr_to_buffer(name));

	if (!jis_string(value)) {
		luna_service_message_reply_error_invalid_params(handle, message);
		return NULL;
	}

	buf = jstring_get_fast(value);

	return g_strndup(buf.m_str, buf.m_len);
}

static bool esim_download_profile(LSHandle *handle, LSMessage *message,
							void *user_data)
{
	jvalue_ref parsed = luna_service_message_parse_and_validate(
					LSMessageGetPayload(message));
	char *smdp = NULL;
	char *code = NULL;
	char *confirm = NULL;
	jvalue_ref value;
	bool ret = true;

	if (!jis_valid(parsed)) {
		luna_service_message_reply_error_bad_json(handle, message);
		return true;
	}

	smdp = esim_get_string_param(handle, message, parsed, "smdp");
	if (!smdp)
		goto done;

	code = esim_get_string_param(handle, message, parsed, "activationCode");
	if (!code)
		goto done;

	value = jobject_get(parsed, J_CSTR_TO_BUF("confirmationCode"));
	if (jis_string(value)) {
		raw_buffer buf = jstring_get_fast(value);

		confirm = g_strndup(buf.m_str, buf.m_len);
	}

	if (confirm) {
		const char * const argv[] = { "profile", "download",
				"-s", smdp, "-m", code, "-c", confirm, NULL };

		ret = esim_run_lpac(user_data, handle, message, argv, true);
	} else {
		const char * const argv[] = { "profile", "download",
				"-s", smdp, "-m", code, NULL };

		ret = esim_run_lpac(user_data, handle, message, argv, true);
	}

done:
	g_free(smdp);
	g_free(code);
	g_free(confirm);
	j_release(&parsed);

	return ret;
}

/* enable / disable / delete all take just an iccid */
static bool esim_profile_action(LSHandle *handle, LSMessage *message,
				void *user_data, const char *action)
{
	jvalue_ref parsed = luna_service_message_parse_and_validate(
					LSMessageGetPayload(message));
	char *iccid;
	bool ret;

	if (!jis_valid(parsed)) {
		luna_service_message_reply_error_bad_json(handle, message);
		return true;
	}

	iccid = esim_get_string_param(handle, message, parsed, "iccid");
	if (!iccid) {
		j_release(&parsed);
		return true;
	}

	{
		const char * const argv[] = { "profile", action, iccid, NULL };

		ret = esim_run_lpac(user_data, handle, message, argv, false);
	}

	g_free(iccid);
	j_release(&parsed);

	return ret;
}

static bool esim_enable_profile(LSHandle *handle, LSMessage *message, void *d)
{
	return esim_profile_action(handle, message, d, "enable");
}

static bool esim_disable_profile(LSHandle *handle, LSMessage *message, void *d)
{
	return esim_profile_action(handle, message, d, "disable");
}

static bool esim_delete_profile(LSHandle *handle, LSMessage *message, void *d)
{
	return esim_profile_action(handle, message, d, "delete");
}

static bool esim_set_profile_nickname(LSHandle *handle, LSMessage *message,
							void *user_data)
{
	jvalue_ref parsed = luna_service_message_parse_and_validate(
					LSMessageGetPayload(message));
	char *iccid = NULL;
	char *nickname = NULL;
	bool ret = true;

	if (!jis_valid(parsed)) {
		luna_service_message_reply_error_bad_json(handle, message);
		return true;
	}

	iccid = esim_get_string_param(handle, message, parsed, "iccid");
	if (!iccid)
		goto done;

	nickname = esim_get_string_param(handle, message, parsed, "nickname");
	if (!nickname)
		goto done;

	{
		const char * const argv[] = { "profile", "nickname",
						iccid, nickname, NULL };

		ret = esim_run_lpac(user_data, handle, message, argv, false);
	}

done:
	g_free(iccid);
	g_free(nickname);
	j_release(&parsed);

	return ret;
}

/* --- slot switching ------------------------------------------------------ */

static bool esim_set_active_slot(LSHandle *handle, LSMessage *message,
							void *user_data)
{
	struct esim_service *service = user_data;
	jvalue_ref parsed = luna_service_message_parse_and_validate(
					LSMessageGetPayload(message));
	GError *error = NULL;
	GVariant *reply;
	int32_t slot = 0;

	if (!jis_valid(parsed)) {
		luna_service_message_reply_error_bad_json(handle, message);
		return true;
	}

	if (jnumber_get_i32(jobject_get(parsed, J_CSTR_TO_BUF("slot")),
				&slot) != CONV_OK || slot < 1) {
		luna_service_message_reply_error_invalid_params(handle, message);
		j_release(&parsed);
		return true;
	}

	j_release(&parsed);

	if (!service->modem) {
		luna_service_message_reply_custom_error(handle, message,
				"No modem with eUICC support");
		return true;
	}

	/*
	 * ofono numbers card slots from 1. This is a synchronous call because
	 * ofono only answers once the modem has acted on it, and the reply is
	 * what tells us whether the switch took.
	 */
	reply = g_dbus_connection_call_sync(service->bus, OFONO_SERVICE,
			service->modem, OFONO_SIM_MANAGER_INTERFACE,
			"SetProperty",
			g_variant_new("(sv)", "ActiveCardSlot",
					g_variant_new_uint32(slot)),
			NULL, G_DBUS_CALL_FLAGS_NONE, 60000, NULL, &error);

	if (!reply) {
		/*
		 * Switching back too soon after a previous switch fails: the
		 * card is still being re-read. Worth surfacing verbatim so the
		 * UI can suggest a retry rather than claiming the hardware is
		 * broken.
		 */
		luna_service_message_reply_custom_error(handle, message,
				error ? error->message : "SetProperty failed");
		if (error)
			g_error_free(error);
		return true;
	}

	g_variant_unref(reply);
	luna_service_message_reply_success(handle, message);
	esim_post_status(service);

	return true;
}

/* --- reading an activation code off a QR ---------------------------------- */

static void esim_qr_scan_cb(const char *text, const char *error,
							void *user_data)
{
	struct esim_request *req = user_data;
	jvalue_ref reply = jobject_create();

	if (!text) {
		jobject_put(reply, J_CSTR_TO_JVAL("returnValue"),
				jboolean_create(false));
		jobject_put(reply, J_CSTR_TO_JVAL("errorText"),
				jstring_create(error ? error : "no barcode found"));
		goto send;
	}

	jobject_put(reply, J_CSTR_TO_JVAL("returnValue"), jboolean_create(true));
	jobject_put(reply, J_CSTR_TO_JVAL("text"), jstring_create(text));

	/*
	 * An eSIM QR holds "LPA:1$<smdp>$<matching id>[$<confirmation code>]".
	 * Split it here so the caller does not have to know the format - and
	 * so a QR that is not an activation code is reported as such rather
	 * than silently filling the fields with nonsense.
	 */
	if (!strncmp(text, "LPA:", 4)) {
		char **parts = g_strsplit(text + 4, "$", 4);
		guint n = g_strv_length(parts);

		if (n >= 3) {
			jobject_put(reply, J_CSTR_TO_JVAL("smdp"),
					jstring_create(parts[1]));
			jobject_put(reply, J_CSTR_TO_JVAL("activationCode"),
					jstring_create(parts[2]));

			if (n >= 4 && *parts[3])
				jobject_put(reply,
					J_CSTR_TO_JVAL("confirmationCode"),
					jstring_create(parts[3]));
		}

		g_strfreev(parts);
	}

send:
	luna_service_message_validate_and_send(req->handle, req->message, reply);
	j_release(&reply);
	esim_request_free(req);
}

static bool esim_scan_qr_code(LSHandle *handle, LSMessage *message,
							void *user_data)
{
	struct esim_service *service = user_data;
	jvalue_ref parsed = luna_service_message_parse_and_validate(
					LSMessageGetPayload(message));
	struct esim_request *req;
	char *path;

	if (!jis_valid(parsed)) {
		luna_service_message_reply_error_bad_json(handle, message);
		return true;
	}

	path = esim_get_string_param(handle, message, parsed, "path");
	j_release(&parsed);

	if (!path)
		return true;

	req = esim_request_new(service, handle, message);

	if (!qr_scan_file(path, esim_qr_scan_cb, req)) {
		luna_service_message_reply_custom_error(handle, message,
				"Could not start the QR decoder. Is the zbar "
				"GStreamer element installed?");
		esim_request_free(req);
	}

	g_free(path);

	return true;
}

static LSMethod esim_service_methods[] = {
	{ "getStatus", esim_get_status },
	{ "getChipInfo", esim_get_chip_info },
	{ "getProfiles", esim_get_profiles },
	{ "downloadProfile", esim_download_profile },
	{ "enableProfile", esim_enable_profile },
	{ "disableProfile", esim_disable_profile },
	{ "deleteProfile", esim_delete_profile },
	{ "setProfileNickname", esim_set_profile_nickname },
	{ "setActiveSlot", esim_set_active_slot },
	{ "scanQrCode", esim_scan_qr_code },
	{ NULL, NULL },
};

struct esim_service *esim_service_create(void)
{
	struct esim_service *service;
	LSError error;

	service = g_new0(struct esim_service, 1);

	LSErrorInit(&error);

	if (!LSRegister(ESIM_SERVICE_NAME, &service->handle, &error)) {
		g_critical("Failed to register %s: %s", ESIM_SERVICE_NAME,
				error.message);
		goto error;
	}

	if (!LSGmainAttach(service->handle, event_loop, &error)) {
		g_critical("Failed to attach %s to the main loop: %s",
				ESIM_SERVICE_NAME, error.message);
		goto error;
	}

	if (!LSRegisterCategory(service->handle, "/", esim_service_methods,
				NULL, NULL, &error)) {
		g_critical("Failed to register the service category: %s",
				error.message);
		goto error;
	}

	if (!LSCategorySetData(service->handle, "/", service, &error)) {
		g_critical("Could not set service data: %s", error.message);
		goto error;
	}

	service->bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
	if (!service->bus) {
		g_warning("Could not connect to the system bus");
	} else {
		service->modem = esim_find_modem(service->bus);

		service->prop_watch = g_dbus_connection_signal_subscribe(
				service->bus, OFONO_SERVICE,
				OFONO_SIM_MANAGER_INTERFACE, "PropertyChanged",
				NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
				esim_sim_props_changed, service, NULL);
	}

	return service;

error:
	LSErrorFree(&error);
	g_free(service);

	return NULL;
}

void esim_service_free(struct esim_service *service)
{
	LSError error;

	if (!service)
		return;

	LSErrorInit(&error);

	if (service->bus) {
		if (service->prop_watch)
			g_dbus_connection_signal_unsubscribe(service->bus,
							service->prop_watch);
		g_object_unref(service->bus);
	}

	if (service->handle && !LSUnregister(service->handle, &error)) {
		g_warning("Could not unregister service: %s", error.message);
		LSErrorFree(&error);
	}

	g_free(service->modem);
	g_free(service);
}

// vim:ts=4:sw=4:noexpandtab

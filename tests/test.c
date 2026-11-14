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
 * Drives the adapter's real esim_service.c, lpac_client.c and
 * luna_service_utils.c, unchanged:
 *
 *   - luna-service2 is the stand-in in shim.c, which calls the methods
 *     and records the replies and subscription posts;
 *   - lpac is a shell script on PATH that prints what a test gives it and
 *     notes down how it was run;
 *   - ofono is fake_ofono.c, on a private bus that the adapter takes for
 *     the system bus.
 *
 * qr_scan.c needs GStreamer's zbar element and is replaced by
 * test_qr_scan_file() below, which "decodes" the path it is given: what is
 * tested is how the adapter reads an activation code, not zbar.
 *
 * Run by run.sh under ASan+UBSan, TSan and valgrind.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <pbnjson.h>

#include "esim_service.h"
#include "qr_scan.h"
#include "shim.h"
#include "fake_ofono.h"

#define TEST_ICCID	"8931087626093906051"
#define TEST_TIMEOUT_S	20

GMainLoop *event_loop;	/* esim_service.c's */

static struct test_env {
	GTestDBus *dbus;
	FakeOfono *ofono;
	char *dir;		/* The fake lpac's, and its notes */
	char *bin;		/* On PATH, holding lpac */
} env;

/* --- the stand-in for qr_scan.c ------------------------------------------- */

struct test_qr {
	char *text;
	qr_scan_cb cb;
	void *user_data;
};

static gboolean test_qr_done(gpointer data)
{
	struct test_qr *qr = data;

	if (g_str_has_prefix(qr->text, "error:"))
		qr->cb(NULL, qr->text + 6, qr->user_data);
	else
		qr->cb(qr->text, NULL, qr->user_data);
	g_free(qr->text);
	g_free(qr);
	return G_SOURCE_REMOVE;
}

/*
 * The path is what the QR says. "nopipeline" fails as a missing zbar
 * element would, "error:<text>" as a frame with no barcode.
 */
bool qr_scan_file(const char *path, qr_scan_cb cb, void *user_data)
{
	struct test_qr *qr;

	if (!strcmp(path, "nopipeline"))
		return false;

	qr = g_new0(struct test_qr, 1);
	qr->text = g_strdup(path);
	qr->cb = cb;
	qr->user_data = user_data;
	g_idle_add(test_qr_done, qr);
	return true;
}

/* --- the fake lpac -------------------------------------------------------- */

/*
 * Notes "<LPAC_APDU>|<args>" in calls, and "overlap" in overlap if
 * another lpac was running. The running directory exists while it runs.
 */
static const char test_lpac_script[] =
	"#!/bin/sh\n"
	"d=\"$LPAC_TEST_DIR\"\n"
	"mkdir \"$d/running\" 2>/dev/null || echo overlap >> \"$d/overlap\"\n"
	"printf '%s|%s\\n' \"$LPAC_APDU\" \"$*\" >> \"$d/calls\"\n"
	"[ -f \"$d/sleep\" ] && sleep \"$(cat \"$d/sleep\")\"\n"
	"[ -f \"$d/out\" ] && cat \"$d/out\"\n"
	"rmdir \"$d/running\" 2>/dev/null\n"
	"exit 0\n";

static char *test_file(const char *name)
{
	return g_build_filename(env.dir, name, NULL);
}

static void test_write(const char *name, const char *contents)
{
	char *path = test_file(name);

	g_assert(g_file_set_contents(path, contents, -1, NULL));
	g_free(path);
}

static void test_remove(const char *name)
{
	char *path = test_file(name);

	g_unlink(path);
	g_free(path);
}

/* What lpac will print: progress lines, then its verdict */
static void test_lpac_output(const char * const *steps, int code,
			const char *message, const char *data)
{
	GString *out = g_string_new(NULL);

	while (steps && *steps) {
		g_string_append_printf(out, "{\"type\":\"progress\",\"payload\":"
			"{\"code\":0,\"message\":\"%s\",\"data\":null}}\n",
			*steps);
		steps++;
	}
	g_string_append_printf(out, "{\"type\":\"lpa\",\"payload\":{\"code\":%d,"
		"\"message\":\"%s\",\"data\":%s}}\n", code, message,
		data ? data : "null");
	test_write("out", out->str);
	g_string_free(out, TRUE);
}

static void test_lpac_ok(const char *data)
{
	test_lpac_output(NULL, 0, "success", data);
}

/* The lpac runs so far, one "<LPAC_APDU>|<args>" each */
static char **test_lpac_calls(void)
{
	char *path = test_file("calls");
	char *contents = NULL;
	char **lines;

	if (!g_file_get_contents(path, &contents, NULL, NULL))
		contents = g_strdup("");
	g_free(path);
	g_strchomp(contents);
	lines = *contents ? g_strsplit(contents, "\n", -1) : g_new0(char *, 1);
	g_free(contents);
	return lines;
}

static guint test_lpac_runs(void)
{
	char **calls = test_lpac_calls();
	guint n = g_strv_length(calls);

	g_strfreev(calls);
	return n;
}

static gboolean test_lpac_overlapped(void)
{
	char *path = test_file("overlap");
	gboolean overlap = g_file_test(path, G_FILE_TEST_EXISTS);

	g_free(path);
	return overlap;
}

/*
 * Puts lpac on PATH or takes it off. Renamed rather than PATH changed:
 * setenv() isn't safe with GDBus's threads running.
 */
static void test_lpac_install(gboolean installed)
{
	char *on = g_build_filename(env.bin, "lpac", NULL);
	char *off = g_build_filename(env.bin, "lpac.off", NULL);

	if (installed && g_file_test(off, G_FILE_TEST_EXISTS))
		g_assert_cmpint(g_rename(off, on), ==, 0);
	else if (!installed)
		g_assert_cmpint(g_rename(on, off), ==, 0);
	g_free(on);
	g_free(off);
}

/* --- waiting -------------------------------------------------------------- */

typedef gboolean (*test_cond)(gconstpointer data);

static gboolean test_tick(gpointer data)
{
	return G_SOURCE_CONTINUE;
}

/* Runs the main loop until cond holds; fails the test after a while */
static void test_wait(test_cond cond, gconstpointer data)
{
	gint64 deadline = g_get_monotonic_time() +
				TEST_TIMEOUT_S * G_TIME_SPAN_SECOND;
	guint tick = g_timeout_add(10, test_tick, NULL);

	while (!cond(data)) {
		g_assert_cmpint(g_get_monotonic_time(), <, deadline);
		g_main_context_iteration(NULL, TRUE);
	}
	g_source_remove(tick);
}

/* Runs the main loop for a while, for things that must not happen */
static void test_settle(void)
{
	gint64 until = g_get_monotonic_time() + 200 * G_TIME_SPAN_MILLISECOND;
	guint tick = g_timeout_add(10, test_tick, NULL);

	while (g_get_monotonic_time() < until)
		g_main_context_iteration(NULL, TRUE);
	g_source_remove(tick);
}

static gboolean test_has_reply(gconstpointer message)
{
	return shim_replies((LSMessage *) message)->len > 0;
}

/* Calls method and waits for its reply, which must be the only one */
static char *test_call(const char *method, const char *payload)
{
	LSMessage *message = shim_call(method, payload);
	const GPtrArray *replies;
	char *reply;

	test_wait(test_has_reply, message);
	test_settle();
	replies = shim_replies(message);
	g_assert_cmpuint(replies->len, ==, 1);
	reply = g_strdup(g_ptr_array_index(replies, 0));
	LSMessageUnref(message);
	return reply;
}

/* --- reading a reply ------------------------------------------------------ */

static jvalue_ref test_parse(const char *json)
{
	jvalue_ref value = jdom_create(j_cstr_to_buffer(json), jschema_all(),
					NULL);

	g_assert(jis_object(value));
	return value;
}

static gboolean test_has(jvalue_ref obj, const char *key)
{
	return jis_valid(jobject_get(obj, j_cstr_to_buffer(key)));
}

static gboolean test_bool(jvalue_ref obj, const char *key)
{
	bool value = false;

	g_assert(test_has(obj, key));
	g_assert_cmpint(jboolean_get(jobject_get(obj, j_cstr_to_buffer(key)),
					&value), ==, CONV_OK);
	return value;
}

static int test_int(jvalue_ref obj, const char *key)
{
	int32_t value = 0;

	g_assert(test_has(obj, key));
	g_assert_cmpint(jnumber_get_i32(jobject_get(obj, j_cstr_to_buffer(key)),
					&value), ==, CONV_OK);
	return value;
}

/* The string, or NULL if there is none; to be freed */
static char *test_string(jvalue_ref obj, const char *key)
{
	jvalue_ref value = jobject_get(obj, j_cstr_to_buffer(key));
	raw_buffer buf;

	if (!jis_string(value))
		return NULL;
	buf = jstring_get_fast(value);
	return g_strndup(buf.m_str, buf.m_len);
}

static void test_assert_string(jvalue_ref obj, const char *key,
				const char *expected)
{
	char *value = test_string(obj, key);

	g_assert_cmpstr(value, ==, expected);
	g_free(value);
}

/*
 * g_assert() rather than g_assert_nonnull() where a pointer is used next:
 * only g_assert() never returns, even with non-fatal assertions.
 */

/* An error reply with this text, from the adapter itself */
static void test_assert_error(const char *reply, const char *text)
{
	jvalue_ref obj = test_parse(reply);

	g_assert(!test_bool(obj, "returnValue"));
	test_assert_string(obj, "errorText", text);
	j_release(&obj);
}

/* --- fixture -------------------------------------------------------------- */

typedef struct test_fixture {
	struct esim_service *service;
} TestFixture;

static void test_setup(TestFixture *f, gconstpointer data)
{
	shim_reset();
	fake_ofono_reset(env.ofono);
	test_remove("out");
	test_remove("calls");
	test_remove("overlap");
	test_remove("sleep");
	test_lpac_install(TRUE);

	f->service = esim_service_create();
	g_assert(f->service);
	g_assert(shim_registered());
}

static void test_teardown(TestFixture *f, gconstpointer data)
{
	esim_service_free(f->service);
	test_lpac_install(TRUE);
	g_assert(!shim_registered());
	g_assert_cmpuint(shim_live_messages(), ==, 0);
}

/* --- getStatus ------------------------------------------------------------ */

static void test_status(TestFixture *f, gconstpointer data)
{
	char *reply = test_call("getStatus", "{}");
	jvalue_ref obj = test_parse(reply);

	g_assert(test_bool(obj, "returnValue"));
	g_assert(test_bool(obj, "available"));
	test_assert_string(obj, "modemPath", FAKE_OFONO_MODEM);
	g_assert(test_bool(obj, "simPresent"));
	g_assert_cmpint(test_int(obj, "slotCount"), ==, 2);
	g_assert_cmpint(test_int(obj, "activeSlot"), ==, 2);
	test_assert_string(obj, "iccid", TEST_ICCID);
	g_assert(!test_bool(obj, "subscribed"));
	j_release(&obj);
	g_free(reply);
}

/*
 * ofono leaves CardSlotCount out with no card; the slots are counted from
 * the modems instead, and the active one is left unknown.
 */
static void test_status_slot_fallback(TestFixture *f, gconstpointer data)
{
	char *reply;
	jvalue_ref obj;

	fake_ofono_set_slots(env.ofono, 0, 0);
	reply = test_call("getStatus", "{\"subscribe\":true}");
	obj = test_parse(reply);
	g_assert_cmpint(test_int(obj, "slotCount"), ==, 2);
	g_assert(!test_has(obj, "activeSlot"));
	g_assert(test_bool(obj, "subscribed"));
	j_release(&obj);
	g_free(reply);
}

/* No modem with EuiccManager: no eSIM here */
static void test_status_no_euicc(TestFixture *f, gconstpointer data)
{
	char *reply;
	jvalue_ref obj;

	fake_ofono_set_euicc(env.ofono, FALSE);
	reply = test_call("getStatus", "{}");
	obj = test_parse(reply);
	g_assert(test_bool(obj, "returnValue"));
	g_assert(!test_bool(obj, "available"));
	g_assert(!test_has(obj, "modemPath"));
	j_release(&obj);
	g_free(reply);
}

/*
 * A SIM property change reaches getStatus subscribers. GDBus matches the
 * signal's sender against org.ofono's owner only once it has looked the
 * owner up, which the adapter doesn't wait for; a real ofono's signals
 * come long after. So the signal is sent until one gets through.
 */
static void test_status_changed(TestFixture *f, gconstpointer data)
{
	gint64 deadline = g_get_monotonic_time() +
				TEST_TIMEOUT_S * G_TIME_SPAN_SECOND;

	while (!shim_posts_for("getStatus")) {
		g_assert_cmpint(g_get_monotonic_time(), <, deadline);
		fake_ofono_emit_property_changed(env.ofono);
		test_settle();
	}
}

/* --- lpac-backed methods -------------------------------------------------- */

/* lpac runs with LPAC_APDU=ofono, and its data comes back as the result */
static void test_profiles(TestFixture *f, gconstpointer data)
{
	char *reply;
	const char *result;
	char **calls;
	jvalue_ref obj;

	test_lpac_ok("[{\"iccid\":\"" TEST_ICCID "\",\"profileState\":"
			"\"enabled\"}]");
	reply = test_call("getProfiles", "{}");
	obj = test_parse(reply);
	g_assert(test_bool(obj, "returnValue"));
	g_assert(jis_array(jobject_get(obj, J_CSTR_TO_BUF("result"))));
	result = jvalue_stringify(jobject_get(obj, J_CSTR_TO_BUF("result")));
	g_assert(result);
	g_assert_nonnull(strstr(result, TEST_ICCID));

	calls = test_lpac_calls();
	g_assert_cmpuint(g_strv_length(calls), ==, 1);
	g_assert_cmpstr(calls[0], ==, "ofono|profile list");
	g_strfreev(calls);
	g_assert_cmpuint(fake_ofono_reset_calls(env.ofono), ==, 0);

	j_release(&obj);
	g_free(reply);
}

/* lpac's error comes back with its code, message and data */
static void test_lpac_error(TestFixture *f, gconstpointer data)
{
	char *reply;
	jvalue_ref obj;

	test_lpac_output(NULL, -1, "euicc_init", "\"SCardTransmit\"");
	reply = test_call("getChipInfo", "{}");
	obj = test_parse(reply);
	g_assert(!test_bool(obj, "returnValue"));
	g_assert_cmpint(test_int(obj, "errorCode"), ==, -1);
	test_assert_string(obj, "errorText", "euicc_init");
	test_assert_string(obj, "errorDetail", "SCardTransmit");
	j_release(&obj);
	g_free(reply);
}

/* lpac printed nothing useful */
static void test_lpac_no_result(TestFixture *f, gconstpointer data)
{
	char *reply;
	jvalue_ref obj;

	test_write("out", "not json\n");
	reply = test_call("getChipInfo", "{}");
	obj = test_parse(reply);
	g_assert(!test_bool(obj, "returnValue"));
	g_assert_cmpint(test_int(obj, "errorCode"), ==, -1);
	test_assert_string(obj, "errorText", "lpac produced no result");
	j_release(&obj);
	g_free(reply);
}

/*
 * No lpac at all: one error reply, whatever the method. The request is
 * answered and freed once, not by both the lpac client and the method.
 */
static void test_lpac_missing(TestFixture *f, gconstpointer method)
{
	char *reply;

	test_lpac_install(FALSE);
	reply = test_call(method, "{\"iccid\":\"" TEST_ICCID "\"}");
	test_assert_error(reply, "Could not run lpac");
	g_assert_cmpuint(fake_ofono_reset_calls(env.ofono), ==, 0);
	g_free(reply);
}

/* One lpac at a time: the eUICC has room for one LPA session */
static void test_lpac_queue(TestFixture *f, gconstpointer data)
{
	LSMessage *m[3];
	guint i;

	test_lpac_ok("[]");
	test_write("sleep", "0.2");
	m[0] = shim_call("getProfiles", "{}");
	m[1] = shim_call("getChipInfo", "{}");
	m[2] = shim_call("enableProfile", "{\"iccid\":\"" TEST_ICCID "\"}");

	for (i = 0; i < G_N_ELEMENTS(m); i++) {
		test_wait(test_has_reply, m[i]);
		g_assert_cmpuint(shim_replies(m[i])->len, ==, 1);
		LSMessageUnref(m[i]);
	}
	g_assert_cmpuint(test_lpac_runs(), ==, 3);
	g_assert(!test_lpac_overlapped());
	g_assert_cmpuint(fake_ofono_reset_calls(env.ofono), ==, 1);
	g_assert_cmpuint(fake_ofono_reset_calls_while_busy(env.ofono), ==, 0);
}

/* A download reports its steps to subscribers, and passes every code */
static void test_download(TestFixture *f, gconstpointer data)
{
	static const char * const steps[] = {
		"es9p_initiate_authentication",
		"es10b_load_bound_profile_package",
		NULL
	};
	char *reply;
	char **calls;
	const GPtrArray *posts;
	jvalue_ref obj;

	test_lpac_output(steps, 0, "success", NULL);
	reply = test_call("downloadProfile", "{\"smdp\":\"smdp.example.com\","
		"\"activationCode\":\"ABC-123\",\"confirmationCode\":\"0000\","
		"\"subscribe\":true}");
	obj = test_parse(reply);
	g_assert(test_bool(obj, "returnValue"));
	j_release(&obj);
	g_free(reply);

	posts = shim_posts();
	g_assert_cmpuint(shim_posts_for("downloadProfile"), ==, 2);
	obj = test_parse(((ShimPost *) g_ptr_array_index(posts, 1))->payload);
	test_assert_string(obj, "stage", "es10b_load_bound_profile_package");
	j_release(&obj);

	calls = test_lpac_calls();
	g_assert_cmpstr(calls[0], ==, "ofono|profile download -s "
			"smdp.example.com -m ABC-123 -c 0000");
	g_strfreev(calls);
}

static void test_download_no_confirmation(TestFixture *f, gconstpointer data)
{
	char *reply;
	char **calls;

	test_lpac_ok(NULL);
	reply = test_call("downloadProfile", "{\"smdp\":\"smdp.example.com\","
			"\"activationCode\":\"ABC-123\"}");
	g_free(reply);
	calls = test_lpac_calls();
	g_assert_cmpstr(calls[0], ==, "ofono|profile download -s "
			"smdp.example.com -m ABC-123");
	g_strfreev(calls);
}

static void test_download_bad_params(TestFixture *f, gconstpointer data)
{
	char *reply = test_call("downloadProfile",
				"{\"smdp\":\"smdp.example.com\"}");

	test_assert_error(reply, "Invalid parameters.");
	g_free(reply);
	reply = test_call("downloadProfile", "{\"smdp\":");
	test_assert_error(reply, "Malformed json.");
	g_free(reply);
	g_assert_cmpuint(test_lpac_runs(), ==, 0);
}

static void test_nickname(TestFixture *f, gconstpointer data)
{
	char *reply;
	char **calls;

	test_lpac_ok(NULL);
	reply = test_call("setProfileNickname", "{\"iccid\":\"" TEST_ICCID
				"\",\"nickname\":\"Travel\"}");
	g_free(reply);
	calls = test_lpac_calls();
	g_assert_cmpstr(calls[0], ==, "ofono|profile nickname " TEST_ICCID
			" Travel");
	g_strfreev(calls);

	reply = test_call("setProfileNickname", "{\"iccid\":\"" TEST_ICCID
				"\"}");
	test_assert_error(reply, "Invalid parameters.");
	g_free(reply);
	g_assert_cmpuint(test_lpac_runs(), ==, 1);
}

/* --- enable / disable / delete -------------------------------------------- */

/*
 * The card is reset once lpac is done with it, and the reply waits for
 * that. Subscribers to getStatus hear about the card coming back.
 */
static void test_switch(TestFixture *f, gconstpointer action)
{
	char *method = g_strdup_printf("%sProfile", (const char *) action);
	char *expected = g_strdup_printf("ofono|profile %s " TEST_ICCID,
					(const char *) action);
	char *busy = test_file("running");
	char *reply;
	char **calls;
	jvalue_ref obj;

	test_lpac_ok(NULL);
	fake_ofono_set_busy_file(env.ofono, busy);
	reply = test_call(method, "{\"iccid\":\"" TEST_ICCID "\"}");
	obj = test_parse(reply);
	g_assert(test_bool(obj, "returnValue"));
	g_assert(test_bool(obj, "cardReset"));
	g_assert(!test_has(obj, "cardResetError"));

	calls = test_lpac_calls();
	g_assert_cmpuint(g_strv_length(calls), ==, 1);
	g_assert_cmpstr(calls[0], ==, expected);
	g_strfreev(calls);

	g_assert_cmpuint(fake_ofono_reset_calls(env.ofono), ==, 1);
	g_assert_cmpuint(fake_ofono_reset_calls_while_busy(env.ofono), ==, 0);
	g_assert_cmpuint(shim_posts_for("getStatus"), >=, 1);

	j_release(&obj);
	g_free(reply);
	g_free(busy);
	g_free(expected);
	g_free(method);
}

/*
 * ofono can't reset the card. The profile did switch, so the call
 * succeeds, and says the reset didn't happen and why.
 */
static void test_switch_reset_fails(TestFixture *f, gconstpointer data)
{
	char *reply, *error;
	jvalue_ref obj;

	test_lpac_ok(NULL);
	fake_ofono_set_reset_error(env.ofono, "org.ofono.Error.NotImplemented");
	reply = test_call("enableProfile", "{\"iccid\":\"" TEST_ICCID "\"}");
	obj = test_parse(reply);
	g_assert(test_bool(obj, "returnValue"));
	g_assert(!test_bool(obj, "cardReset"));
	error = test_string(obj, "cardResetError");
	g_assert(error);
	g_assert_nonnull(strstr(error, "NotImplemented"));
	g_assert_cmpuint(fake_ofono_reset_calls(env.ofono), ==, 1);

	g_free(error);
	j_release(&obj);
	g_free(reply);
}

/* lpac failed: nothing changed on the card, so nothing is reset */
static void test_switch_lpac_fails(TestFixture *f, gconstpointer data)
{
	char *reply;
	jvalue_ref obj;

	test_lpac_output(NULL, -1, "es10c_enable_profile", "\"profileNotInDisabledState\"");
	reply = test_call("enableProfile", "{\"iccid\":\"" TEST_ICCID "\"}");
	obj = test_parse(reply);
	g_assert(!test_bool(obj, "returnValue"));
	test_assert_string(obj, "errorText", "es10c_enable_profile");
	test_assert_string(obj, "errorDetail", "profileNotInDisabledState");
	g_assert(!test_has(obj, "cardReset"));
	g_assert_cmpuint(fake_ofono_reset_calls(env.ofono), ==, 0);
	j_release(&obj);
	g_free(reply);
}

/* No modem to reset the card on */
static void test_switch_no_modem(TestFixture *f, gconstpointer data)
{
	char *reply;
	jvalue_ref obj;

	esim_service_free(f->service);
	fake_ofono_set_euicc(env.ofono, FALSE);
	f->service = esim_service_create();

	test_lpac_ok(NULL);
	reply = test_call("disableProfile", "{\"iccid\":\"" TEST_ICCID "\"}");
	obj = test_parse(reply);
	g_assert(test_bool(obj, "returnValue"));
	g_assert(!test_bool(obj, "cardReset"));
	test_assert_string(obj, "cardResetError",
				"No modem with eUICC support");
	g_assert_cmpuint(test_lpac_runs(), ==, 1);
	g_assert_cmpuint(fake_ofono_reset_calls(env.ofono), ==, 0);
	j_release(&obj);
	g_free(reply);
}

/* Deleting a profile switches nothing: no reset */
static void test_delete(TestFixture *f, gconstpointer data)
{
	char *reply;
	char **calls;
	jvalue_ref obj;

	test_lpac_ok(NULL);
	reply = test_call("deleteProfile", "{\"iccid\":\"" TEST_ICCID "\"}");
	obj = test_parse(reply);
	g_assert(test_bool(obj, "returnValue"));
	g_assert(!test_has(obj, "cardReset"));
	calls = test_lpac_calls();
	g_assert_cmpstr(calls[0], ==, "ofono|profile delete " TEST_ICCID);
	g_strfreev(calls);
	g_assert_cmpuint(fake_ofono_reset_calls(env.ofono), ==, 0);
	j_release(&obj);
	g_free(reply);
}

static void test_profile_bad_params(TestFixture *f, gconstpointer method)
{
	char *reply = test_call(method, "{}");

	test_assert_error(reply, "Invalid parameters.");
	g_free(reply);
	reply = test_call(method, "{\"iccid\":42}");
	test_assert_error(reply, "Invalid parameters.");
	g_free(reply);
	reply = test_call(method, "[");
	test_assert_error(reply, "Malformed json.");
	g_free(reply);
	g_assert_cmpuint(test_lpac_runs(), ==, 0);
	g_assert_cmpuint(fake_ofono_reset_calls(env.ofono), ==, 0);
}

/* --- setActiveSlot -------------------------------------------------------- */

static void test_slot(TestFixture *f, gconstpointer data)
{
	char *reply = test_call("setActiveSlot", "{\"slot\":1}");
	jvalue_ref obj = test_parse(reply);

	g_assert(test_bool(obj, "returnValue"));
	g_assert_cmpuint(fake_ofono_slot_calls(env.ofono), ==, 1);
	g_assert_cmpuint(fake_ofono_last_slot(env.ofono), ==, 1);
	g_assert_cmpuint(shim_posts_for("getStatus"), ==, 1);
	j_release(&obj);
	g_free(reply);
}

/* ofono's own error is passed on as it is */
static void test_slot_fails(TestFixture *f, gconstpointer data)
{
	char *reply, *text;
	jvalue_ref obj;

	fake_ofono_set_slot_error(env.ofono, "org.ofono.Error.Busy");
	reply = test_call("setActiveSlot", "{\"slot\":2}");
	obj = test_parse(reply);
	g_assert(!test_bool(obj, "returnValue"));
	text = test_string(obj, "errorText");
	g_assert(text);
	g_assert_nonnull(strstr(text, "org.ofono.Error.Busy"));
	g_assert_cmpuint(shim_posts_for("getStatus"), ==, 0);
	g_free(text);
	j_release(&obj);
	g_free(reply);
}

static void test_slot_bad_params(TestFixture *f, gconstpointer data)
{
	static const char * const payloads[] = {
		"{}", "{\"slot\":0}", "{\"slot\":-1}", "{\"slot\":\"1\"}"
	};
	guint i;
	char *reply;

	for (i = 0; i < G_N_ELEMENTS(payloads); i++) {
		reply = test_call("setActiveSlot", payloads[i]);
		test_assert_error(reply, "Invalid parameters.");
		g_free(reply);
	}
	g_assert_cmpuint(fake_ofono_slot_calls(env.ofono), ==, 0);
}

static void test_slot_no_modem(TestFixture *f, gconstpointer data)
{
	char *reply;

	esim_service_free(f->service);
	fake_ofono_set_euicc(env.ofono, FALSE);
	f->service = esim_service_create();
	reply = test_call("setActiveSlot", "{\"slot\":1}");
	test_assert_error(reply, "No modem with eUICC support");
	g_assert_cmpuint(fake_ofono_slot_calls(env.ofono), ==, 0);
	g_free(reply);
}

/* --- scanQrCode ----------------------------------------------------------- */

/* An activation code is split into its parts */
static void test_qr(TestFixture *f, gconstpointer data)
{
	char *reply = test_call("scanQrCode",
			"{\"path\":\"LPA:1$smdp.example.com$ABC-123$0000\"}");
	jvalue_ref obj = test_parse(reply);

	g_assert(test_bool(obj, "returnValue"));
	test_assert_string(obj, "smdp", "smdp.example.com");
	test_assert_string(obj, "activationCode", "ABC-123");
	test_assert_string(obj, "confirmationCode", "0000");
	j_release(&obj);
	g_free(reply);

	reply = test_call("scanQrCode",
			"{\"path\":\"LPA:1$smdp.example.com$ABC-123$\"}");
	obj = test_parse(reply);
	test_assert_string(obj, "activationCode", "ABC-123");
	g_assert(!test_has(obj, "confirmationCode"));
	j_release(&obj);
	g_free(reply);
}

/* Any other QR is passed on, without made-up activation fields */
static void test_qr_other(TestFixture *f, gconstpointer data)
{
	static const char * const payloads[] = {
		"{\"path\":\"https://example.com\"}",
		"{\"path\":\"LPA:1$only-one\"}",
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(payloads); i++) {
		char *reply = test_call("scanQrCode", payloads[i]);
		jvalue_ref obj = test_parse(reply);

		g_assert(test_bool(obj, "returnValue"));
		g_assert(test_has(obj, "text"));
		g_assert(!test_has(obj, "smdp"));
		g_assert(!test_has(obj, "activationCode"));
		j_release(&obj);
		g_free(reply);
	}
}

static void test_qr_errors(TestFixture *f, gconstpointer data)
{
	char *reply = test_call("scanQrCode", "{\"path\":\"error:no barcode\"}");

	test_assert_error(reply, "no barcode");
	g_free(reply);
	reply = test_call("scanQrCode", "{\"path\":\"nopipeline\"}");
	test_assert_error(reply, "Could not start the QR decoder. Is the zbar "
				"GStreamer element installed?");
	g_free(reply);
	reply = test_call("scanQrCode", "{}");
	test_assert_error(reply, "Invalid parameters.");
	g_free(reply);
}

/* --- main ----------------------------------------------------------------- */

#define TEST_ADD(name, fn, data) \
	g_test_add("/esim/" name, TestFixture, data, test_setup, fn, \
			test_teardown)

int main(int argc, char *argv[])
{
	char *lpac;
	int ret;

	g_test_init(&argc, &argv, NULL);
	/*
	 * The adapter warns on the failures tests cause on purpose (no lpac,
	 * ofono refusing a reset); criticals, glib's own included, still fail.
	 */
	g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

	/* The fake lpac, first on PATH */
	env.dir = g_dir_make_tmp("esim-test-XXXXXX", NULL);
	g_assert(env.dir);
	env.bin = g_build_filename(env.dir, "bin", NULL);
	g_assert_cmpint(g_mkdir(env.bin, 0700), ==, 0);
	lpac = g_build_filename(env.bin, "lpac", NULL);
	g_assert(g_file_set_contents(lpac, test_lpac_script, -1, NULL));
	g_assert_cmpint(g_chmod(lpac, 0700), ==, 0);
	g_free(lpac);
	/* The environment is set up before any thread is started */
	{
		char *path = g_strdup_printf("%s:%s", env.bin,
						g_getenv("PATH"));

		g_setenv("PATH", path, TRUE);
		g_free(path);
	}
	g_setenv("LPAC_TEST_DIR", env.dir, TRUE);

	/* A private bus, which the adapter takes for the system bus */
	env.dbus = g_test_dbus_new(G_TEST_DBUS_NONE);
	g_test_dbus_up(env.dbus);
	g_setenv("DBUS_SYSTEM_BUS_ADDRESS",
			g_test_dbus_get_bus_address(env.dbus), TRUE);
	env.ofono = fake_ofono_start(g_test_dbus_get_bus_address(env.dbus));

	event_loop = g_main_loop_new(NULL, FALSE);

	TEST_ADD("status", test_status, NULL);
	TEST_ADD("status/slot_fallback", test_status_slot_fallback, NULL);
	TEST_ADD("status/no_euicc", test_status_no_euicc, NULL);
	TEST_ADD("status/changed", test_status_changed, NULL);
	TEST_ADD("lpac/profiles", test_profiles, NULL);
	TEST_ADD("lpac/error", test_lpac_error, NULL);
	TEST_ADD("lpac/no_result", test_lpac_no_result, NULL);
	TEST_ADD("lpac/missing/getProfiles", test_lpac_missing, "getProfiles");
	TEST_ADD("lpac/missing/enableProfile", test_lpac_missing,
			"enableProfile");
	TEST_ADD("lpac/missing/deleteProfile", test_lpac_missing,
			"deleteProfile");
	TEST_ADD("lpac/queue", test_lpac_queue, NULL);
	TEST_ADD("download", test_download, NULL);
	TEST_ADD("download/no_confirmation", test_download_no_confirmation,
			NULL);
	TEST_ADD("download/bad_params", test_download_bad_params, NULL);
	TEST_ADD("nickname", test_nickname, NULL);
	TEST_ADD("switch/enable", test_switch, "enable");
	TEST_ADD("switch/disable", test_switch, "disable");
	TEST_ADD("switch/reset_fails", test_switch_reset_fails, NULL);
	TEST_ADD("switch/lpac_fails", test_switch_lpac_fails, NULL);
	TEST_ADD("switch/no_modem", test_switch_no_modem, NULL);
	TEST_ADD("delete", test_delete, NULL);
	TEST_ADD("profile/bad_params/enableProfile", test_profile_bad_params,
			"enableProfile");
	TEST_ADD("profile/bad_params/deleteProfile", test_profile_bad_params,
			"deleteProfile");
	TEST_ADD("slot", test_slot, NULL);
	TEST_ADD("slot/fails", test_slot_fails, NULL);
	TEST_ADD("slot/bad_params", test_slot_bad_params, NULL);
	TEST_ADD("slot/no_modem", test_slot_no_modem, NULL);
	TEST_ADD("qr", test_qr, NULL);
	TEST_ADD("qr/other", test_qr_other, NULL);
	TEST_ADD("qr/errors", test_qr_errors, NULL);

	ret = g_test_run();

	g_main_loop_unref(event_loop);
	fake_ofono_stop(env.ofono);
	g_test_dbus_down(env.dbus);
	g_object_unref(env.dbus);
	shim_reset();
	{
		char *bin_lpac = g_build_filename(env.bin, "lpac", NULL);

		g_unlink(bin_lpac);
		g_free(bin_lpac);
	}
	g_rmdir(env.bin);
	test_remove("out");
	test_remove("calls");
	test_remove("overlap");
	test_remove("sleep");
	g_rmdir(env.dir);
	g_free(env.bin);
	g_free(env.dir);

	return ret;
}

// vim:ts=4:sw=4:noexpandtab

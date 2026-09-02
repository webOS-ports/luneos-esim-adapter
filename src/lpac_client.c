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

#include <string.h>
#include <stdlib.h>

#include "lpac_client.h"

#define LPAC_BINARY	"lpac"

/*
 * The eUICC allows one LPA session at a time: a second lpac cannot open a
 * logical channel while the first holds one, and comes back with euicc_init.
 * Nothing stops two clients - or one page asking for the chip info and the
 * profile list at once - from doing exactly that, so runs are queued here
 * rather than left to collide.
 */
static GQueue lpac_pending = G_QUEUE_INIT;
static struct lpac_call *lpac_current;

static gboolean lpac_start(struct lpac_call *call);
static void lpac_start_next(void);

struct lpac_call {
	gchar **spawn_argv;	/* owned; ready to hand to g_spawn */
	lpac_result_cb cb;
	lpac_progress_cb progress_cb;
	void *user_data;
	GIOChannel *out;
	guint out_watch;
	GPid pid;
	gboolean reaped;
	gboolean drained;
	GString *tail;		/* last {"type":"lpa"} line seen */
};

static void lpac_call_finish(struct lpac_call *call)
{
	jvalue_ref parsed = jinvalid();
	jvalue_ref payload;
	int code = -1;
	const char *message = "lpac produced no result";
	raw_buffer buf;

	/*
	 * Only finish once both the pipe has closed and the child has been
	 * reaped, otherwise the tail of the output can be lost.
	 */
	if (!call->reaped || !call->drained)
		return;

	if (call->tail->len > 0) {
		parsed = jdom_create(j_cstr_to_buffer(call->tail->str),
					jschema_all(), NULL);

		if (jis_valid(parsed)) {
			payload = jobject_get(parsed, J_CSTR_TO_BUF("payload"));

			if (jis_valid(payload)) {
				jnumber_get_i32(jobject_get(payload,
					J_CSTR_TO_BUF("code")), &code);

				buf = jstring_get_fast(jobject_get(payload,
					J_CSTR_TO_BUF("message")));
				if (buf.m_str)
					message = buf.m_str;

				call->cb(code, message,
					jobject_get(payload,
						J_CSTR_TO_BUF("data")),
					call->user_data);
				goto done;
			}
		}
	}

	call->cb(code, message, jinvalid(), call->user_data);

done:
	if (jis_valid(parsed))
		j_release(&parsed);

	g_string_free(call->tail, TRUE);
	g_strfreev(call->spawn_argv);

	if (lpac_current == call)
		lpac_current = NULL;

	g_free(call);

	lpac_start_next();
}

static void lpac_handle_line(struct lpac_call *call, const char *line)
{
	jvalue_ref parsed;
	jvalue_ref payload;
	raw_buffer type;
	raw_buffer msg;

	parsed = jdom_create(j_cstr_to_buffer(line), jschema_all(), NULL);
	if (!jis_valid(parsed))
		return;

	type = jstring_get_fast(jobject_get(parsed, J_CSTR_TO_BUF("type")));

	if (type.m_str && !strncmp(type.m_str, "lpa", type.m_len)) {
		/* The final verdict. Keep it for lpac_call_finish(). */
		g_string_assign(call->tail, line);
	} else if (type.m_str && !strncmp(type.m_str, "progress", type.m_len) &&
			call->progress_cb) {
		payload = jobject_get(parsed, J_CSTR_TO_BUF("payload"));
		msg = jstring_get_fast(jobject_get(payload,
						J_CSTR_TO_BUF("message")));
		if (msg.m_str) {
			char *step = g_strndup(msg.m_str, msg.m_len);

			call->progress_cb(step, call->user_data);
			g_free(step);
		}
	}

	j_release(&parsed);
}

static gboolean lpac_output_cb(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct lpac_call *call = user_data;
	char *line = NULL;
	gsize len = 0;

	if (cond & (G_IO_IN | G_IO_PRI)) {
		while (g_io_channel_read_line(channel, &line, &len, NULL, NULL) ==
				G_IO_STATUS_NORMAL && line) {
			g_strchomp(line);
			if (*line)
				lpac_handle_line(call, line);
			g_free(line);
			line = NULL;
		}
	}

	if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
		call->drained = TRUE;
		call->out_watch = 0;
		lpac_call_finish(call);
		return FALSE;
	}

	return TRUE;
}

static void lpac_child_cb(GPid pid, gint status, gpointer user_data)
{
	struct lpac_call *call = user_data;

	g_spawn_close_pid(pid);
	call->reaped = TRUE;
	lpac_call_finish(call);
}

/* Spawns call; on failure reports it to the caller and frees call. */
static gboolean lpac_start(struct lpac_call *call)
{
	GError *error = NULL;
	gchar **envp;
	int out_fd = -1;

	/*
	 * LPAC_APDU=ofono is the whole point - see the comment in
	 * lpac_client.h. Everything else lpac needs comes from the inherited
	 * environment.
	 */
	envp = g_get_environ();
	envp = g_environ_setenv(envp, "LPAC_APDU", "ofono", TRUE);

	if (!g_spawn_async_with_pipes(NULL, call->spawn_argv, envp,
			G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
			NULL, NULL, &call->pid, NULL, &out_fd, NULL, &error)) {
		g_warning("Could not run %s: %s", LPAC_BINARY,
				error ? error->message : "unknown error");
		if (error)
			g_error_free(error);
		g_strfreev(envp);

		call->cb(-1, "could not run lpac", jinvalid(), call->user_data);

		g_string_free(call->tail, TRUE);
		g_strfreev(call->spawn_argv);
		g_free(call);

		return FALSE;
	}

	g_strfreev(envp);

	lpac_current = call;

	call->out = g_io_channel_unix_new(out_fd);
	g_io_channel_set_close_on_unref(call->out, TRUE);
	g_io_channel_set_encoding(call->out, NULL, NULL);
	call->out_watch = g_io_add_watch(call->out,
			G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			lpac_output_cb, call);
	g_io_channel_unref(call->out);

	g_child_watch_add(call->pid, lpac_child_cb, call);

	return TRUE;
}

static void lpac_start_next(void)
{
	while (!lpac_current) {
		struct lpac_call *next = g_queue_pop_head(&lpac_pending);

		if (!next)
			return;

		/* lpac_start() reports and frees on failure; try the one after */
		lpac_start(next);
	}
}

bool lpac_run(const char * const *argv, lpac_result_cb cb,
              lpac_progress_cb progress_cb, void *user_data)
{
	struct lpac_call *call;
	guint n = 0;
	guint i;

	if (!cb)
		return false;

	while (argv && argv[n])
		n++;

	call = g_new0(struct lpac_call, 1);
	call->cb = cb;
	call->progress_cb = progress_cb;
	call->user_data = user_data;
	call->tail = g_string_new(NULL);

	call->spawn_argv = g_new0(gchar *, n + 2);
	call->spawn_argv[0] = g_strdup(LPAC_BINARY);
	for (i = 0; i < n; i++)
		call->spawn_argv[i + 1] = g_strdup(argv[i]);

	if (lpac_current) {
		/* One session at a time - see the note at the top */
		g_queue_push_tail(&lpac_pending, call);
		return true;
	}

	return lpac_start(call);
}

// vim:ts=4:sw=4:noexpandtab

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

#include <stdio.h>
#include <string.h>

#include "shim.h"

struct LSHandle {
	char *name;
	LSMethod *methods;
	void *category_data;
};

struct LSMessage {
	int refcount;
	char *payload;
	GPtrArray *replies;
};

static LSHandle *shim_handle;
static GPtrArray *shim_post_list;
static guint shim_messages;

static void shim_post_free(gpointer data)
{
	ShimPost *post = data;

	g_free(post->method);
	g_free(post->payload);
	g_free(post);
}

/* --- luna-service2 -------------------------------------------------------- */

bool LSErrorInit(LSError *error)
{
	memset(error, 0, sizeof(*error));
	return true;
}

void LSErrorFree(LSError *error)
{
	if (error) {
		g_free(error->message);
		error->message = NULL;
	}
}

void LSErrorPrint(LSError *error, FILE *out)
{
	fprintf(out, "LSError: %s\n", error && error->message ?
					error->message : "(none)");
}

bool LSRegister(const char *name, LSHandle **sh, LSError *error)
{
	g_assert(!shim_handle);
	shim_handle = g_new0(LSHandle, 1);
	shim_handle->name = g_strdup(name);
	*sh = shim_handle;
	return true;
}

bool LSGmainAttach(LSHandle *sh, GMainLoop *loop, LSError *error)
{
	g_assert(sh == shim_handle);
	return true;
}

bool LSRegisterCategory(LSHandle *sh, const char *category, LSMethod *methods,
			LSSignal *signals, LSProperty *properties,
			LSError *error)
{
	g_assert(sh == shim_handle);
	g_assert_cmpstr(category, ==, "/");
	sh->methods = methods;
	return true;
}

bool LSCategorySetData(LSHandle *sh, const char *category, void *user_data,
			LSError *error)
{
	g_assert(sh == shim_handle);
	g_assert_cmpstr(category, ==, "/");
	sh->category_data = user_data;
	return true;
}

bool LSUnregister(LSHandle *sh, LSError *error)
{
	g_assert(sh == shim_handle);
	g_free(sh->name);
	g_free(sh);
	shim_handle = NULL;
	return true;
}

const char *LSMessageGetPayload(LSMessage *message)
{
	g_assert_cmpint(message->refcount, >, 0);
	return message->payload;
}

void LSMessageRef(LSMessage *message)
{
	g_assert_cmpint(message->refcount, >, 0);
	message->refcount++;
}

void LSMessageUnref(LSMessage *message)
{
	g_assert_cmpint(message->refcount, >, 0);
	if (!--message->refcount) {
		g_free(message->payload);
		g_ptr_array_free(message->replies, TRUE);
		g_free(message);
		shim_messages--;
	}
}

bool LSMessageReply(LSHandle *sh, LSMessage *message, const char *payload,
			LSError *error)
{
	g_assert(sh == shim_handle);
	g_assert_cmpint(message->refcount, >, 0);
	g_ptr_array_add(message->replies, g_strdup(payload));
	return true;
}

bool LSMessageIsSubscription(LSMessage *message)
{
	return strstr(message->payload, "\"subscribe\":true") != NULL;
}

bool LSSubscriptionProcess(LSHandle *sh, LSMessage *message, bool *subscribed,
			LSError *error)
{
	*subscribed = LSMessageIsSubscription(message);
	return true;
}

bool LSSubscriptionPost(LSHandle *sh, const char *category, const char *method,
			const char *payload, LSError *error)
{
	ShimPost *post = g_new0(ShimPost, 1);

	g_assert(sh == shim_handle);
	g_assert_cmpstr(category, ==, "/");
	post->method = g_strdup(method);
	post->payload = g_strdup(payload);
	g_ptr_array_add(shim_post_list, post);
	return true;
}

/* --- the test's side ------------------------------------------------------ */

LSMessage *shim_call(const char *method, const char *payload)
{
	LSMessage *message;
	const LSMethod *m;

	g_assert(shim_handle && shim_handle->methods);
	for (m = shim_handle->methods; m->name; m++) {
		if (!strcmp(m->name, method))
			break;
	}
	g_assert_nonnull(m->name);

	message = g_new0(LSMessage, 1);
	message->refcount = 2;	/* The bus's, for the call, and the test's */
	message->payload = g_strdup(payload);
	message->replies = g_ptr_array_new_with_free_func(g_free);
	shim_messages++;

	g_assert(m->function(shim_handle, message, shim_handle->category_data));

	/* The bus's reference goes; the test's keeps the message alive */
	g_assert_cmpint(message->refcount, >, 1);
	message->refcount--;

	return message;
}

const GPtrArray *shim_replies(LSMessage *message)
{
	return message->replies;
}

const GPtrArray *shim_posts(void)
{
	return shim_post_list;
}

guint shim_posts_for(const char *method)
{
	guint i, n = 0;

	for (i = 0; i < shim_post_list->len; i++) {
		const ShimPost *post = g_ptr_array_index(shim_post_list, i);

		if (!strcmp(post->method, method))
			n++;
	}
	return n;
}

guint shim_live_messages(void)
{
	return shim_messages;
}

gboolean shim_registered(void)
{
	return shim_handle != NULL;
}

void shim_reset(void)
{
	if (shim_post_list)
		g_ptr_array_free(shim_post_list, TRUE);
	shim_post_list = g_ptr_array_new_with_free_func(shim_post_free);
}

// vim:ts=4:sw=4:noexpandtab

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

#ifndef SHIM_H_
#define SHIM_H_

#include <glib.h>
#include <luna-service2/lunaservice.h>

/*
 * A stand-in for the part of luna-service2 the adapter uses, linked in
 * place of the real library. The service registers its methods with it,
 * and a test calls them the way the hub would: the bus holds a reference
 * on the message while the method runs and drops it when the method
 * returns, so a reply sent later only works if the adapter took its own
 * reference. Replies and subscription posts are recorded.
 */

/* Calls method with payload. The returned message is the test's to unref. */
LSMessage *shim_call(const char *method, const char *payload);

/* Replies sent to message so far, oldest first */
const GPtrArray *shim_replies(LSMessage *message);

typedef struct shim_post {
	char *method;
	char *payload;
} ShimPost;

/* LSSubscriptionPost calls since the last shim_reset(), oldest first */
const GPtrArray *shim_posts(void);
guint shim_posts_for(const char *method);

/* Messages still referenced; 0 once every test message is unrefed */
guint shim_live_messages(void);

/* Whether the service is registered (between LSRegister and LSUnregister) */
gboolean shim_registered(void);

void shim_reset(void);

#endif

// vim:ts=4:sw=4:noexpandtab

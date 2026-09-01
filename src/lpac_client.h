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

#ifndef LPAC_CLIENT_H_
#define LPAC_CLIENT_H_

#include <glib.h>
#include <pbnjson.h>

/*
 * Thin wrapper around the lpac binary.
 *
 * lpac is the LPA: it speaks SGP.22 to the eUICC and HTTPS to the SM-DP+.
 * Reimplementing that here would be pointless, so this runs it and parses its
 * JSON. It is always invoked with LPAC_APDU=ofono, which routes the APDUs
 * through org.ofono.EuiccManager - so ofono keeps the modem and telephony
 * survives profile management.
 *
 * Calls are asynchronous: a profile download talks to a remote server and can
 * take a minute or more, which must not block the luna-service2 main loop.
 */

/*
 * result is the payload of lpac's final {"type":"lpa"} line, already parsed,
 * or NULL if lpac could not be run at all. Ownership stays with the caller of
 * the callback - do not free it.
 *
 * code is lpac's own status: 0 on success, non-zero otherwise, and -1 if lpac
 * produced nothing usable. message is lpac's error string when it has one.
 */
typedef void (*lpac_result_cb)(int code, const char *message,
                               jvalue_ref result, void *user_data);

/*
 * Progress lines ({"type":"progress"}) as they arrive - es9p_initiate_
 * authentication, es10b_load_bound_profile_package and so on. A download is
 * slow enough that the UI wants to show these. May be NULL.
 */
typedef void (*lpac_progress_cb)(const char *step, void *user_data);

/* argv is NULL-terminated, without the "lpac" itself: e.g. {"profile","list"} */
bool lpac_run(const char * const *argv, lpac_result_cb cb,
              lpac_progress_cb progress_cb, void *user_data);

#endif

// vim:ts=4:sw=4:noexpandtab

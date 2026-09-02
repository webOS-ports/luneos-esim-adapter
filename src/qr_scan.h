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

#ifndef QR_SCAN_H_
#define QR_SCAN_H_

#include <glib.h>
#include <stdbool.h>

/*
 * Reads a barcode out of a still image, using GStreamer's zbar element:
 *
 *     filesrc ! decodebin ! videoconvert ! zbar ! fakesink
 *
 * An operator's eSIM QR contains an activation code and nothing else, so
 * decoding one frame is all that is needed - the settings page grabs frames
 * from its camera preview and offers them here one at a time. Doing it this
 * way keeps the camera in the UI process, where the preview already is,
 * instead of fighting it for the device.
 *
 * Nothing else on a LuneOS image can decode a QR: libqrencode only writes
 * them.
 */

/*
 * text is the decoded string, or NULL with error set. Neither outlives the
 * callback.
 */
typedef void (*qr_scan_cb)(const char *text, const char *error,
                           void *user_data);

/* Decodes asynchronously; false means the pipeline could not even be built. */
bool qr_scan_file(const char *path, qr_scan_cb cb, void *user_data);

#endif

// vim:ts=4:sw=4:noexpandtab

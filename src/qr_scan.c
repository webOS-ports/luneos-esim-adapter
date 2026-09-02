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

#include <gst/gst.h>

#include "qr_scan.h"

/* A still image decodes in well under this; the bound is just a safety net. */
#define QR_SCAN_TIMEOUT_MS	10000

struct qr_scan {
	GstElement *pipeline;
	guint bus_watch;
	guint timeout_id;
	qr_scan_cb cb;
	void *user_data;
	gboolean finished;
};

static void qr_scan_finish(struct qr_scan *scan, const char *text,
							const char *error)
{
	if (scan->finished)
		return;

	scan->finished = TRUE;

	scan->cb(text, error, scan->user_data);

	if (scan->timeout_id) {
		g_source_remove(scan->timeout_id);
		scan->timeout_id = 0;
	}

	if (scan->bus_watch) {
		g_source_remove(scan->bus_watch);
		scan->bus_watch = 0;
	}

	gst_element_set_state(scan->pipeline, GST_STATE_NULL);
	gst_object_unref(scan->pipeline);
	g_free(scan);
}

static gboolean qr_scan_timeout(gpointer user_data)
{
	struct qr_scan *scan = user_data;

	scan->timeout_id = 0;
	qr_scan_finish(scan, NULL, "timed out decoding the image");

	return G_SOURCE_REMOVE;
}

static gboolean qr_scan_bus_cb(GstBus *bus, GstMessage *message,
							gpointer user_data)
{
	struct qr_scan *scan = user_data;

	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_ELEMENT: {
		const GstStructure *s = gst_message_get_structure(message);

		/* zbar reports every symbol it finds as an element message */
		if (s && gst_structure_has_name(s, "barcode")) {
			const char *symbol = gst_structure_get_string(s,
								"symbol");

			if (symbol) {
				qr_scan_finish(scan, symbol, NULL);
				return G_SOURCE_REMOVE;
			}
		}
		break;
	}
	case GST_MESSAGE_EOS:
		/* Ran through the whole image without zbar seeing anything */
		qr_scan_finish(scan, NULL, "no barcode found in the image");
		return G_SOURCE_REMOVE;
	case GST_MESSAGE_ERROR: {
		GError *err = NULL;
		char *debug = NULL;

		gst_message_parse_error(message, &err, &debug);
		qr_scan_finish(scan, NULL,
				err ? err->message : "could not decode the image");

		if (err)
			g_error_free(err);
		g_free(debug);

		return G_SOURCE_REMOVE;
	}
	default:
		break;
	}

	return G_SOURCE_CONTINUE;
}

bool qr_scan_file(const char *path, qr_scan_cb cb, void *user_data)
{
	struct qr_scan *scan;
	GstElement *pipeline;
	GstElement *src;
	GstBus *bus;
	GError *error = NULL;
	static gboolean gst_ready;

	if (!cb || !path)
		return false;

	if (!gst_ready) {
		gst_init(NULL, NULL);
		gst_ready = TRUE;
	}

	/*
	 * decodebin rather than a fixed decoder: the caller decides what it
	 * grabs its frames as, and this way a PNG and a JPEG both work.
	 */
	pipeline = gst_parse_launch("filesrc name=src ! decodebin ! "
				"videoconvert ! zbar ! fakesink sync=false",
				&error);
	if (!pipeline) {
		g_warning("Could not build the QR pipeline: %s",
				error ? error->message : "unknown error");
		if (error)
			g_error_free(error);
		return false;
	}

	if (error) {
		/* parse_launch can succeed with warnings */
		g_error_free(error);
	}

	src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
	if (!src) {
		gst_object_unref(pipeline);
		return false;
	}

	g_object_set(src, "location", path, NULL);
	gst_object_unref(src);

	scan = g_new0(struct qr_scan, 1);
	scan->pipeline = pipeline;
	scan->cb = cb;
	scan->user_data = user_data;

	bus = gst_element_get_bus(pipeline);
	scan->bus_watch = gst_bus_add_watch(bus, qr_scan_bus_cb, scan);
	gst_object_unref(bus);

	scan->timeout_id = g_timeout_add(QR_SCAN_TIMEOUT_MS, qr_scan_timeout,
						scan);

	if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
					GST_STATE_CHANGE_FAILURE) {
		qr_scan_finish(scan, NULL, "could not start the decoder");
		return true;	/* the caller has been told through cb */
	}

	return true;
}

// vim:ts=4:sw=4:noexpandtab

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

#ifndef FAKE_OFONO_H_
#define FAKE_OFONO_H_

#include <glib.h>

/*
 * org.ofono on a test bus, with one modem, FAKE_OFONO_MODEM. It runs in a
 * thread of its own: the adapter makes synchronous D-Bus calls from its
 * main loop, and they could never be answered from that same loop.
 *
 * It offers what the adapter uses: Manager.GetModems, the nemo
 * ModemManager.GetAvailableModems, SimManager.GetProperties, SetProperty
 * and PropertyChanged, and EuiccManager.ResetCard.
 */

#define FAKE_OFONO_MODEM	"/ril_1"

typedef struct fake_ofono FakeOfono;

FakeOfono *fake_ofono_start(const char *address);
void fake_ofono_stop(FakeOfono *ofono);

/*
 * How the modem looks. The defaults: EuiccManager listed, a SIM present
 * in slot 2 of 2, ResetCard and SetProperty answered successfully.
 */
void fake_ofono_set_euicc(FakeOfono *ofono, gboolean euicc);
/* 0 leaves CardSlotCount and ActiveCardSlot out, as ofono does with no SIM */
void fake_ofono_set_slots(FakeOfono *ofono, guint count, guint active);
/* A D-Bus error name to answer with, or NULL to succeed */
void fake_ofono_set_reset_error(FakeOfono *ofono, const char *error);
void fake_ofono_set_slot_error(FakeOfono *ofono, const char *error);
/*
 * A file whose existence means lpac is running: ResetCard notes down
 * whether it was called while it existed.
 */
void fake_ofono_set_busy_file(FakeOfono *ofono, const char *path);

/* Back to the defaults, and the counts below to 0 */
void fake_ofono_reset(FakeOfono *ofono);

guint fake_ofono_reset_calls(FakeOfono *ofono);
guint fake_ofono_reset_calls_while_busy(FakeOfono *ofono);
guint fake_ofono_slot_calls(FakeOfono *ofono);
guint fake_ofono_last_slot(FakeOfono *ofono);

/* SimManager.PropertyChanged on the modem */
void fake_ofono_emit_property_changed(FakeOfono *ofono);

#endif

// vim:ts=4:sw=4:noexpandtab

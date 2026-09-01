# com.webos.service.esim — Luna Service API reference

`luneos-esim-adapter` exposes eSIM (GSMA SGP.22) profile management to
LuneOS applications as the luna-service2 service **`com.webos.service.esim`**.

    eUICC ──APDU──▶ ofono (org.ofono.EuiccManager) ──D-Bus──▶ lpac ──▶ luneos-esim-adapter ──LS2──▶ apps
                    ofono (org.ofono.SimManager)   ──D-Bus──────────────▶

Two different things are going on, and it helps to keep them apart:

* **Profiles** are SGP.22 — a protocol conversation between the eUICC and the
  operator's SM-DP+ server. That is [lpac](https://github.com/estkme-group/lpac)'s
  job; this service runs it and parses its JSON.
* **The card** — whether an eUICC exists, which physical slot it is in, which
  slot the modem is using — is ofono's, read over D-Bus directly.

lpac is always run with `LPAC_APDU=ofono`, so APDUs travel through ofono rather
than around it, and telephony keeps working while profiles are managed.

All methods are registered on the root category, so the full method URI is
`luna://com.webos.service.esim/<method>`.

| Method | Kind | Purpose |
|---|---|---|
| [`getStatus`](#getstatus) | query, subscribable | Whether eSIM is usable, and slot state |
| [`getChipInfo`](#getchipinfo) | query | EID, firmware, free space |
| [`getProfiles`](#getprofiles) | query | Installed profiles |
| [`downloadProfile`](#downloadprofile) | operation, subscribable | Install a profile from an activation code |
| [`enableProfile`](#enableprofile) | operation | Make a profile the active SIM |
| [`disableProfile`](#disableprofile) | operation | Deactivate a profile |
| [`deleteProfile`](#deleteprofile) | operation | Remove a profile permanently |
| [`setProfileNickname`](#setprofilenickname) | operation | Rename a profile locally |
| [`setActiveSlot`](#setactiveslot) | operation | Choose which physical card slot the modem uses |

## Conventions

**Replies.** Every reply carries `returnValue` (boolean). Failures from lpac
carry both fields:

```json
{ "returnValue": false, "errorCode": -1, "errorText": "es9p_authenticate_client" }
```

`errorText` is lpac's own message where there is one — `"Refused"`,
`"campaign resource pool is empty"`, `"install_failed_due_to_iccid_already_exists_on_euicc"`
— so it is worth showing verbatim rather than replacing with something generic.
Failures raised by the service itself carry only `errorText`. Two generic errors
can come back from any method taking parameters: `"Malformed json."` and
`"Invalid parameters."`.

**Asynchronous methods.** Everything that touches the card runs lpac as a child
process and replies when it exits, not immediately. `getChipInfo` and
`getProfiles` take about a second. `downloadProfile` talks to a remote server
and can take a minute or more.

**Subscriptions.** `getStatus` accepts `"subscribe": true` and pushes the same
payload whenever ofono reports a SIM property change — which covers both slot
switches and profile enables. `downloadProfile` accepts `"subscribe": true` to
receive progress.

```
luna-send -i -n 5 luna://com.webos.service.esim/getStatus '{"subscribe":true}'
```

**Availability.** A device with no eUICC is a normal state, not an error:
`getStatus` reports `"available": false`. That happens when no modem exposes
`org.ofono.EuiccManager`, which in turn means either the SIM driver cannot open
logical channels, or — on a multi-slot device — the eUICC is not the active slot.

---

## getStatus

Card and slot state. Cheap: this is D-Bus only, no lpac.

**Parameters**

| Name | Type | Required | Meaning |
|---|---|---|---|
| `subscribe` | boolean | no | Push updates on SIM property changes |

**Reply**

```json
{
    "returnValue": true,
    "available": true,
    "modemPath": "/ril_0",
    "slotCount": 2,
    "activeSlot": 2,
    "simPresent": true,
    "iccid": "8944476500018274378"
}
```

| Field | Meaning |
|---|---|
| `available` | A modem exposes `org.ofono.EuiccManager` — eSIM is usable |
| `modemPath` | ofono modem object path |
| `slotCount` | Physical card slots the modem knows about |
| `activeSlot` | Which one is mapped to the modem, **1-based** |
| `simPresent` | Whether the active slot has a usable card |
| `iccid` | ICCID of the currently active SIM, i.e. the enabled profile |

Only `returnValue` and `available` are guaranteed; the rest are absent when
`available` is false, and `slotCount`/`activeSlot` are absent on drivers that
cannot report slots.

Note `simPresent` can be false while an eUICC is perfectly healthy: an eUICC
with no *enabled* profile presents no UICC application.

## getChipInfo

Identity and capabilities of the eUICC itself, from `lpac chip info`. The EID
identifies the chip, not any profile, and does not change.

**Parameters** — none.

**Reply**

```json
{
    "returnValue": true,
    "result": {
        "eidValue": "89049032000001000000022582603733",
        "EuiccConfiguredAddresses": { "defaultDpAddress": "...", "rootDsAddress": "..." },
        "EUICCInfo2": {
            "profileVersion": "2.0.0",
            "euiccFirmwareVer": "1.3.0",
            "extCardResource": { "installedApplication": 1, "freeNonVolatileMemory": 1077056 },
            "euiccCiPKIdListForVerification": [ "81370f51...", "b60f0b89..." ]
        }
    }
}
```

`result` is lpac's payload unchanged. Two fields are more useful than they look:

* `freeNonVolatileMemory` — profiles are tens of kB; this is what limits how
  many fit.
* `euiccCiPKIdListForVerification` — the certificate issuers this chip trusts.
  A profile signed by a CI not in this list cannot be installed, which is why
  SGP.26 *test* SM-DP+ servers do not work on retail hardware.

## getProfiles

**Parameters** — none.

**Reply**

```json
{
    "returnValue": true,
    "result": [
        {
            "iccid": "8944476500018274378",
            "isdpAid": "a0000005591010ffffffff8900001200",
            "profileState": "enabled",
            "profileNickname": null,
            "serviceProviderName": "BetterRoaming",
            "profileName": "BetterRoaming",
            "profileClass": "operational"
        }
    ]
}
```

`profileState` is `"enabled"` or `"disabled"`. At most one profile is enabled at
a time. `profileClass` `"test"` marks factory test profiles, which are normally
present from the factory and cannot be used for service.

## downloadProfile

Downloads and installs a profile from an operator's activation code. This is the
long one — a dozen round trips to the SM-DP+.

**Parameters**

| Name | Type | Required | Meaning |
|---|---|---|---|
| `smdp` | string | yes | SM-DP+ address, e.g. `rsp.truphone.com` |
| `activationCode` | string | yes | Matching ID from the operator |
| `confirmationCode` | string | no | Only if the operator issued one |
| `subscribe` | boolean | no | Receive progress updates |

An operator's QR code contains `LPA:1$<smdp>$<activationCode>` and optionally
`$<confirmationCode>`; splitting that string on `$` gives exactly these fields.

**Progress updates** (subscribers only) name the SGP.22 step in flight:

```json
{ "returnValue": true, "stage": "es9p_initiate_authentication" }
```

The sequence is roughly `es10b_get_euicc_challenge_and_info` →
`es9p_initiate_authentication` → `es10b_authenticate_server` →
`es9p_authenticate_client` → `es8p_meatadata_parse` → `es10b_prepare_download`
→ `es9p_get_bound_profile_package` → `es10b_load_bound_profile_package`.

**Reply** — `returnValue: true` on success, with lpac's payload in `result`.

Three things worth telling the user before calling this:

* It needs a working internet connection *before* the profile exists — WiFi.
* An activation code can normally only be used **once**. A failed download
  usually means the operator has to reissue it.
* Some plans start their validity window at install rather than first use.

Failures arrive as `errorText` from the SM-DP+ or the card, e.g.
`"Refused"` (code already used or unknown) or
`"install_failed_due_to_iccid_already_exists_on_euicc"` (this exact profile is
already installed).

## enableProfile

Makes a profile the active SIM. Any currently enabled profile is disabled first.

**Parameters** — `iccid` (string, required).

**A successful enable is not the whole story.** The modem does not re-read the
card by itself, so ofono can still report no SIM afterwards. Bouncing the slot
mapping forces a card reset:

```
luna-send -n 1 luna://com.webos.service.esim/setActiveSlot '{"slot":1}'
luna-send -n 1 luna://com.webos.service.esim/setActiveSlot '{"slot":2}'
```

## disableProfile

**Parameters** — `iccid` (string, required).

Leaves the eUICC with no enabled profile, which presents no UICC application —
ofono will report `simPresent: false`.

## deleteProfile

Removes a profile permanently. Most profiles cannot be re-downloaded, so this
usually destroys the subscription.

**Parameters** — `iccid` (string, required).

An enabled profile normally cannot be deleted; disable it first.

## setProfileNickname

Local label only — nothing is sent to the operator.

**Parameters** — `iccid` (string, required), `nickname` (string, required).

## setActiveSlot

Chooses which physical card slot the modem uses. Only meaningful when
`getStatus` reports `slotCount` greater than 1.

**Parameters** — `slot` (integer, required, **1-based**, matching `activeSlot`).

On a device where the eUICC is a second physical slot behind a single logical
modem, this is the difference between the eUICC being reachable and not. It is
also how a card reset is forced after enabling a profile.

**Failures.** Switching twice in quick succession fails while the card is still
being re-read — leave several seconds between switches, and treat a failure as
"try again shortly" rather than a hardware fault. On drivers that cannot report
slots, ofono answers `not implemented`.

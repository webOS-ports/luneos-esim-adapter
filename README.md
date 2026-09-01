luneos-esim-adapter
===================

eSIM (GSMA SGP.22) profile management for LuneOS, on the luna-service2 bus as
`com.webos.service.esim`.

The work splits in two, and the split is the whole design:

* **Profiles** — listing, downloading, enabling, deleting — are SGP.22. That is
  a large protocol with real crypto and an HTTPS conversation with the
  operator's SM-DP+ server, and [lpac](https://github.com/estkme-group/lpac)
  already implements it. This service runs lpac and parses its JSON rather than
  reimplementing any of it.
* **The card** — is there an eUICC, which physical slot is it in, which slot is
  live — belongs to ofono, and is read over `org.ofono.SimManager` and
  `org.ofono.EuiccManager`.

lpac is always invoked with `LPAC_APDU=ofono`, so its APDUs travel through
ofono's `org.ofono.EuiccManager` interface. That matters: the alternative
backends take the modem away from ofono (lpac's `gbinder_hidl` calls
`setResponseFunctions` and steals IRadio's response callback), which breaks
telephony for as long as the LPA runs. Going through ofono keeps calls and data
working while profiles are managed, and works on AT modems, binder HIDL and
binder AIDL alike, because ofono has already abstracted them.

Requirements
------------

* ofono with the `org.ofono.EuiccManager` interface (patches in meta-luneos,
  `recipes-connectivity/ofono`)
* `lpac`, built with the oFono APDU backend
* a modem whose SIM driver can open logical channels — `atmodem` and the binder
  plugin can, `qmimodem` needs the logical-channel patch

The eUICC does not have to be soldered in: an eSIM adapter card in the physical
SIM slot is an eUICC too, and works the same way.

API
---

```
luna://com.webos.service.esim/getStatus            (subscribable)
  -> { available, modemPath, slotCount, activeSlot, simPresent, iccid }
```
`available` is false when no modem exposes `org.ofono.EuiccManager`, which is
also the answer to "can this device do eSIM at all".

```
luna://com.webos.service.esim/getChipInfo
  -> { result: { eidValue, EUICCInfo2: { ... } } }

luna://com.webos.service.esim/getProfiles
  -> { result: [ { iccid, profileName, profileNickname,
                   serviceProviderName, profileState }, ... ] }

luna://com.webos.service.esim/downloadProfile      (subscribe for progress)
  { smdp, activationCode, confirmationCode? }
  -> progress: { stage: "es9p_initiate_authentication" }, then the final reply
```
An operator's QR code contains `LPA:1$<smdp>$<activationCode>`; the two fields
come straight out of it. Downloading needs working internet, and an activation
code can normally only be used once.

```
luna://com.webos.service.esim/enableProfile        { iccid }
luna://com.webos.service.esim/disableProfile       { iccid }
luna://com.webos.service.esim/deleteProfile        { iccid }
luna://com.webos.service.esim/setProfileNickname   { iccid, nickname }
luna://com.webos.service.esim/setActiveSlot        { slot }   1-based
```

Slots
-----

On a phone where the eUICC is a second *physical* slot behind a single logical
modem — a Pixel 3a, for instance — nothing on the eUICC is reachable until that
slot is the active one, and `getStatus` will report `slotCount` 2. Switching is
`setActiveSlot`, which ofono performs through `IRadioConfig.setSimSlotsMapping`.

Two behaviours worth knowing:

* Switching slots twice in quick succession fails; the card is still being
  re-read. Leave a few seconds between switches.
* Enabling a profile is not enough on its own to make it usable — the modem has
  to re-read the card. A slot bounce does that.

License
-------

Apache-2.0. See COPYING.

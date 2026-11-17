# ESP32 Wi-Fi Provisioning & Diagnostic Captive Portal

### ESP32 · ESP-IDF v5.4.2 · STA auto-connect · SoftAP fallback · Captive portal · OTA endpoint

This project started with a simple goal:

> Connect an ESP32 to Wi-Fi and expose a local web page.

But real embedded systems rarely fail on the happy path.

Credentials may be missing. A saved SSID may no longer exist. A router may be powered
off. A phone hotspot may change channel or go to sleep. A browser may submit malformed
data. The device may reboot before an HTTP response reaches the client. NVS may hold
stale configuration. A network interface may be initialized twice.

So this became a Wi-Fi provisioning component for ESP32 that tries to handle the boot
path when things go wrong, and falls back to a captive portal when it cannot get online.

---

## Status

**Working and usable on the bench. Not production-ready.**

The boot-time failure handling is real and it works. What is not there yet is a security
model, and there is no supervision of the Wi-Fi link after the first successful connect.
Both are covered honestly in [Security status](#security-status) and
[Known limitations](#known-limitations) below — read those before putting this on a
device that leaves your desk.

---

## What it does

| Capability | State |
| --- | --- |
| STA auto-connect from credentials in NVS | Working |
| SoftAP fallback when STA fails or credentials are missing | Working |
| Captive portal with DNS redirect | Working |
| Wi-Fi scan endpoint, sorted by signal strength | Working, blocking |
| Credential save and delayed reboot | Working, over `GET` |
| NVS erase / factory reset endpoint | Working, unauthenticated |
| OTA firmware push endpoint | Registered and live, unauthenticated, unsigned |
| Runtime heap and IP diagnostics | In the example app |
| Reconnect after the link drops | **Not implemented** |
| Authentication on any endpoint | **Not implemented** |

---

## Repository layout

| File | Role |
| --- | --- |
| `ESP32_WIFI_Provision_Care.c` | The component: state machine, HTTP server, endpoints, SoftAP bring-up, OTA receiver |
| `ESP32_WIFI_Provision_Care.h` | Public API |
| `dns_server.c` / `dns_server.h` | DNS responder from the Espressif captive portal example (CC0 / Unlicense), unmodified |
| `main.c` | Example application: NVS dump, component call, mDNS, diagnostic loop |
| `wifi.html.gz` | Portal page, embedded in the binary via `EMBED_FILES` |

---

## Public API

```c
/* Connect using credentials in flash, or raise a provisioning AP.
   Blocks the calling task for up to 30 seconds while connecting. */
void wifi_provision_care(const char ap_ssid_name[32]);

/* Current state of the component's state machine. */
wifi_state_t wifi_provision_care_get_state(void);

/* The station netif, or NULL before initialization. */
esp_netif_t *wifi_provision_care_get_sta_netif(void);

/* OTA POST handler. The component already registers this on its own
   server; only register it yourself if you are running your own. */
esp_err_t wifi_provision_care_updateota_post_handler(httpd_req_t *req);
```

States are `WIFI_STATE_INIT`, `WIFI_STATE_STA_CONNECTING`, `WIFI_STATE_STA_CONNECTED`,
`WIFI_STATE_PROVISIONING` and `WIFI_STATE_RESTART_REQUIRED`.

> **Note:** `wifi_provision_care_get_state()` reports the state as of the last
> transition. Because the event handlers are unregistered once a connection succeeds, it
> will keep reporting `WIFI_STATE_STA_CONNECTED` even if the link later drops. Do not use
> it as a liveness check.

Minimal usage:

```c
void app_main(void)
{
    nvs_flash_init();
    wifi_provision_care("my-device-ap");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}
```

---

## System behaviour

### 1. Station mode

If credentials exist in flash, the device connects to the saved access point. It allows
5 retries at 3-second intervals, and waits up to 30 seconds for an IP.

```text
I (512)  wifi_prov_care: Subsystem State Transitioned to Enum: 1
I (1840) wifi_prov_care: Leased IP from router system space: 192.168.1.45
I (1850) wifi_prov_care: Subsystem State Transitioned to Enum: 2
I (1860) wifi_prov_care: Starting http server on port: 80
I (1870) wifi_prov_care: HTTP server started in STA mode. Open: http://<ESP32-IP>/wifi
I (1880) wifi_prov_care: Provisioning framework detached. Device safely connected.
```

The example app then publishes `esp32-wifi-provision-care-example.local` over mDNS and
logs heap and address information every 15 seconds. mDNS belongs to the example, not to
the component.

### 2. SoftAP fallback

If credentials are missing, wrong, or the access point cannot be reached, the device
raises its own network.

```text
W (512)   wifi_prov_care: Disconnect detected. Disconnect Reason Code: 201, Current Retry Iteration: 6/5
E (31500) wifi_prov_care: Failed to establish station hook within time window allocation limits.
W (31520) wifi_prov_care: Entering fallback mode: Constructing Safe Provisioning Access Point...
I (31530) esp_netif_lwip: DHCP server started on interface WIFI_AP_DEF with IP: 200.200.200.2
I (31540) wifi_prov_care: Starting http server on port: 80
```

Connect to the device's access point and open:

```text
http://200.200.200.2/wifi
```

The DNS responder answers every A query with the AP address, so most browsers will raise
the captive portal sign-in page by themselves.

**The AP name is derived from what you pass in.** `wifi_provision_care()` copies at most
31 characters, so a longer name is truncated. The example passes a 33-character tag and
therefore advertises `esp32-wifi-provision-care-examp`. Pass `NULL` or an empty string and
the component generates `ESP32-PROV-XXXX` from the last two bytes of the factory MAC.

### 3. Credential save

Submitting the form writes the credentials to flash and reboots the device after 3
seconds, so the browser receives the response first.

The device does **not** test the credentials before saving them. If they are wrong you
will see the save confirmation, the device will reboot, fail to connect, and come back as
an access point about 45 seconds later. The browser is told nothing, and the previously
stored network has already been overwritten.

---

## Boot flow

```text
Boot
  |
Initialize netif, event loop, NVS
  |
Credentials in flash?
  |-- Yes --> STA mode
  |             |
  |           Connect (5 retries, 30 s timeout)
  |             |
  |           Got an IP?
  |             |-- Yes --> HTTP server on the LAN, component returns
  |             '-- No  --> SoftAP provisioning
  |
  '-- No  --> SoftAP provisioning
                |
              Captive portal (AP + STA, DNS redirect)
                |
              User submits credentials
                |
              Write to flash, reboot after 3 s
                |
              Boot again in STA mode
```

---

## HTTP endpoints

All six handlers are registered in **both** modes — while provisioning, and again on the
local network once the device has connected.

| Endpoint | Method | Auth | Effect |
| --- | --- | --- | --- |
| `/wifi` | GET | none | The portal page, served gzipped from flash |
| `/favicon.ico` | GET | none | Icon |
| `/scanap` | GET | none | Blocking scan, JSON list sorted by RSSI |
| `/savewifi` | GET | none | Writes credentials to flash, reboots |
| `/nvserase` | GET | none | Erases the entire NVS partition, reboots |
| `/updateota` | POST | none | Writes the request body to the next OTA partition, reboots |
| any 404 | any | none | 302 redirect to the portal page |

Credentials are currently submitted as **URL query parameters on a `GET`**, not in a POST
body. A `savewifi_post_handler` exists in the source but is not registered.

Socket limits are `max_open_sockets = 7` in station mode and `5` in AP mode.

---

## Security status

This component has **no authentication, no transport security and no firmware signing**.
It is fine on a bench and on an isolated network. It is not safe on a network you care
about, and the following are deliberate statements of fact rather than a roadmap.

- **`/updateota` accepts firmware from anyone.** The image is checked for structural
  validity by `esp_ota_end()`, but nothing checks that it came from you. There is no
  signature verification and no Secure Boot. Anyone who can reach port 80 can replace the
  firmware permanently.
- **`/nvserase` and `/savewifi` are unauthenticated `GET` requests.** They can be
  triggered by a cross-site request from any page the user happens to visit — an `<img>`
  tag is enough.
- **The endpoints stay open after provisioning.** The HTTP server is started again in
  station mode, so all of the above remain reachable from the whole local network for as
  long as the device is powered.
- **The provisioning AP is open.** No passphrase. Anyone in radio range can join it.
- **Credentials travel in clear text,** in a URL, over HTTP, on that open AP.
- **Credentials are stored in plain text** in the Wi-Fi driver's NVS namespace. Neither
  NVS encryption nor flash encryption is enabled, so anyone holding the hardware can read
  them back.
- **The station authmode threshold is left at `WIFI_AUTH_OPEN`,** so the device will
  associate with an open network impersonating your SSID.

If you want to try this on a real network, put it on a guest VLAN, and assume anything on
that VLAN can take the device over.

---

## Known limitations

- **No reconnect supervision.** Both event handlers are unregistered once a connection
  succeeds. If the link drops afterwards, the device stays offline until it is
  power-cycled.
- **Credentials are not validated before being committed.** See
  [Credential save](#3-credential-save) above.
- **There is no way to re-provision at runtime.** No button, no API, no timeout. If the
  device is holding credentials for a network that no longer exists, the only recovery is
  `/nvserase` — which requires being on a network the device can no longer join — or a
  serial reflash.
- **An unprovisioned device reboots every 10 minutes.** A watchdog task starts with the
  provisioning AP and is never cancelled, so a user still typing a password at the
  ten-minute mark loses the session.
- **The Wi-Fi scan is blocking** and runs inside the HTTP task, so the portal is
  unresponsive for a few seconds during each scan, and there is no rate limit.
- **The AP advertises `200.200.200.1` as its gateway,** which nothing answers on, on a
  subnet that is public rather than RFC 1918 address space.
- **32-character SSIDs and 64-character PSKs are truncated by one character** and will not
  connect.
- **OTA needs an OTA partition table.** Without one, `/updateota` returns 500.
- **The captive portal is HTTP only.** HTTPS probes cannot be redirected by any captive
  portal.
- **Nothing is configurable at build time.** Timeouts, retry counts, the AP subnet, the
  watchdog period and the task stack sizes are all hard-coded literals; there is no
  `Kconfig` and no `idf_component.yml`.

---

## Roadmap

In rough priority order:

1. Close the provisioning endpoints outside a provisioning window, and authenticate what
   remains.
2. Keep the event handlers registered and supervise the link for the life of the device,
   with backoff and a path back into provisioning.
3. Validate credentials before committing them, and serve the result from a `/status`
   endpoint so the portal can show a real error.
4. Signed, pull-based OTA behind Secure Boot v2, with flash and NVS encryption.
5. Async Wi-Fi scanning, off the HTTP task.
6. A GPIO long-press to clear credentials and reopen the portal.
7. Move the hard-coded values to `Kconfig`; add a component manifest and a version.
8. Multiple stored networks with a last-known-good fallback.
9. Regulatory domain configuration (`wifi_country_t`) for multi-region use.
10. Possibly BLE provisioning, or adopting ESP-IDF's `wifi_provisioning` manager for its
    session encryption and proof-of-possession.

---

## Build

```bash
idf.py set-target esp32
idf.py build
```

## Flash

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

On Windows, replace the port with `COMx`.

## First boot

1. Flash the firmware and open the serial monitor.
2. Wait for the device to fall back to SoftAP mode.
3. Connect a phone or laptop to the device's access point.
4. Open `http://200.200.200.2/wifi` if the portal does not appear by itself.
5. Pick a network and enter the password.
6. The device saves the credentials and reboots.
7. On reboot it connects in station mode. If it comes back as an access point, the
   credentials were wrong — there is currently no error message for this.

## Erase and start over

```bash
idf.py erase-flash
```

---

## Debugging notes

Issues found and fixed during development, kept here because they are easy to hit again.

**Duplicate AP netif creation.** Calling `esp_netif_create_default_wifi_ap()` more than
once asserts at runtime. The AP netif is now held in a static pointer
(`s_wifi_ap_netif`) and reused.

**HTTP server socket limit.** ESP-IDF rejects an oversized socket configuration. The
server now sets an explicit `max_open_sockets` with `lru_purge_enable`.

**Blocking forever waiting for an IP.** Waiting indefinitely on `IP_EVENT_STA_GOT_IP`
locks the provisioning flow, so the wait is bounded:

```c
xSemaphoreTake(s_semph_get_ip_addrs, pdMS_TO_TICKS(30000));
```

**Restarting before the HTTP response is sent.** Calling `esp_restart()` directly in the
save handler means the browser never sees the confirmation. The handler now spawns a task
that waits 3 seconds, stops the services and then restarts:

```c
xTaskCreate(delayed_restart_task, "delayed_restart", 2048, NULL, tskIDLE_PRIORITY, NULL);
```

---

## What this repository is for

Wi-Fi lifecycle management, ESP-IDF event handling, NVS-backed configuration, SoftAP
fallback recovery, captive portal design, HTTP server resource limits, FreeRTOS
synchronisation, and failure-aware behaviour on the boot path.

It is a learning and demonstration project. Treat the security section above as the
boundary of what it is currently suitable for.

---

## License

GNU GPLv3. `dns_server.c` and `dns_server.h` are from Espressif's captive portal example
and remain under CC0-1.0 / Unlicense as marked in their headers.

> SPDX headers still need to be added to the project's own source files.

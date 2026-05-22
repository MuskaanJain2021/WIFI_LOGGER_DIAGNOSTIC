# ESP32 Wi-Fi Provisioning & Diagnostic Captive Portal

### ESP32 | ESP-IDF v5.4.2 | Wi-Fi Lifecycle Engine | SoftAP Fallback | Captive Portal | OTA-Ready Design

This project started with a simple goal:

> Connect an ESP32 to Wi-Fi and expose a local web page.

But real embedded systems rarely fail on the happy path.

Wi-Fi credentials may be missing.  
A saved SSID may no longer exist.  
A router may be powered off.  
A phone hotspot may change channels or sleep.  
A browser may submit malformed data.  
The ESP32 may reboot before an HTTP response reaches the client.  
NVS may contain stale configuration.  
A network interface may accidentally be initialized twice.

So this project evolved into a **fault-aware Wi-Fi provisioning and diagnostic engine** for ESP32.

It handles:

- STA auto-connect using credentials stored in NVS
- SoftAP fallback when STA connection fails
- Captive portal onboarding
- DNS redirect for provisioning flow
- Web-based Wi-Fi credential update
- Runtime heap and IP diagnostics
- HTTP server lifecycle handling
- OTA update endpoint support
- Delayed reboot after credential save
- NVS erase / factory-reset recovery path

This is not just a Wi-Fi demo.

It is a small example of how embedded firmware should behave when the network is unreliable.

---

## Why This Project Exists

Most Wi-Fi provisioning examples work only when everything goes right.

Product firmware has to survive the cases that do not.

- What if credentials are missing?
- What if saved credentials are wrong?
- What if the AP is not found?
- What if DHCP never gives an IP?
- What if the browser sends malformed data?
- What if the device restarts before the HTTP response is sent?
- What if the user needs to recover the device without UART access?
- What if the Wi-Fi stack, HTTP server, and provisioning AP race each other?

The design goal is simple:

> The ESP32 should never silently get stuck just because Wi-Fi failed.

---

## System Behaviour

### 1. STA Mode

If valid Wi-Fi credentials exist in NVS, the ESP32 boots into STA mode and attempts to connect to the saved access point.

Example log:

```text
Wi-Fi credentials loaded. SSID:'HomeRouter' Password:hidden
Wi-Fi interface up. Connecting ...
Connected to AP HomeRouter
Got IPv4 address: 192.168.1.45
Successfully connected to Wi-Fi.
HTTP server started in STA mode
Paste this after your current section:

````markdown
In this mode, the firmware can expose local diagnostics, status endpoints, and OTA support over the existing Wi-Fi network.

---

### 2. Wi-Fi Failure / SoftAP Fallback

If the saved Wi-Fi credentials are missing, invalid, or the access point cannot be found, the ESP32 falls back to SoftAP provisioning mode.

Example log:

```text
Disconnected from AP. reason=201, retry=6
IP address timeout. Starting Wi-Fi provisioning AP.
SoftAP started. Connect to SSID:'esp32-wifi-provision-care-example'
DHCP server started on interface WIFI_AP_DEF with IP: 200.200.200.2
Starting HTTP server on port: 80
````

In SoftAP mode, the ESP32 creates its own Wi-Fi network.
The user connects to this network and opens:

```text
http://200.200.200.2/wifi
```

From there, new Wi-Fi credentials can be submitted through the captive portal.

---

## High-Level Flow

```text
Boot
  ↓
Initialize NVS
  ↓
Load saved Wi-Fi credentials
  ↓
Credentials available?
  ├── Yes → Start STA mode
  │          ↓
  │       Connect to router
  │          ↓
  │       Got IP?
  │          ├── Yes → Normal diagnostic mode
  │          └── No  → Start SoftAP provisioning
  │
  └── No  → Start SoftAP provisioning
              ↓
          User opens captive portal
              ↓
          User submits Wi-Fi credentials
              ↓
          Save credentials to NVS
              ↓
          Delayed reboot
              ↓
          Boot again in STA mode
```

---

## Architecture

```text
Application Layer
        ↓
Wi-Fi Provisioning Engine
        ↓
Event Handlers
        ├── WIFI_EVENT_STA_START
        ├── WIFI_EVENT_STA_CONNECTED
        ├── WIFI_EVENT_STA_DISCONNECTED
        └── IP_EVENT_STA_GOT_IP
        ↓
Network Services
        ├── HTTP Server
        ├── DNS Redirect Server
        ├── DHCP Server
        └── mDNS
        ↓
ESP-IDF Wi-Fi Driver
        ↓
ESP32 Radio / TCP-IP Stack
```

---

## Key Firmware Features

### STA Auto-Connect

The firmware first checks whether Wi-Fi credentials already exist in NVS.

If credentials are available, ESP32 starts in STA mode and attempts to connect automatically.

### SoftAP Provisioning

If STA connection fails, ESP32 starts its own Wi-Fi access point.

This allows field recovery without UART, JTAG, or reflashing.

### Captive Portal

The firmware redirects browser traffic to the provisioning page using DNS redirect and HTTP 404 handling.

### POST-Based Credential Save

Wi-Fi credentials are submitted using HTTP POST instead of exposing them in the URL.

### Delayed Restart

After saving credentials, the device delays restart so the HTTP response can reach the browser before reboot.

### Runtime Diagnostics

The application periodically logs:

* minimum free heap
* STA IPv4 address
* IPv6 address information
* Wi-Fi connection state

### OTA-Ready Endpoint

The project includes an OTA update handler structure for future firmware updates through the local web interface.

---

## Failure Cases Handled

* Missing Wi-Fi credentials
* Wrong saved credentials
* AP not found
* DHCP timeout
* STA disconnect events
* SoftAP fallback
* Duplicate SoftAP initialization prevention
* HTTP server socket limit issue
* NVS erase / factory reset path
* Browser redirect to captive portal
* Safe restart after credential save

---

## Important Debug Findings

During development, several real firmware issues were debugged and fixed:

### 1. Duplicate AP Netif Creation

Calling:

```c
esp_netif_create_default_wifi_ap();
```

multiple times caused a runtime assert because the AP network interface was already created.

Fix:

```c
static esp_netif_t *s_ap_netif = NULL;
```

The AP netif is now owned and reused safely.

### 2. HTTP Server Socket Limit

ESP-IDF rejected an oversized HTTP server socket configuration.

Fix:

```c
config.max_open_sockets = 4;
```

### 3. Blocking Forever While Waiting for IP

Waiting forever for `IP_EVENT_STA_GOT_IP` can lock the provisioning flow.

Fix:

```c
xSemaphoreTake(s_semph_get_ip_addrs, pdMS_TO_TICKS(30000));
```

### 4. Restart Before HTTP Response

Restarting immediately after saving Wi-Fi credentials can prevent the browser from receiving a response.

Fix:

```c
esp_restart_after_3sec();
```

---

## How to Build

```bash
cd WIFI_LOGGER_DIAGNOSTIC
idf.py set-target esp32
idf.py build
```

---

## How to Flash

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

For Windows ESP-IDF terminal:

```bash
idf.py -p COMx flash monitor
```

Replace `COMx` with the actual serial port.

---

## How to Use

### First Boot / No Valid Wi-Fi

1. Flash the firmware.
2. Open serial monitor.
3. Wait for SoftAP mode.
4. Connect phone/laptop to the ESP32 SoftAP.
5. Open:

```text
http://200.200.200.2/wifi
```

6. Enter router credentials.
7. ESP32 saves credentials and restarts.
8. On reboot, ESP32 connects in STA mode.

---

## Useful Test Commands

### Build

```bash
idf.py build
```

### Erase Flash

```bash
idf.py erase-flash
```

### Flash and Monitor

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

### Check Git Status

```bash
git status
```

---

## Current Limitations

* OTA requires an OTA partition table for full production use.
* Captive portal currently uses HTTP, not HTTPS.
* `/nvserase` should be protected by a physical button or token before production use.
* Wi-Fi scan is currently blocking and can later be converted to async scan.
* A centralized Wi-Fi state machine can further improve lifecycle control.

---

## Planned Improvements

* Centralized Wi-Fi state machine
* Async Wi-Fi scanning
* Protected factory reset flow
* OTA partition table support
* MQTT/cloud diagnostic publishing
* WebSocket live diagnostics
* BLE provisioning fallback
* LittleFS-based diagnostic log storage

---

## Repository Focus

This project demonstrates embedded firmware thinking around:

* Wi-Fi lifecycle management
* ESP-IDF event handling
* NVS-based configuration persistence
* SoftAP fallback recovery
* Captive portal design
* HTTP server resource limits
* FreeRTOS synchronization
* Failure-aware product behavior

---

## License

GNU GPLV3
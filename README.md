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
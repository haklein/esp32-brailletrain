# ESP32-S3 Braille Trainer

A braille training device that connects an ESP32-S3 to a HandyTech BrailleWave 40-cell display via MAX232 level shifter. Includes a web UI for settings and monitoring, BLE HID pass-through for screen reader use, and maintenance tools.

## Hardware

- ESP32-S3-DevKitC-1
- HandyTech BrailleWave (40 cells, HT serial protocol)
- MAX232 level shifter (TTL ↔ RS-232)

```
ESP32 GPIO 7 (TX) → MAX232 → DB9 → BrailleWave
ESP32 GPIO 6 (RX) ← MAX232 ← DB9 ← BrailleWave
```

## Features

### Training
- Letter, word, and mixed training modes
- 26-level progressive curriculum (teaching order: e, a, i, o, s, h, b, c, ...)
- Oxford 5000 word list with frequency weights, filtered by introduced letters
- Per-letter statistics, confusion tracking, streak counting
- Auto-advancement when accuracy thresholds are met
- Confusable pair drills (d/f, e/i, h/j, m/n, etc.) plus actual confusion-based selection
- Per-letter confusion tracking across both letter and word modes (e.g., bed→beg records d/g confusion)
- Mirror mode (right-hand) and wide word spacing options
- Ergonomic positioning: left word at cell 12, mirror at cell 24, 4-cell gap
- Configurable max word length
- All settings persisted across reboots

### Web UI
- Real-time WebSocket updates at `http://brailletrain.local`
- Does not reveal the prompted letter/word — only shows result after input
- Level selector, mode switcher, option toggles
- Screen wake lock during training (releases after 5 min inactivity)
- Optional sound feedback: chime on correct, buzz on error (Web Audio API, no samples)
- Statistics panel: per-letter accuracy table, top confusion pairs, overall stats
- Reset option to clear all statistics and settings (e.g., for user change)
- BrailleWave connection status indicator with auto-reconnect

### WiFi
- Always runs AP mode (SSID: `BrailleTrain`, open)
- Optional STA mode — scan and connect to existing networks from the web UI
- Credentials saved to flash, auto-reconnects on boot
- mDNS: `http://brailletrain.local`

### BLE HID Braille Display
- Advertises as "BrailleWave 40" (BLE HID, appearance 0x03C9)
- When a screen reader (VoiceOver, TalkBack, BRLTTY, NVDA) connects via BLE, the device enters pass-through mode
- Host cell data → ESP32 → UART → BrailleWave display
- BrailleWave keys → UART → ESP32 → HID input reports → Host
- Auto-reverts to trainer mode on BLE disconnect
- HID reports: dot keys 1-8 + nav (Input 1), 40 router keys (Input 2), 40 cells (Output 3)

### Maintenance
- **Exercise mode**: flips all dots on/off at slow (1s) or fast (200ms) intervals for 5 or 15 minutes — for pin break-in and cleaning
- **Test mode**: cycles through every dot on every cell sequentially; reports all key presses/releases in the web UI
- **Auto-reconnect**: detects BrailleWave sleep/power-off after 30s inactivity, reconnects within 3s of power-on (uses HT ping when supported, falls back to reset)
- Manual reconnect button
- **Device probe**: queries serial number, firmware version, cell count, RTC, firmness, ping support via HT extended protocol
- **RTC sync**: synchronize BrailleWave clock from ESP32 (requires NTP via WiFi STA)
- **Firmness control**: adjust pin pressure (soft/medium/hard) on supported models

## Building

Requires [PlatformIO](https://platformio.org/).

```bash
pio run              # build
pio run -t upload    # flash
```

## License

Oxford 5000 word list: GPL-3.0-or-later (from Morserino-32 by Willi Kraml, OE1WKL).

/*
 * BLE HID Braille Display — pass-through bridge to HandyTech BrailleWave.
 *
 * When a BLE host (screen reader) connects, the ESP32 presents itself as
 * a 40-cell BLE HID braille display and bridges:
 *   Host cell data  → UART → BrailleWave
 *   BrailleWave keys → UART → Host HID input reports
 */
#pragma once

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEHIDDevice.h>
#include <BLE2902.h>
#include "braille_data.h"

// ---------------------------------------------------------------------------
// HID Report Descriptor — 40-cell braille display
// ---------------------------------------------------------------------------
//
//  Input Report 1 (2 bytes): dot keys 1-8, space, nav buttons
//  Input Report 2 (5 bytes): 40 router keys (1 bit each)
//  Output Report 3 (40 bytes): braille cell data from host

static const uint8_t BRL_REPORT_MAP[] = {
    0x05, 0x41,             // Usage Page (Braille Display)
    0x09, 0x01,             // Usage (Braille Display)
    0xA1, 0x01,             // Collection (Application)

    // --- Input Report 1: braille keyboard + nav ---
    0x85, 0x01,             //   Report ID (1)
    0x05, 0x41,             //   Usage Page (Braille Display)
    // Dots 1-8
    0x0A, 0x01, 0x02,       //   Usage (Braille Keyboard Dot 1)
    0x0A, 0x02, 0x02,       //   Usage (Braille Keyboard Dot 2)
    0x0A, 0x03, 0x02,       //   Usage (Braille Keyboard Dot 3)
    0x0A, 0x04, 0x02,       //   Usage (Braille Keyboard Dot 4)
    0x0A, 0x05, 0x02,       //   Usage (Braille Keyboard Dot 5)
    0x0A, 0x06, 0x02,       //   Usage (Braille Keyboard Dot 6)
    0x0A, 0x07, 0x02,       //   Usage (Braille Keyboard Dot 7)
    0x0A, 0x08, 0x02,       //   Usage (Braille Keyboard Dot 8)
    0x15, 0x00,             //   Logical Minimum (0)
    0x25, 0x01,             //   Logical Maximum (1)
    0x75, 0x01,             //   Report Size (1)
    0x95, 0x08,             //   Report Count (8)
    0x81, 0x02,             //   Input (Data, Var, Abs)
    // Space + nav (5 buttons) + 2 padding bits
    0x0A, 0x09, 0x02,       //   Usage (Braille Keyboard Space)
    0x0A, 0x0A, 0x02,       //   Usage (Braille Keyboard Left Space)
    0x0A, 0x0B, 0x02,       //   Usage (Braille Keyboard Right Space)
    0x0A, 0x13, 0x02,       //   Usage (Joystick Left)
    0x0A, 0x14, 0x02,       //   Usage (Joystick Right)
    0x95, 0x05,             //   Report Count (5)
    0x81, 0x02,             //   Input (Data, Var, Abs)
    // 3-bit padding
    0x95, 0x03,             //   Report Count (3)
    0x81, 0x03,             //   Input (Const)

    // --- Input Report 2: router keys ---
    0x85, 0x02,             //   Report ID (2)
    0x05, 0x41,             //   Usage Page (Braille Display)
    0x09, 0xFA,             //   Usage (Router Set 1)
    0xA1, 0x02,             //   Collection (Logical)
    0x0A, 0x00, 0x01,       //     Usage (Router Key)
    0x15, 0x00,             //     Logical Minimum (0)
    0x25, 0x01,             //     Logical Maximum (1)
    0x75, 0x01,             //     Report Size (1)
    0x95, 0x28,             //     Report Count (40)
    0x81, 0x02,             //     Input (Data, Var, Abs)
    0xC0,                   //   End Collection

    // --- Output Report 3: braille cells from host ---
    0x85, 0x03,             //   Report ID (3)
    0x05, 0x41,             //   Usage Page (Braille Display)
    0x09, 0x02,             //   Usage (Braille Row)
    0xA1, 0x02,             //   Collection (Logical)
    0x09, 0x03,             //     Usage (8 Dot Braille Cell)
    0x15, 0x00,             //     Logical Minimum (0)
    0x26, 0xFF, 0x00,       //     Logical Maximum (255)
    0x75, 0x08,             //     Report Size (8)
    0x95, 0x28,             //     Report Count (40)
    0x91, 0x02,             //     Output (Data, Var, Abs)
    0xC0,                   //   End Collection

    0xC0                    // End Collection
};

// ---------------------------------------------------------------------------
// Forward declarations — main.cpp provides these
// ---------------------------------------------------------------------------

extern HardwareSerial BRL;
extern HardwareSerial DBG;
static void ble_write_cells(const uint8_t *cells);  // defined below

// ---------------------------------------------------------------------------
// Pending cell buffer (BLE callback → main loop, thread safe)
// ---------------------------------------------------------------------------

static volatile bool ble_cells_pending = false;
static uint8_t ble_pending_cells[HT_CELLS];

// ---------------------------------------------------------------------------
// BLE callbacks
// ---------------------------------------------------------------------------

static bool ble_host_connected = false;

class BrlServerCB : public BLEServerCallbacks {
    void onConnect(BLEServer *s) override {
        ble_host_connected = true;
        DBG.println("BLE: host connected");
    }
    void onDisconnect(BLEServer *s) override {
        ble_host_connected = false;
        DBG.println("BLE: host disconnected");
        s->getAdvertising()->start();
    }
};

class CellOutputCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        size_t len = c->getLength();
        if (len >= HT_CELLS) {
            memcpy(ble_pending_cells, c->getData(), HT_CELLS);
            ble_cells_pending = true;
        }
    }
};

// ---------------------------------------------------------------------------
// BrailleHID — main interface
// ---------------------------------------------------------------------------

class BrailleHID {
public:
    void begin() {
        BLEDevice::init("BrailleWave 40");
        BLEDevice::setMTU(64);

        _server = BLEDevice::createServer();
        _server->setCallbacks(new BrlServerCB());

        _hid = new BLEHIDDevice(_server);

        // Reports
        _inputKeys   = _hid->inputReport(1);
        _inputRouter = _hid->inputReport(2);
        _outputCells = _hid->outputReport(3);
        _outputCells->setCallbacks(new CellOutputCB());

        // Device info
        _hid->manufacturer()->setValue("HandyTech");
        _hid->pnp(0x02, 0x1FE7, 0x0040, 0x0100);
        _hid->hidInfo(0x00, 0x02);  // not localized, normally connectable
        _hid->reportMap((uint8_t *)BRL_REPORT_MAP, sizeof(BRL_REPORT_MAP));

        // Security — Just Works pairing with bonding
        BLESecurity *sec = new BLESecurity();
        sec->setAuthenticationMode(ESP_LE_AUTH_BOND);
        sec->setCapability(ESP_IO_CAP_NONE);
        sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
        sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

        _hid->startServices();

        // Advertising
        BLEAdvertising *adv = _server->getAdvertising();
        adv->setAppearance(HID_BRAILLE_DISPLAY);
        adv->addServiceUUID(_hid->hidService()->getUUID());
        adv->setScanResponse(true);
        adv->setMinPreferred(0x06);
        adv->setMaxPreferred(0x12);
        adv->start();

        DBG.println("BLE: advertising as BrailleWave 40");
    }

    bool connected() const { return ble_host_connected; }

    // Send key report (dots + nav buttons)
    void sendKeys(uint8_t dots, uint8_t nav) {
        uint8_t report[2] = { dots, nav };
        _inputKeys->setValue(report, 2);
        _inputKeys->notify();
    }

    // Send router key press (momentary: set bit, then clear)
    void sendRouterKey(int key, bool pressed) {
        uint8_t report[5] = {0};
        if (pressed && key >= 0 && key < 40)
            report[key / 8] |= (1 << (key % 8));
        _inputRouter->setValue(report, 5);
        _inputRouter->notify();
    }

    // Process pending cell writes from BLE host (call from main loop)
    bool processPendingCells(uint8_t *out) {
        if (!ble_cells_pending) return false;
        ble_cells_pending = false;
        memcpy(out, ble_pending_cells, HT_CELLS);
        return true;
    }

private:
    BLEServer *_server = nullptr;
    BLEHIDDevice *_hid = nullptr;
    BLECharacteristic *_inputKeys = nullptr;
    BLECharacteristic *_inputRouter = nullptr;
    BLECharacteristic *_outputCells = nullptr;
};

// ---------------------------------------------------------------------------
// Pass-through key bridge: HT key events → HID input reports
// ---------------------------------------------------------------------------
// Maintains a live bitmap of pressed keys, sends HID report on each change.

static uint8_t pt_dot_bits = 0;   // bits 0-7 = dots 1-8
static uint8_t pt_nav_bits = 0;   // bit0=space, bit1=navL, bit2=navR, bit3=navP, bit4=navN

// Map HT key code → (byte_index, bit_mask) in the 2-byte HID report
struct PTKeyMap { uint8_t ht_code; int byte_idx; uint8_t bit; };
static const PTKeyMap PT_KEYS[] = {
    {0x0F, 0, 0},  // dot 1
    {0x0B, 0, 1},  // dot 2
    {0x07, 0, 2},  // dot 3
    {0x13, 0, 3},  // dot 4
    {0x17, 0, 4},  // dot 5
    {0x1B, 0, 5},  // dot 6
    {0x03, 0, 6},  // dot 7
    {0x1F, 0, 7},  // dot 8
    {0x10, 1, 0},  // space
    {0x0C, 1, 1},  // nav left  → Left Space
    {0x14, 1, 2},  // nav right → Right Space
    {0x08, 1, 3},  // nav prev  → Joystick Left
    {0x04, 1, 4},  // nav next  → Joystick Right
};
#define PT_KEYS_N (sizeof(PT_KEYS) / sizeof(PT_KEYS[0]))

static void passthrough_poll(BrailleHID &hid) {
    bool changed = false;
    while (BRL.available()) {
        int b = BRL.read();
        bool release = b & 0x80;
        uint8_t code = b & 0x7F;

        // Router key
        if (code >= 0x20 && code < 0x48) {
            hid.sendRouterKey(code - 0x20, !release);
            continue;
        }

        // Dot / nav key
        for (int i = 0; i < (int)PT_KEYS_N; i++) {
            if (PT_KEYS[i].ht_code == code) {
                uint8_t *byte = (PT_KEYS[i].byte_idx == 0) ? &pt_dot_bits : &pt_nav_bits;
                if (!release)
                    *byte |= (1 << PT_KEYS[i].bit);
                else
                    *byte &= ~(1 << PT_KEYS[i].bit);
                changed = true;
                break;
            }
        }
    }
    if (changed)
        hid.sendKeys(pt_dot_bits, pt_nav_bits);
}

static void passthrough_reset_keys() {
    pt_dot_bits = 0;
    pt_nav_bits = 0;
}

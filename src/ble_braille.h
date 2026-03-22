/*
 * BLE HID Braille Display — dual-protocol bridge to HandyTech BrailleWave.
 *
 * Exposes TWO protocols on a single BLE HID service:
 *
 * 1) Standard HID Braille Display (Usage Page 0x41, reports 0x11-0x13)
 *    For iOS VoiceOver, Android TalkBack, and other standard consumers.
 *
 * 2) HandyTech USB-HID serial tunnel (reports 0x01, 0x02, 0xFB, 0xFC)
 *    For brltty's 'ht' driver. Wraps HT serial frames in HID reports.
 *
 * When a BLE host connects, the ESP32 bridges both simultaneously:
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
// Report IDs
// ---------------------------------------------------------------------------

// Standard HID Braille Display (Usage Page 0x41)
#define RPT_STD_KEYS     0x11   // Input:  2B dot/nav key bits
#define RPT_STD_ROUTER   0x12   // Input:  5B routing key bits
#define RPT_STD_CELLS    0x13   // Output: 40B cell data

// HandyTech USB-HID serial tunnel (for brltty ht driver)
#define RPT_HT_OUT_DATA  0x01   // Input (GET_REPORT): HT serial from device
#define RPT_HT_IN_DATA   0x02   // Output (SET_REPORT): HT serial to device
#define RPT_HT_IN_CMD    0xFB   // Output (SET_REPORT): firmware command
#define RPT_HT_OUT_VER   0xFC   // Input (GET_REPORT): firmware version

// HT USB-HID data report payload size (matches real HT USB-HID adapters)
// Format: [report_id] [length] [data...] padded to fixed size
#define HT_HID_DATA_SIZE 64

// ---------------------------------------------------------------------------
// HID Report Descriptor — dual protocol
// ---------------------------------------------------------------------------
//
// Collection 1: Standard HID Braille (Usage Page 0x41)
//   Input Report 0x11 (2 bytes): dot keys 1-8, space, nav buttons
//   Input Report 0x12 (5 bytes): 40 router keys (1 bit each)
//   Output Report 0x13 (40 bytes): braille cell data from host
//
// Collection 2: HandyTech USB-HID serial tunnel (Usage Page 0x01 Generic)
//   Input Report 0x01 (64 bytes): HT serial data from device
//   Output Report 0x02 (64 bytes): HT serial data to device
//   Output Report 0xFB (2 bytes): firmware command
//   Input Report 0xFC (3 bytes): firmware version

static const uint8_t BRL_REPORT_MAP[] = {
    // =================================================================
    // Collection 1: Standard HID Braille Display
    // =================================================================
    0x05, 0x41,             // Usage Page (Braille Display)
    0x09, 0x01,             // Usage (Braille Display)
    0xA1, 0x01,             // Collection (Application)

    // --- Input Report 0x11: braille keyboard + nav ---
    0x85, RPT_STD_KEYS,     //   Report ID (0x11)
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
    // Space + nav (5 buttons) + 3 padding bits
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

    // --- Input Report 0x12: router keys ---
    0x85, RPT_STD_ROUTER,   //   Report ID (0x12)
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

    // --- Output Report 0x13: braille cells from host ---
    0x85, RPT_STD_CELLS,    //   Report ID (0x13)
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

    0xC0,                   // End Collection (Standard Braille)

    // =================================================================
    // Collection 2: HandyTech USB-HID serial tunnel
    // =================================================================
    0x06, 0x00, 0xFF,       // Usage Page (Vendor Defined 0xFF00)
    0x09, 0x01,             // Usage (Vendor Usage 1)
    0xA1, 0x01,             // Collection (Application)

    // --- Report 0x01: HT serial data from device (brltty polls via GET_REPORT) ---
    0x85, RPT_HT_OUT_DATA,  //   Report ID (0x01)
    0x09, 0x01,             //   Usage (Vendor Data Out)
    0x15, 0x00,             //   Logical Minimum (0)
    0x26, 0xFF, 0x00,       //   Logical Maximum (255)
    0x75, 0x08,             //   Report Size (8)
    0x95, HT_HID_DATA_SIZE, //   Report Count (64)
    0x81, 0x02,             //   Input (Data, Var, Abs)

    // --- Report 0x02: HT serial data to device (brltty sends via SET_REPORT) ---
    0x85, RPT_HT_IN_DATA,   //   Report ID (0x02)
    0x09, 0x02,             //   Usage (Vendor Data In)
    0x15, 0x00,             //   Logical Minimum (0)
    0x26, 0xFF, 0x00,       //   Logical Maximum (255)
    0x75, 0x08,             //   Report Size (8)
    0x95, HT_HID_DATA_SIZE, //   Report Count (64)
    0x91, 0x02,             //   Output (Data, Var, Abs)

    // --- Report 0xFB: firmware command (flush buffers etc.) ---
    0x85, RPT_HT_IN_CMD,    //   Report ID (0xFB)
    0x09, 0x03,             //   Usage (Vendor Command)
    0x15, 0x00,             //   Logical Minimum (0)
    0x26, 0xFF, 0x00,       //   Logical Maximum (255)
    0x75, 0x08,             //   Report Size (8)
    0x95, 0x02,             //   Report Count (2)
    0x91, 0x02,             //   Output (Data, Var, Abs)

    // --- Report 0xFC: firmware version (brltty reads via GET_REPORT) ---
    0x85, RPT_HT_OUT_VER,   //   Report ID (0xFC)
    0x09, 0x04,             //   Usage (Vendor Version)
    0x15, 0x00,             //   Logical Minimum (0)
    0x26, 0xFF, 0x00,       //   Logical Maximum (255)
    0x75, 0x08,             //   Report Size (8)
    0x95, 0x03,             //   Report Count (3)
    0x81, 0x02,             //   Input (Data, Var, Abs)

    0xC0                    // End Collection (HT USB-HID)
};

// ---------------------------------------------------------------------------
// Forward declarations — main.cpp provides these
// ---------------------------------------------------------------------------

extern HardwareSerial BRL;
#ifndef DBG
#define DBG Serial
#endif

// ---------------------------------------------------------------------------
// Pending buffers (BLE callback → main loop, thread safe)
// ---------------------------------------------------------------------------

// Standard Braille cells (from report 0x13)
static volatile bool ble_cells_pending = false;
static uint8_t ble_pending_cells[HT_CELLS];

// HT USB-HID serial data (from report 0x02 — brltty sends HT commands)
static volatile bool ble_ht_data_pending = false;
static uint8_t ble_ht_pending_data[HT_HID_DATA_SIZE];
static volatile uint8_t ble_ht_pending_len = 0;

// HT USB-HID command (from report 0xFB)
static volatile bool ble_ht_cmd_pending = false;
static volatile uint8_t ble_ht_pending_cmd = 0;

// HT serial data received from UART, buffered for brltty GET_REPORT(0x01)
static uint8_t ht_uart_rx_buf[HT_HID_DATA_SIZE];
static volatile uint8_t ht_uart_rx_len = 0;

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

// Standard Braille cells (report 0x13)
class CellOutputCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        size_t len = c->getLength();
        if (len >= HT_CELLS) {
            memcpy(ble_pending_cells, c->getData(), HT_CELLS);
            ble_cells_pending = true;
        }
    }
};

// HT USB-HID serial data from brltty (report 0x02)
// Format: [report_id] [length] [HT serial bytes...]
class HtDataOutputCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        const uint8_t *data = c->getData();
        size_t len = c->getLength();
        // data[0] = length of HT serial payload
        // data[1..] = raw HT serial bytes
        if (len >= 2 && data[0] > 0) {
            uint8_t payload_len = data[0];
            if (payload_len > sizeof(ble_ht_pending_data) - 1)
                payload_len = sizeof(ble_ht_pending_data) - 1;
            memcpy(ble_ht_pending_data, &data[1], payload_len);
            ble_ht_pending_len = payload_len;
            ble_ht_data_pending = true;
        }
    }
};

// HT USB-HID firmware command (report 0xFB)
class HtCmdOutputCB : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *c) override {
        const uint8_t *data = c->getData();
        size_t len = c->getLength();
        if (len >= 1) {
            ble_ht_pending_cmd = data[0];
            ble_ht_cmd_pending = true;
        }
    }
};

// HT USB-HID OutData read (report 0x01) — return buffered UART data
class HtDataInputCB : public BLECharacteristicCallbacks {
    void onRead(BLECharacteristic *c) override {
        // Prepare report: [length] [data...] zero-padded to HT_HID_DATA_SIZE
        uint8_t report[HT_HID_DATA_SIZE];
        memset(report, 0, sizeof(report));
        if (ht_uart_rx_len > 0) {
            uint8_t n = ht_uart_rx_len;
            if (n > HT_HID_DATA_SIZE - 1) n = HT_HID_DATA_SIZE - 1;
            report[0] = n;
            memcpy(&report[1], ht_uart_rx_buf, n);
            ht_uart_rx_len = 0;  // consumed
        }
        c->setValue(report, sizeof(report));
    }
};

// HT USB-HID version read (report 0xFC) — return firmware version
class HtVersionInputCB : public BLECharacteristicCallbacks {
    void onRead(BLECharacteristic *c) override {
        // [report_id] [major] [minor]
        uint8_t report[3] = { RPT_HT_OUT_VER, 1, 0 };  // version 1.0
        c->setValue(report, sizeof(report));
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

        // --- Standard Braille reports ---
        _inputKeys   = _hid->inputReport(RPT_STD_KEYS);
        _inputRouter = _hid->inputReport(RPT_STD_ROUTER);
        _outputCells = _hid->outputReport(RPT_STD_CELLS);
        _outputCells->setCallbacks(new CellOutputCB());

        // --- HT USB-HID reports ---
        _htOutData = _hid->inputReport(RPT_HT_OUT_DATA);
        _htOutData->setCallbacks(new HtDataInputCB());

        _htInData = _hid->outputReport(RPT_HT_IN_DATA);
        _htInData->setCallbacks(new HtDataOutputCB());

        _htInCmd = _hid->outputReport(RPT_HT_IN_CMD);
        _htInCmd->setCallbacks(new HtCmdOutputCB());

        _htOutVer = _hid->inputReport(RPT_HT_OUT_VER);
        _htOutVer->setCallbacks(new HtVersionInputCB());

        // Device info — use HandyTech vendor for ht driver auto-detection
        // ESP32 BLE lib byte-swaps 16-bit PnP values, so pre-swap:
        // vendor 0x1FE4 → 0xE41F, product 0x0003 → 0x0300, version 0x0100 → 0x0001
        _hid->manufacturer()->setValue("HandyTech");
        _hid->pnp(0x02, 0xE41F, 0x0300, 0x0001);  // HT USB-HID adapter (pre-swapped)
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

        DBG.println("BLE: advertising as BrailleWave 40 (dual protocol)");
    }

    bool connected() const { return ble_host_connected; }

    // --- Standard Braille interface ---

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
    // Handles both standard (0x13) and HT (0x02) cell writes
    bool processPendingCells(uint8_t *out) {
        if (!ble_cells_pending) return false;
        ble_cells_pending = false;
        memcpy(out, ble_pending_cells, HT_CELLS);
        return true;
    }

    // --- HT USB-HID interface ---

    // Buffer UART bytes from BrailleWave for brltty GET_REPORT(0x01)
    void htBufferUartByte(uint8_t b) {
        if (ht_uart_rx_len < HT_HID_DATA_SIZE - 1) {
            ht_uart_rx_buf[ht_uart_rx_len++] = b;
        }
    }

    // Notify brltty of available data via input report 0x01
    void htNotifyData() {
        if (ht_uart_rx_len == 0) return;
        uint8_t report[HT_HID_DATA_SIZE];
        memset(report, 0, sizeof(report));
        uint8_t n = ht_uart_rx_len;
        if (n > HT_HID_DATA_SIZE - 1) n = HT_HID_DATA_SIZE - 1;
        report[0] = n;
        memcpy(&report[1], ht_uart_rx_buf, n);
        ht_uart_rx_len = 0;  // consumed — clear buffer
        _htOutData->setValue(report, sizeof(report));
        _htOutData->notify();
    }

    // Process HT serial data from brltty → forward to UART
    bool processHtData(uint8_t *out, uint8_t *out_len) {
        if (!ble_ht_data_pending) return false;
        ble_ht_data_pending = false;
        *out_len = ble_ht_pending_len;
        memcpy(out, ble_ht_pending_data, ble_ht_pending_len);
        return true;
    }

    // Process HT firmware command from brltty
    bool processHtCommand(uint8_t *cmd) {
        if (!ble_ht_cmd_pending) return false;
        ble_ht_cmd_pending = false;
        *cmd = ble_ht_pending_cmd;
        return true;
    }

private:
    BLEServer *_server = nullptr;
    BLEHIDDevice *_hid = nullptr;
    // Standard Braille
    BLECharacteristic *_inputKeys = nullptr;
    BLECharacteristic *_inputRouter = nullptr;
    BLECharacteristic *_outputCells = nullptr;
    // HT USB-HID
    BLECharacteristic *_htOutData = nullptr;
    BLECharacteristic *_htInData = nullptr;
    BLECharacteristic *_htInCmd = nullptr;
    BLECharacteristic *_htOutVer = nullptr;
};

// ---------------------------------------------------------------------------
// Pass-through key bridge: HT key events → HID input reports
// ---------------------------------------------------------------------------
// Maintains a live bitmap of pressed keys, sends HID report on each change.
// UART bytes are also buffered for the HT USB-HID interface (brltty).

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
    bool ht_buffered = false;
    while (BRL.available()) {
        int b = BRL.read();

        // Always buffer for HT USB-HID interface (brltty reads via report 0x01)
        hid.htBufferUartByte((uint8_t)b);
        ht_buffered = true;

        bool release = b & 0x80;
        uint8_t code = b & 0x7F;

        // Router key → standard braille report
        if (code >= 0x20 && code < 0x48) {
            hid.sendRouterKey(code - 0x20, !release);
            continue;
        }

        // Dot / nav key → standard braille report
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

    // Notify brltty of buffered UART data
    if (ht_buffered)
        hid.htNotifyData();

    // Process HT serial data from brltty → forward to UART
    uint8_t ht_data[HT_HID_DATA_SIZE];
    uint8_t ht_len = 0;
    if (hid.processHtData(ht_data, &ht_len)) {
        BRL.write(ht_data, ht_len);
        BRL.flush();
    }

    // Process HT firmware commands from brltty
    uint8_t cmd = 0;
    if (hid.processHtCommand(&cmd)) {
        if (cmd == 0x01) {
            // Flush buffers
            ht_uart_rx_len = 0;
            while (BRL.available()) BRL.read();
            DBG.println("BLE-HT: flush buffers");
        }
    }
}

static void passthrough_reset_keys() {
    pt_dot_bits = 0;
    pt_nav_bits = 0;
    ht_uart_rx_len = 0;
}

/*
 * ESP32-S3 Braille Trainer with web UI
 *
 * Hardware:
 *   BRL_TX -> MAX232 RX pin -> DB9 -> BrailleWave
 *   BRL_RX <- MAX232 TX pin <- DB9 <- BrailleWave
 *   USB CDC -> PC serial monitor at 115200
 *   Pin assignments per board in platformio.ini
 *
 * WiFi AP "BrailleTrain" -> http://192.168.4.1
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#include "braille_data.h"
#include "progress.h"
#include "engine.h"
#include "wordlist.h"
#include "html_content.h"
#include "ble_braille.h"

// =========================================================================
// Hardware
// =========================================================================

#define DBG Serial  // USB CDC on ESP32-S3

// BRL_TX and BRL_RX defined via build flags in platformio.ini
HardwareSerial BRL(1);

// Status LED (optional, per-board via platformio.ini)
//   LED_PIN  = simple GPIO LED (active low, XIAO)
//   LED_RGB  = addressable WS2812 (DevKitC-1, GPIO48)
#if defined(LED_RGB)
static inline void led_on()      { neopixelWrite(LED_RGB, 0, 20, 0); }  // green
static inline void led_attempt() { neopixelWrite(LED_RGB, 20, 12, 0); } // yellow
static inline void led_off()     { neopixelWrite(LED_RGB, 0, 0, 0); }
#elif defined(LED_PIN)
static inline void led_on()      { digitalWrite(LED_PIN, LOW); }  // active low
static inline void led_attempt() { digitalWrite(LED_PIN, LOW); }
static inline void led_off()     { digitalWrite(LED_PIN, HIGH); }
#else
static inline void led_on()      {}
static inline void led_attempt() {}
static inline void led_off()     {}
#endif

#define HT_PKT_RESET    0xFF
#define HT_PKT_OK       0xFE
#define HT_PKT_BRAILLE  0x01
#define HT_PKT_EXTENDED 0x79
#define HT_MODEL_WAVE   0x05
#define HT_SYN          0x16

// Extended packet types
#define HT_EXT_BRAILLE          0x01
#define HT_EXT_KEY              0x04
#define HT_EXT_CONFIRMATION     0x07
#define HT_EXT_PING             0x19
#define HT_EXT_GET_SERIAL       0x41
#define HT_EXT_SET_RTC          0x44
#define HT_EXT_GET_RTC          0x45
#define HT_EXT_SET_FIRMNESS     0x60
#define HT_EXT_GET_FIRMNESS     0x61
#define HT_EXT_GET_PROTO_PROPS  0xC1
#define HT_EXT_GET_FW_VERSION   0xC2

// =========================================================================
// Web server
// =========================================================================

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// =========================================================================
// Settings (changed via web or defaults)
// =========================================================================

static volatile bool mirror_enabled = false;
static volatile bool word_spacing = false;
static volatile bool keepalive_enabled = true;
static volatile bool ergonomic_enabled = false;
static volatile int max_word_len = 0;  // 0 = unlimited

enum TrainMode { MODE_LETTERS, MODE_WORDS, MODE_MIXED };
static volatile int training_mode = MODE_LETTERS;

// BrailleWave connectivity
static bool brl_connected = false;
static unsigned long last_key_time = 0;
static unsigned long last_keepalive = 0;
static volatile bool pending_reconnect = false;

// Pending settings from WebSocket (processed in main loop)
static volatile int pending_level = -1;

// Pending WiFi commands from WebSocket
static volatile bool pending_wifi_scan = false;
static volatile bool pending_wifi_connect = false;
static volatile bool pending_wifi_disconnect = false;
static char pending_ssid[33] = {0};
static char pending_pass[65] = {0};

// =========================================================================
// Key codes
// =========================================================================

struct KeyInfo { uint8_t code; int dot; };

static const KeyInfo KEY_TABLE[] = {
    {0x03, 7}, {0x07, 3}, {0x0B, 2}, {0x0F, 1},
    {0x13, 4}, {0x17, 5}, {0x1B, 6}, {0x1F, 8},
    {0x10, -1},  // space
    {0x0C, -2},  // nav left
    {0x14, -3},  // nav right
    {0x08, -4},  // nav prev
    {0x04, -5},  // nav next
};
#define KEY_TABLE_SIZE (sizeof(KEY_TABLE) / sizeof(KEY_TABLE[0]))

#define DOT_KEY(ki)  ((ki).dot > 0)
#define NAV_LEFT   0x0C
#define NAV_RIGHT  0x14
#define NAV_PREV   0x08
#define NAV_NEXT   0x04
#define KEY_SPACE  0x10

// =========================================================================
// HT protocol
// =========================================================================

static bool ht_reset() {
    DBG.println("HT reset...");
    led_attempt();
    while (BRL.available()) BRL.read();
    BRL.write((uint8_t)HT_PKT_RESET);
    BRL.flush();
    delay(300);

    uint8_t buf[16];
    int count = 0;
    unsigned long deadline = millis() + 1000;
    while (millis() < deadline && count < 16)
        if (BRL.available()) buf[count++] = BRL.read();

    for (int i = 0; i < count; i++)
        if (buf[i] == HT_PKT_OK) {
            DBG.println("  BrailleWave OK");
            led_on();  // stay solid
            return true;
        }
    led_off();
    return false;
}

static void ht_write_cells(const uint8_t *cells) {
    BRL.write((uint8_t)HT_PKT_BRAILLE);
    BRL.write(cells, HT_CELLS);
    BRL.flush();
    delay(50);
    while (BRL.available()) BRL.read();
}

static void ht_clear() {
    uint8_t cells[HT_CELLS] = {0};
    ht_write_cells(cells);
}

// Send an extended packet: type + optional data
static void ht_send_ext(uint8_t type, const uint8_t *data = nullptr, int data_len = 0) {
    BRL.write((uint8_t)HT_PKT_EXTENDED);
    BRL.write((uint8_t)HT_MODEL_WAVE);
    BRL.write((uint8_t)(data_len + 1));  // length includes type byte
    BRL.write(type);
    if (data && data_len > 0) BRL.write(data, data_len);
    BRL.write((uint8_t)HT_SYN);
    BRL.flush();
}

// Read an extended packet response. Returns payload length (0 = no response).
// out_type gets the response type, out_data gets payload (up to out_max bytes).
static int ht_recv_ext(uint8_t *out_type, uint8_t *out_data, int out_max,
                       int timeout_ms = 500) {
    unsigned long deadline = millis() + timeout_ms;
    // Look for 0x79 header
    while (millis() < deadline) {
        if (!BRL.available()) { delay(1); continue; }
        int b = BRL.read();
        if (b != HT_PKT_EXTENDED) continue;

        // Read model, length, type
        while (BRL.available() < 3 && millis() < deadline) delay(1);
        if (BRL.available() < 3) return 0;
        uint8_t model = BRL.read();
        uint8_t len = BRL.read();     // includes type byte
        *out_type = BRL.read();
        int payload_len = len - 1;

        // Read payload + SYN
        int got = 0;
        while (got < payload_len && millis() < deadline) {
            if (BRL.available()) {
                uint8_t c = BRL.read();
                if (got < out_max) out_data[got] = c;
                got++;
            } else delay(1);
        }
        // Read trailing SYN
        while (millis() < deadline) {
            if (BRL.available()) { BRL.read(); break; }
            delay(1);
        }
        return min(got, out_max);
    }
    return 0;
}

// Ping — less disruptive than full reset for keepalive
static bool ht_ping() {
    while (BRL.available()) BRL.read();
    ht_send_ext(HT_EXT_PING);
    // Accept any response (extended confirmation, or even just bytes back)
    unsigned long deadline = millis() + 300;
    while (millis() < deadline) {
        if (BRL.available()) {
            while (BRL.available()) BRL.read();
            return true;
        }
        delay(5);
    }
    return false;
}

// Probe device: try all extended queries, return results as JSON
static void ht_probe(String &out) {
    JsonDocument doc;
    doc["t"] = "devinfo";
    uint8_t rtype, rbuf[64];
    int rlen;

    // Serial number
    while (BRL.available()) BRL.read();
    ht_send_ext(HT_EXT_GET_SERIAL);
    rlen = ht_recv_ext(&rtype, rbuf, sizeof(rbuf));
    if (rlen > 0) {
        rbuf[min(rlen, 63)] = 0;
        doc["serial"] = (const char *)rbuf;
        DBG.printf("HT serial: %s\n", rbuf);
    }

    // Firmware version
    while (BRL.available()) BRL.read();
    ht_send_ext(HT_EXT_GET_FW_VERSION);
    rlen = ht_recv_ext(&rtype, rbuf, sizeof(rbuf));
    if (rlen > 0) {
        rbuf[min(rlen, 63)] = 0;
        doc["firmware"] = (const char *)rbuf;
        DBG.printf("HT firmware: %s\n", rbuf);
    }

    // Protocol properties (cell count)
    while (BRL.available()) BRL.read();
    ht_send_ext(HT_EXT_GET_PROTO_PROPS);
    rlen = ht_recv_ext(&rtype, rbuf, sizeof(rbuf));
    if (rlen > 0) {
        doc["cells"] = (int)rbuf[0];
        DBG.printf("HT cells: %d\n", rbuf[0]);
    }

    // Firmness
    while (BRL.available()) BRL.read();
    ht_send_ext(HT_EXT_GET_FIRMNESS);
    rlen = ht_recv_ext(&rtype, rbuf, sizeof(rbuf));
    if (rlen > 0) {
        doc["firmness"] = (int)rbuf[0];
        doc["has_firmness"] = true;
        DBG.printf("HT firmness: %d\n", rbuf[0]);
    }

    // RTC
    while (BRL.available()) BRL.read();
    ht_send_ext(HT_EXT_GET_RTC);
    rlen = ht_recv_ext(&rtype, rbuf, sizeof(rbuf));
    if (rlen >= 7) {
        int year = (rbuf[0] << 8) | rbuf[1];
        char ts[32];
        snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
                 year, rbuf[2], rbuf[3], rbuf[4], rbuf[5], rbuf[6]);
        doc["rtc"] = ts;
        doc["has_rtc"] = true;
        DBG.printf("HT RTC: %s\n", ts);
    }

    // Ping
    bool ping_ok = ht_ping();
    doc["ping"] = ping_ok;

    serializeJson(doc, out);
}

// =========================================================================
// Display helpers (mirror + spacing)
// =========================================================================

static void display_cells(const uint8_t *src, int src_len) {
    uint8_t cells[HT_CELLS] = {0};

    int left = ergonomic_enabled ? 12 : 0;

    if (mirror_enabled) {
        // Right (mirror) copy: prefer cell 24, push left only if it won't fit
        int right = 24;
        if (right + src_len > HT_CELLS)
            right = HT_CELLS - src_len;
        if (right < 0) right = 0;

        // Left copy: push left if 4-cell gap can't be maintained
        if (left + src_len + 4 > right)
            left = right - src_len - 4;
        if (left < 0) left = 0;

        for (int i = 0; i < src_len && right + i < HT_CELLS; i++)
            cells[right + i] = src[i];
    } else {
        // No mirror: ensure it fits
        if (left + src_len > HT_CELLS)
            left = HT_CELLS - src_len;
        if (left < 0) left = 0;
    }

    for (int i = 0; i < src_len && left + i < HT_CELLS; i++)
        cells[left + i] = src[i];

    ht_write_cells(cells);
}

static void display_letter(char letter) {
    uint8_t c[1] = { char_to_cell(letter) };
    display_cells(c, 1);
}

static int word_to_cells(const char *word, uint8_t *out, int max_cells) {
    int pos = 0;
    for (int i = 0; word[i] && pos < max_cells; i++) {
        if (i > 0 && word_spacing) {
            if (pos < max_cells) out[pos++] = 0x00;
        }
        if (pos < max_cells) out[pos++] = char_to_cell(word[i]);
    }
    return pos;
}

static void display_word(const char *word) {
    uint8_t cells[HT_CELLS] = {0};
    int n = word_to_cells(word, cells, mirror_enabled ? HT_CELLS / 2 : HT_CELLS);
    display_cells(cells, n);
}

static void display_comparison(uint8_t got_cell, uint8_t exp_cell) {
    uint8_t cells[4] = { got_cell, 0, exp_cell, 0 };
    display_cells(cells, 4);
}

static void display_word_progress(const char *typed, int typed_len) {
    uint8_t cells[HT_CELLS] = {0};
    int pos = 0;
    for (int i = 0; i < typed_len; i++) {
        if (i > 0 && word_spacing) {
            if (pos < HT_CELLS) cells[pos++] = 0x00;
        }
        if (pos < HT_CELLS) cells[pos++] = char_to_cell(typed[i]);
    }
    display_cells(cells, pos);
}

// =========================================================================
// Chord accumulator
// =========================================================================

static uint8_t pressed_keys[16];
static int pressed_n = 0;
static uint8_t chord_keys[16];
static int chord_n = 0;

static bool chord_ready = false;
static uint8_t chord_dots = 0;
static bool chord_is_space = false;
static uint8_t chord_nav = 0;
static int routing_key = -1;

static const KeyInfo *lookup_key(uint8_t code) {
    for (int i = 0; i < (int)KEY_TABLE_SIZE; i++)
        if (KEY_TABLE[i].code == code) return &KEY_TABLE[i];
    return NULL;
}

static void finish_chord() {
    if (chord_n == 0) return;
    chord_dots = 0;
    chord_is_space = false;
    chord_nav = 0;

    for (int i = 0; i < chord_n; i++) {
        const KeyInfo *ki = lookup_key(chord_keys[i]);
        if (ki) {
            if (ki->dot > 0) chord_dots |= (1 << (ki->dot - 1));
            else if (chord_keys[i] == KEY_SPACE) chord_is_space = true;
            else chord_nav = chord_keys[i];
        }
    }
    chord_ready = true;
    chord_n = 0;
}

static void drain_ext_packet() {
    // Consume model + len, then len bytes + trailing SYN
    unsigned long deadline = millis() + 100;
    while (BRL.available() < 2 && millis() < deadline) delay(1);
    if (BRL.available() < 2) return;
    BRL.read();                         // model
    uint8_t len = BRL.read();           // length (includes type)
    int remain = len + 1;               // data + SYN
    while (remain > 0 && millis() < deadline) {
        if (BRL.available()) { BRL.read(); remain--; }
        else delay(1);
    }
}

static void poll_keys() {
    while (BRL.available()) {
        last_key_time = millis();
        int b = BRL.read();

        // Skip extended packets (0x79 model len type data... SYN)
        if (b == HT_PKT_EXTENDED) { drain_ext_packet(); continue; }

        // Skip ack bytes (0xFE followed by model)
        if (b == HT_PKT_OK) {
            unsigned long t = millis();
            while (!BRL.available() && millis() - t < 10) {}
            if (BRL.available()) BRL.read();  // model byte
            continue;
        }

        bool release = b & 0x80;
        uint8_t code = b & 0x7F;

        if (code >= 0x20 && code < 0x48) {
            if (!release) routing_key = code - 0x20;
            continue;
        }
        if (!lookup_key(code)) continue;

        if (!release) {
            bool already = false;
            for (int i = 0; i < pressed_n; i++)
                if (pressed_keys[i] == code) already = true;
            if (!already && pressed_n < 16) pressed_keys[pressed_n++] = code;

            already = false;
            for (int i = 0; i < chord_n; i++)
                if (chord_keys[i] == code) already = true;
            if (!already && chord_n < 16) chord_keys[chord_n++] = code;
        } else {
            for (int i = 0; i < pressed_n; i++) {
                if (pressed_keys[i] == code) {
                    pressed_keys[i] = pressed_keys[--pressed_n];
                    break;
                }
            }
            if (pressed_n == 0) finish_chord();
        }
    }
}

static void chord_reset() {
    chord_ready = false;
    chord_dots = 0;
    chord_is_space = false;
    chord_nav = 0;
    chord_n = 0;
    pressed_n = 0;
    routing_key = -1;
}

// =========================================================================
// WebSocket notifications
// =========================================================================

static void ws_send(const char *json) {
    ws.textAll(json);
}

static void ws_notify_prompt(const char *item, bool is_word) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"t\":\"prompt\",\"s\":\"%s\",\"w\":%s}",
             item, is_word ? "true" : "false");
    ws_send(buf);
}

static void ws_notify_correct(const char *item, bool is_word) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"t\":\"ok\",\"s\":\"%s\",\"w\":%s}",
             item, is_word ? "true" : "false");
    ws_send(buf);
}

static void ws_notify_letter_incorrect(char expected, char got,
                                       uint8_t got_dots, uint8_t exp_dots) {
    char buf[128];
    char es[2] = {expected, 0}, gs[2] = {got ? got : '?', 0};
    snprintf(buf, sizeof(buf),
             "{\"t\":\"no\",\"s\":\"%s\",\"g\":\"%s\",\"w\":false,\"gd\":%d,\"ed\":%d}",
             es, gs, got_dots, exp_dots);
    ws_send(buf);
}

static void ws_notify_word_incorrect(const char *expected, const char *got,
                                     int len, const bool *pc) {
    JsonDocument doc;
    doc["t"] = "no";
    doc["s"] = expected;
    doc["g"] = got;
    doc["w"] = true;
    JsonArray arr = doc["pc"].to<JsonArray>();
    for (int i = 0; i < len; i++) arr.add(pc[i] ? 1 : 0);
    char buf[256];
    serializeJson(doc, buf, sizeof(buf));
    ws_send(buf);
}

static void ws_notify_stats(Engine &engine) {
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"t\":\"stats\",\"n\":%d,\"a\":%d}",
             engine.session.items_practiced,
             (int)(engine.session.accuracy() * 100));
    ws_send(buf);
}

static void ws_notify_level(Engine &engine) {
    char letters[NUM_LETTERS];
    int n;
    engine.introduced_letters(letters, &n);
    char lstr[NUM_LETTERS + 1];
    memcpy(lstr, letters, n);
    lstr[n] = 0;

    char buf[128];
    snprintf(buf, sizeof(buf), "{\"t\":\"level\",\"l\":%d,\"c\":\"%s\"}", engine.level(), lstr);
    ws_send(buf);
}

static void ws_notify_advance(Engine &engine, const char *msg) {
    char letters[NUM_LETTERS];
    int n;
    engine.introduced_letters(letters, &n);
    char lstr[NUM_LETTERS + 1];
    memcpy(lstr, letters, n);
    lstr[n] = 0;

    JsonDocument doc;
    doc["t"] = "advance";
    doc["l"] = engine.level();
    doc["c"] = lstr;
    doc["m"] = msg;
    char buf[384];
    serializeJson(doc, buf, sizeof(buf));
    ws_send(buf);
}

static void ws_notify_word_progress(const char *target, const char *typed, int typed_len) {
    char ts[16] = {0};
    memcpy(ts, typed, min(typed_len, 15));
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"t\":\"wp\",\"w\":\"%s\",\"p\":\"%s\"}", target, ts);
    ws_send(buf);
}

static void ws_send_state(AsyncWebSocketClient *client, Engine &engine) {
    char letters[NUM_LETTERS];
    int n;
    engine.introduced_letters(letters, &n);
    char lstr[NUM_LETTERS + 1];
    memcpy(lstr, letters, n);
    lstr[n] = 0;

    const char *mode_str = (training_mode == MODE_LETTERS) ? "letters" :
                           (training_mode == MODE_WORDS) ? "words" : "mixed";

    JsonDocument doc;
    doc["t"] = "state";
    doc["l"] = engine.level();
    doc["c"] = lstr;
    doc["mode"] = mode_str;
    doc["mirror"] = (bool)mirror_enabled;
    doc["spacing"] = (bool)word_spacing;
    doc["keepalive"] = (bool)keepalive_enabled;
    doc["ergonomic"] = (bool)ergonomic_enabled;
    doc["wordlen"] = max_word_len;
    doc["brl"] = brl_connected;
    doc["n"] = engine.session.items_practiced;
    doc["a"] = (int)(engine.session.accuracy() * 100);
    if (WiFi.status() == WL_CONNECTED) {
        doc["wssid"] = WiFi.SSID();
        doc["wip"] = WiFi.localIP().toString();
    }
    char buf[384];
    serializeJson(doc, buf, sizeof(buf));
    client->text(buf);
}

// =========================================================================
// WiFi credentials
// =========================================================================

static char wifi_ssid[33] = {0};
static char wifi_pass[65] = {0};

static bool wifi_load_creds() {
    File f = LittleFS.open("/wifi.json", "r");
    if (!f) return false;
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return false; }
    f.close();
    strlcpy(wifi_ssid, doc["ssid"] | "", sizeof(wifi_ssid));
    strlcpy(wifi_pass, doc["pass"] | "", sizeof(wifi_pass));
    return wifi_ssid[0] != 0;
}

static void wifi_save_creds() {
    JsonDocument doc;
    doc["ssid"] = wifi_ssid;
    doc["pass"] = wifi_pass;
    File f = LittleFS.open("/wifi.json", "w");
    if (f) { serializeJson(doc, f); f.close(); }
}

static void ws_notify_wifi_status() {
    JsonDocument doc;
    doc["t"] = "wifi";
    if (WiFi.status() == WL_CONNECTED) {
        doc["ssid"] = WiFi.SSID();
        doc["ip"] = WiFi.localIP().toString();
        doc["s"] = "connected";
    } else {
        doc["s"] = "disconnected";
    }
    char buf[192];
    serializeJson(doc, buf, sizeof(buf));
    ws_send(buf);
}

// =========================================================================
// Trainer state
// =========================================================================

static Progress progress;
static Engine engine;
static BrailleHID bleHID;

enum class AppMode { TRAINER, PASSTHROUGH, EXERCISE, TESTMODE };
static AppMode app_mode = AppMode::TRAINER;

// Exercise mode state
static unsigned long exercise_end = 0;
static unsigned long exercise_toggle = 0;
static unsigned long exercise_interval = 1000;
static bool exercise_on = false;

// Test mode state
static int test_dot_pos = 0;     // current cell position
static int test_dot_bit = 0;     // current dot (0-7)
static unsigned long test_step = 0;

enum class State { RESET, SHOW_ITEM, WAIT_INPUT, FEEDBACK, SUMMARY };
static State state = State::RESET;

// Current item
static char current_item[16] = {0};
static int current_item_len = 0;
static bool item_is_word = false;

// Word input accumulator
static char typed_chars[16] = {0};
static int typed_pos = 0;

// Feedback state
static unsigned long feedback_start = 0;
static bool last_correct = false;
static int save_counter = 0;

static char msg_buf[256];
static bool pending_advance_msg = false;

// Eligible words cache
#define MAX_ELIGIBLE 1024
static const char *eligible[MAX_ELIGIBLE];
static int eligible_count = 0;

static void refresh_eligible() {
    char letters[NUM_LETTERS];
    int n;
    engine.introduced_letters(letters, &n);
    int wmax = (max_word_len > 0) ? max_word_len : 8;
    eligible_count = filter_words(letters, n, eligible, MAX_ELIGIBLE, 2, wmax);
}

static void show_level_info() {
    char letters[NUM_LETTERS];
    int n;
    engine.introduced_letters(letters, &n);
    DBG.printf("Level %d: ", engine.level());
    for (int i = 0; i < n; i++) {
        if (i > 0) DBG.print(", ");
        DBG.print((char)(letters[i] - 32));
    }
    DBG.printf("  [%d words]\n", eligible_count);
}

static void select_and_present() {
    refresh_eligible();

    bool use_word = false;
    if (training_mode == MODE_WORDS && eligible_count > 0)
        use_word = true;
    else if (training_mode == MODE_MIXED)
        use_word = engine.should_use_word(eligible_count);

    if (use_word && eligible_count > 0) {
        const char *word = engine.select_word(eligible, eligible_count);
        strncpy(current_item, word, 15);
        current_item[15] = 0;
        current_item_len = strlen(current_item);
        item_is_word = true;
        display_word(current_item);
        DBG.printf("\n? %s (word, %d letters)\n", current_item, current_item_len);
        ws_notify_prompt(current_item, true);
    } else {
        char letter = engine.select_letter();
        current_item[0] = letter;
        current_item[1] = 0;
        current_item_len = 1;
        item_is_word = false;
        display_letter(letter);
        DBG.printf("\n? %c\n", letter - 32);
        ws_notify_prompt(current_item, false);
    }

    typed_pos = 0;
    memset(typed_chars, 0, sizeof(typed_chars));
    chord_reset();
    state = State::WAIT_INPUT;
}

// =========================================================================
// WebSocket event handler
// =========================================================================

// Forward declare — engine is needed in handler
static void on_ws_event(AsyncWebSocket *srv, AsyncWebSocketClient *client,
                        AwsEventType type, void *arg, uint8_t *data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        DBG.printf("WS client #%u connected\n", client->id());
        ws_send_state(client, engine);
    } else if (type == WS_EVT_DISCONNECT) {
        DBG.printf("WS client #%u disconnected\n", client->id());
    } else if (type == WS_EVT_DATA) {
        AwsFrameInfo *info = (AwsFrameInfo *)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            data[len] = 0;
            JsonDocument doc;
            if (deserializeJson(doc, (char *)data)) return;
            const char *t = doc["t"];
            if (!t) return;

            if (strcmp(t, "level") == 0) {
                pending_level = doc["l"] | -1;
            } else if (strcmp(t, "mode") == 0) {
                const char *m = doc["m"];
                if (m) {
                    if (strcmp(m, "letters") == 0) training_mode = MODE_LETTERS;
                    else if (strcmp(m, "words") == 0) training_mode = MODE_WORDS;
                    else if (strcmp(m, "mixed") == 0) training_mode = MODE_MIXED;
                    progress.mode = training_mode;
                    progress.save();
                    DBG.printf("Mode: %s\n", m);
                }
            } else if (strcmp(t, "opt") == 0) {
                const char *k = doc["k"];
                bool v = doc["v"] | false;
                if (k && strcmp(k, "mirror") == 0) {
                    mirror_enabled = v;
                    progress.mirror = v;
                    progress.save();
                    DBG.printf("Mirror: %s\n", v ? "on" : "off");
                } else if (k && strcmp(k, "spacing") == 0) {
                    word_spacing = v;
                    progress.spacing = v;
                    progress.save();
                    DBG.printf("Word spacing: %s\n", v ? "on" : "off");
                } else if (k && strcmp(k, "ergonomic") == 0) {
                    ergonomic_enabled = v;
                    progress.ergonomic = v;
                    progress.save();
                    DBG.printf("Ergonomic: %s\n", v ? "on" : "off");
                }
            } else if (strcmp(t, "exercise") == 0) {
                int mins = doc["mins"] | 5;
                exercise_interval = doc["ms"] | 1000;
                exercise_end = millis() + (unsigned long)mins * 60000;
                exercise_toggle = 0;
                exercise_on = false;
                app_mode = AppMode::EXERCISE;
                DBG.printf("Exercise mode: %d min\n", mins);
            } else if (strcmp(t, "test") == 0) {
                test_dot_pos = 0;
                test_dot_bit = 0;
                test_step = 0;
                app_mode = AppMode::TESTMODE;
                DBG.println("Test mode");
            } else if (strcmp(t, "stop") == 0) {
                if (app_mode == AppMode::EXERCISE || app_mode == AppMode::TESTMODE) {
                    app_mode = AppMode::TRAINER;
                    state = State::RESET;
                    DBG.println("Stopped, back to trainer");
                }
            } else if (strcmp(t, "wordlen") == 0) {
                max_word_len = doc["v"] | 0;
                progress.word_max_len = max_word_len;
                progress.save();
                refresh_eligible();
                DBG.printf("Max word length: %d\n", max_word_len);
            } else if (strcmp(t, "reqstats") == 0) {
                // Send full statistics
                JsonDocument sd;
                sd["t"] = "fullstats";
                int total_seen = 0, total_correct = 0;
                char letters[NUM_LETTERS];
                int nl;
                engine.introduced_letters(letters, &nl);
                JsonArray la = sd["letters"].to<JsonArray>();
                for (int i = 0; i < nl; i++) {
                    LetterStats &st = progress.get_letter(letters[i]);
                    if (st.seen_count == 0) continue;
                    total_seen += st.seen_count;
                    total_correct += st.correct_count;
                    JsonObject lo = la.add<JsonObject>();
                    char ls[2] = {letters[i], 0};
                    lo["l"] = (const char *)ls;
                    lo["n"] = st.seen_count;
                    lo["c"] = st.correct_count;
                    // Top confusions for this letter
                    JsonArray ca = lo["cf"].to<JsonArray>();
                    for (int j = 0; j < 26; j++) {
                        if (st.confused_with[j] > 0) {
                            JsonObject co = ca.add<JsonObject>();
                            char cs[2] = {(char)('a' + j), 0};
                            co["w"] = (const char *)cs;
                            co["n"] = st.confused_with[j];
                        }
                    }
                }
                sd["total"] = total_seen;
                sd["correct"] = total_correct;
                sd["level"] = engine.level();
                String out;
                serializeJson(sd, out);
                client->text(out);
            } else if (strcmp(t, "resetstats") == 0) {
                progress = Progress();
                progress.save();
                engine.begin(progress);
                training_mode = MODE_LETTERS;
                mirror_enabled = false;
                word_spacing = false;
                ergonomic_enabled = false;
                keepalive_enabled = true;
                max_word_len = 0;
                refresh_eligible();
                state = State::RESET;
                ws_notify_level(engine);
                ws_notify_stats(engine);
                ws_send("{\"t\":\"statsreset\"}");
                DBG.println("All statistics and settings reset");
            } else if (strcmp(t, "probe") == 0) {
                String out;
                ht_probe(out);
                client->text(out);
                // Resync UART after sending extended packets
                ht_reset();
                if (state == State::WAIT_INPUT) {
                    if (item_is_word) display_word(current_item);
                    else display_letter(current_item[0]);
                }
            } else if (strcmp(t, "setfirm") == 0) {
                int v = doc["v"] | 1;
                uint8_t d = (uint8_t)constrain(v, 0, 2);
                ht_send_ext(HT_EXT_SET_FIRMNESS, &d, 1);
                DBG.printf("Set firmness: %d\n", d);
            } else if (strcmp(t, "syncrtc") == 0) {
                // Get current time from NTP or use compile time
                struct tm ti;
                if (getLocalTime(&ti, 100)) {
                    uint8_t rtc[7];
                    int yr = ti.tm_year + 1900;
                    rtc[0] = yr >> 8; rtc[1] = yr & 0xFF;
                    rtc[2] = ti.tm_mon + 1;
                    rtc[3] = ti.tm_mday;
                    rtc[4] = ti.tm_hour;
                    rtc[5] = ti.tm_min;
                    rtc[6] = ti.tm_sec;
                    ht_send_ext(HT_EXT_SET_RTC, rtc, 7);
                    DBG.printf("Synced RTC: %04d-%02d-%02d %02d:%02d:%02d\n",
                               yr, rtc[2], rtc[3], rtc[4], rtc[5], rtc[6]);
                } else {
                    DBG.println("No time available for RTC sync");
                }
            } else if (strcmp(t, "reconnect") == 0) {
                pending_reconnect = true;
            } else if (strcmp(t, "opt") == 0 && doc["k"] && strcmp(doc["k"], "keepalive") == 0) {
                keepalive_enabled = doc["v"] | true;
                progress.keepalive = keepalive_enabled;
                progress.save();
                DBG.printf("Keepalive: %s\n", keepalive_enabled ? "on" : "off");
            } else if (strcmp(t, "wscan") == 0) {
                pending_wifi_scan = true;
            } else if (strcmp(t, "wconn") == 0) {
                strlcpy(pending_ssid, doc["s"] | "", sizeof(pending_ssid));
                strlcpy(pending_pass, doc["p"] | "", sizeof(pending_pass));
                pending_wifi_connect = true;
            } else if (strcmp(t, "wdisc") == 0) {
                pending_wifi_disconnect = true;
            }
        }
    }
}

// =========================================================================
// Web server setup
// =========================================================================

// Freeze DHCP: switch to static IP to prevent DHCP renewal NVS writes
// that crash when BLE is also accessing NVS (ESP-IDF framework bug)
static void wifi_freeze_dhcp() {
    if (WiFi.status() == WL_CONNECTED) {
        IPAddress ip = WiFi.localIP();
        IPAddress gw = WiFi.gatewayIP();
        IPAddress sn = WiFi.subnetMask();
        IPAddress dns = WiFi.dnsIP();
        WiFi.config(ip, gw, sn, dns);
        DBG.printf("WiFi: frozen to static %s\n", ip.toString().c_str());
    }
}

static void web_setup() {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("BrailleTrain");
    DBG.printf("WiFi AP: BrailleTrain  IP: %s\n", WiFi.softAPIP().toString().c_str());

    if (wifi_load_creds()) {
        DBG.printf("Connecting to WiFi: %s ...\n", wifi_ssid);
        WiFi.begin(wifi_ssid, wifi_pass);
        unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000)
            delay(100);
        if (WiFi.status() == WL_CONNECTED) {
            DBG.printf("WiFi STA: %s  IP: %s\n", wifi_ssid,
                       WiFi.localIP().toString().c_str());
            wifi_freeze_dhcp();
        } else
            DBG.println("WiFi STA connection failed");
    }

    ws.onEvent(on_ws_event);
    server.addHandler(&ws);

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/html", HTML_PAGE);
    });

    server.begin();

    if (MDNS.begin("brailletrain")) {
        MDNS.addService("http", "tcp", 80);
        DBG.println("mDNS: http://brailletrain.local");
    }

    DBG.println("Web server started on port 80");
}

// =========================================================================
// setup / loop
// =========================================================================

#ifdef LOOPBACK_TEST
// Loopback test — RS-232 side must have TX/RX bridged
// Tests 8N1 and 8O1, repeats every 10s

static int run_loopback(const char *label, uint32_t config) {
    BRL.end();
    delay(50);
    BRL.begin(19200, config, BRL_RX, BRL_TX);
    delay(100);
    while (BRL.available()) BRL.read();

    int pass = 0, fail = 0, timeout = 0;
    DBG.printf("\n%s: sending 256 bytes...\n", label);
    for (int i = 0; i < 256; i++) {
        uint8_t tb = (uint8_t)i;
        BRL.write(tb);
        BRL.flush();
        unsigned long t0 = millis();
        while (!BRL.available() && millis() - t0 < 50) {}
        if (BRL.available()) {
            uint8_t got = BRL.read();
            if (got == tb) pass++;
            else { fail++; if (fail <= 10) DBG.printf("  MISMATCH: sent 0x%02X got 0x%02X\n", tb, got); }
        } else { timeout++; if (timeout <= 5) DBG.printf("  TIMEOUT: sent 0x%02X\n", tb); }
    }
    DBG.printf("Results: %d pass, %d fail, %d timeout\n", pass, fail, timeout);
    return (pass == 256) ? 1 : 0;
}

void setup() {
    DBG.begin(115200);
    delay(2000);
    DBG.println("\n=== MAX232 Loopback Test ===");
    DBG.printf("TX pin: GPIO%d  RX pin: GPIO%d\n", BRL_TX, BRL_RX);
}

void loop() {
    int ok1 = run_loopback("8N1", SERIAL_8N1);
    int ok2 = run_loopback("8O1 (HT protocol)", SERIAL_8O1);
    DBG.printf("\n--- Summary: 8N1=%s  8O1=%s ---\n",
               ok1 ? "OK" : "FAIL", ok2 ? "OK" : "FAIL");
    delay(10000);
}

#elif defined(HT_PROTOCOL_TEST)
// Bare-bones HT protocol test — no WiFi, no BLE, just UART
// Sends reset, writes test cells, dumps all RX with timestamps

void setup() {
    DBG.begin(115200);
    delay(2000);
    DBG.println("\n=== HT Protocol Test (no WiFi/BLE) ===");
    DBG.printf("TX=GPIO%d  RX=GPIO%d  19200 8O1\n", BRL_TX, BRL_RX);
    BRL.begin(19200, SERIAL_8O1, BRL_RX, BRL_TX);
    delay(100);
    while (BRL.available()) BRL.read();

    // Send reset
    DBG.println("\n--- Sending HT reset (0xFF) ---");
    BRL.write((uint8_t)0xFF);
    BRL.flush();
    unsigned long t0 = millis();
    while (millis() - t0 < 2000) {
        if (BRL.available()) {
            DBG.printf("  +%4lums RX: 0x%02X\n", millis() - t0, BRL.read());
        }
    }

    // Write test pattern to cells: dots 1-6 on cell 0, rest blank
    DBG.println("\n--- Writing cells (0x3F on cell 0, rest 0x00) ---");
    uint8_t cells[40] = {0};
    cells[0] = 0x3F;
    BRL.write((uint8_t)0x01);  // HT_PKT_BRAILLE
    BRL.write(cells, 40);
    BRL.flush();
    t0 = millis();
    while (millis() - t0 < 2000) {
        if (BRL.available()) {
            DBG.printf("  +%4lums RX: 0x%02X\n", millis() - t0, BRL.read());
        }
    }

    // Write "hello" on first 5 cells
    DBG.println("\n--- Writing 'hello' to cells ---");
    memset(cells, 0, 40);
    cells[0] = 0x11; // h: dots 1,2,5
    cells[1] = 0x10; // e: dots 1,5
    cells[2] = 0x07; // l: dots 1,2,3
    cells[3] = 0x07; // l
    cells[4] = 0x0D; // o: dots 1,3,5 -- wait that's wrong
    // Actually: h=125, e=15, l=123, o=135
    cells[0] = (1<<0)|(1<<1)|(1<<4);         // h: 1,2,5
    cells[1] = (1<<0)|(1<<4);                 // e: 1,5
    cells[2] = (1<<0)|(1<<1)|(1<<2);          // l: 1,2,3
    cells[3] = (1<<0)|(1<<1)|(1<<2);          // l: 1,2,3
    cells[4] = (1<<0)|(1<<2)|(1<<4);          // o: 1,3,5
    BRL.write((uint8_t)0x01);
    BRL.write(cells, 40);
    BRL.flush();
    t0 = millis();
    while (millis() - t0 < 2000) {
        if (BRL.available()) {
            DBG.printf("  +%4lums RX: 0x%02X\n", millis() - t0, BRL.read());
        }
    }

    DBG.println("\n--- Listening for key events (press keys on BrailleWave) ---");
    DBG.println("    Format: +<ms since last> RX: 0x<byte>");
}

void loop() {
    static unsigned long last_rx = millis();
    if (BRL.available()) {
        unsigned long now = millis();
        DBG.printf("+%4lums RX: 0x%02X\n", now - last_rx, BRL.read());
        last_rx = now;
    }
}

#else // normal firmware

void setup() {
    DBG.begin(115200);
    delay(500);
    DBG.println("\n=== ESP32-S3 Braille Trainer ===\n");

    BRL.begin(19200, SERIAL_8O1, BRL_RX, BRL_TX);
    DBG.printf("UART1: 19200 8O1, TX=GPIO%d RX=GPIO%d\n", BRL_TX, BRL_RX);

#if defined(LED_PIN)
    pinMode(LED_PIN, OUTPUT);
#endif
    led_off();

    if (!LittleFS.begin(true)) {
        DBG.println("LittleFS mount failed!");
    } else {
        DBG.println("LittleFS mounted");
        if (progress.load())
            DBG.printf("Progress loaded: level %d\n", progress.level);
        else
            DBG.println("No saved progress, starting fresh");
    }

    engine.begin(progress);
    training_mode = progress.mode;
    mirror_enabled = progress.mirror;
    word_spacing = progress.spacing;
    keepalive_enabled = progress.keepalive;
    ergonomic_enabled = progress.ergonomic;
    max_word_len = progress.word_max_len;
    randomSeed(analogRead(0) ^ micros());

    web_setup();
    bleHID.begin();

    ArduinoOTA.setHostname("brailletrain");
    ArduinoOTA.begin();

    delay(100);
    while (BRL.available()) BRL.read();
}

static void trainer_loop();
static void passthrough_loop();
static void exercise_loop();
static void testmode_loop();

void loop() {
    ArduinoOTA.handle();

    // BLE HID mode switching
    if (bleHID.connected() && app_mode == AppMode::TRAINER) {
        app_mode = AppMode::PASSTHROUGH;
        DBG.println("\n=== BLE host connected — pass-through mode ===");
        chord_reset();
        passthrough_reset_keys();
        ws_send("{\"t\":\"ble\",\"s\":\"connected\"}");
    } else if (!bleHID.connected() && app_mode == AppMode::PASSTHROUGH) {
        app_mode = AppMode::TRAINER;
        DBG.println("\n=== BLE host disconnected — trainer mode ===");
        state = State::RESET;
        ws_send("{\"t\":\"ble\",\"s\":\"disconnected\"}");
    }


    // Periodic WebSocket cleanup + heartbeat
    ws.cleanupClients();
    {
        static unsigned long last_hb = 0;
        if (millis() - last_hb > 5000) {
            last_hb = millis();
            ws_send("{\"t\":\"hb\"}");
        }
    }

    // Process pending WiFi commands
    if (pending_wifi_scan) {
        pending_wifi_scan = false;
        int n = WiFi.scanNetworks();
        JsonDocument doc;
        doc["t"] = "wscanr";
        JsonArray nets = doc["nets"].to<JsonArray>();
        for (int i = 0; i < n && i < 20; i++) {
            JsonObject net = nets.add<JsonObject>();
            net["s"] = WiFi.SSID(i);
            net["r"] = WiFi.RSSI(i);
            net["e"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        }
        String out;
        serializeJson(doc, out);
        ws_send(out.c_str());
        WiFi.scanDelete();
    }

    if (pending_wifi_connect) {
        pending_wifi_connect = false;
        strlcpy(wifi_ssid, pending_ssid, sizeof(wifi_ssid));
        strlcpy(wifi_pass, pending_pass, sizeof(wifi_pass));
        wifi_save_creds();
        WiFi.disconnect(false);
        delay(100);
        WiFi.begin(wifi_ssid, wifi_pass);
        unsigned long t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000)
            delay(100);
        if (WiFi.status() == WL_CONNECTED) {
            DBG.printf("WiFi STA: %s  IP: %s\n", wifi_ssid,
                       WiFi.localIP().toString().c_str());
            wifi_freeze_dhcp();
        } else
            DBG.printf("WiFi STA: %s failed\n", wifi_ssid);
        ws_notify_wifi_status();
    }

    if (pending_wifi_disconnect) {
        pending_wifi_disconnect = false;
        WiFi.disconnect(false);
        wifi_ssid[0] = 0;
        wifi_pass[0] = 0;
        wifi_save_creds();
        DBG.println("WiFi STA disconnected");
        ws_notify_wifi_status();
    }

    // Manual reconnect request
    if (pending_reconnect) {
        pending_reconnect = false;
        DBG.println("Reconnect requested");
        if (ht_reset()) {
            delay(500);
            while (BRL.available()) BRL.read();
            brl_connected = true;
            last_key_time = millis();
            chord_reset();
            state = State::SHOW_ITEM;
            if (app_mode != AppMode::TRAINER) app_mode = AppMode::TRAINER;
            ws_send("{\"t\":\"brl\",\"s\":true}");
            DBG.println("BrailleWave reconnected");
        } else {
            brl_connected = false;
            ws_send("{\"t\":\"brl\",\"s\":false}");
            DBG.println("BrailleWave not responding");
        }
    }

    // Background keepalive: ping BrailleWave after inactivity
    if (keepalive_enabled && app_mode == AppMode::TRAINER
        && state != State::RESET) {
        unsigned long now = millis();
        if (brl_connected) {
            // Check if still alive after 30s of no keys
            if (now - last_key_time > 30000 && now - last_keepalive > 10000) {
                last_keepalive = now;
                if (!ht_reset()) {
                    brl_connected = false;
                    led_off();
                    ws_send("{\"t\":\"brl\",\"s\":false}");
                    DBG.println("BrailleWave: lost connection");
                } else {
                    // Still connected, redisplay current item
                    if (state == State::WAIT_INPUT) {
                        if (item_is_word) display_word(current_item);
                        else display_letter(current_item[0]);
                    }
                }
            }
        } else {
            // Disconnected — try reconnect every 3 seconds
            if (now - last_keepalive > 3000) {
                last_keepalive = now;
                if (ht_reset()) {
                    delay(500);
                    while (BRL.available()) BRL.read();
                    brl_connected = true;
                    last_key_time = millis();
                    chord_reset();
                    state = State::SHOW_ITEM;
                    ws_send("{\"t\":\"brl\",\"s\":true}");
                    DBG.println("BrailleWave: reconnected");
                }
            }
        }
    }

    // Always process HT USB-HID reports (brltty may send via report 0x02
    // even when we're not in passthrough mode)
    if (bleHID.connected()) {
        // Forward HT serial data from brltty → UART
        uint8_t ht_data[HT_HID_DATA_SIZE];
        uint8_t ht_len = 0;
        if (bleHID.processHtData(ht_data, &ht_len)) {
            BRL.write(ht_data, ht_len);
            BRL.flush();
            DBG.printf("HT-HID: forwarded %d bytes to UART\n", ht_len);
        }
        // Forward HT firmware commands
        uint8_t cmd = 0;
        if (bleHID.processHtCommand(&cmd)) {
            if (cmd == 0x01) {
                ht_uart_rx_len = 0;
                while (BRL.available()) BRL.read();
                DBG.println("HT-HID: flush buffers");
            }
        }
        // Buffer any UART responses for brltty GET_REPORT(0x01)
        // (only when NOT in full passthrough, which handles this itself)
        if (app_mode != AppMode::PASSTHROUGH) {
            bool got_data = false;
            while (BRL.available()) {
                bleHID.htBufferUartByte(BRL.read());
                got_data = true;
            }
            if (got_data)
                bleHID.htNotifyData();
        }
    }

    switch (app_mode) {
    case AppMode::PASSTHROUGH: passthrough_loop(); break;
    case AppMode::EXERCISE:    exercise_loop(); break;
    case AppMode::TESTMODE:    testmode_loop(); break;
    default:                   trainer_loop(); break;
    }
}

// =========================================================================
// Pass-through mode: bridge BLE HID ↔ BrailleWave UART
// =========================================================================

static void passthrough_loop() {
    uint8_t cells[HT_CELLS];
    if (bleHID.processPendingCells(cells))
        ht_write_cells(cells);
    passthrough_poll(bleHID);
}


// =========================================================================
// Exercise mode: flip all dots on/off for pin break-in / cleaning
// =========================================================================

static void exercise_loop() {
    unsigned long now = millis();
    if (now >= exercise_end) {
        ht_clear();
        app_mode = AppMode::TRAINER;
        state = State::RESET;
        ws_send("{\"t\":\"exdone\"}");
        DBG.println("Exercise done");
        return;
    }

    if (now - exercise_toggle >= exercise_interval) {
        exercise_toggle = now;
        exercise_on = !exercise_on;
        uint8_t cells[HT_CELLS];
        memset(cells, exercise_on ? 0xFF : 0x00, HT_CELLS);
        ht_write_cells(cells);

        // Report remaining time
        int secs_left = (exercise_end - now) / 1000;
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"t\":\"extick\",\"s\":%d}", secs_left);
        ws_send(buf);
    }
}

// =========================================================================
// Test mode: cycle dots sequentially + report key presses
// =========================================================================

static void testmode_loop() {
    unsigned long now = millis();

    // Cycle one dot at a time across all cells, ~200ms per step
    if (now - test_step >= 200) {
        test_step = now;
        uint8_t cells[HT_CELLS] = {0};
        cells[test_dot_pos] = (1 << test_dot_bit);
        ht_write_cells(cells);

        // Report which cell/dot is active
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"t\":\"tdot\",\"c\":%d,\"d\":%d}",
                 test_dot_pos, test_dot_bit + 1);
        ws_send(buf);

        test_dot_bit++;
        if (test_dot_bit >= 8) {
            test_dot_bit = 0;
            test_dot_pos++;
            if (test_dot_pos >= HT_CELLS) test_dot_pos = 0;
        }
    }

    // Report any key presses
    while (BRL.available()) {
        int b = BRL.read();
        bool release = b & 0x80;
        uint8_t code = b & 0x7F;
        char buf[64];

        if (code >= 0x20 && code < 0x48) {
            snprintf(buf, sizeof(buf), "{\"t\":\"tkey\",\"k\":\"rk%d\",\"r\":%s}",
                     code - 0x20, release ? "true" : "false");
        } else {
            const char *name = "?";
            const KeyInfo *ki = lookup_key(code);
            if (ki) {
                if (ki->dot > 0) {
                    static char dn[8];
                    snprintf(dn, sizeof(dn), "dot%d", ki->dot);
                    name = dn;
                } else if (code == KEY_SPACE) name = "space";
                else if (code == NAV_LEFT)  name = "navL";
                else if (code == NAV_RIGHT) name = "navR";
                else if (code == NAV_PREV)  name = "navP";
                else if (code == NAV_NEXT)  name = "navN";
            }
            snprintf(buf, sizeof(buf), "{\"t\":\"tkey\",\"k\":\"%s\",\"r\":%s}",
                     name, release ? "true" : "false");
        }
        ws_send(buf);
    }
}

// =========================================================================
// Trainer mode
// =========================================================================

static void trainer_loop() {
    // Process pending level change from WebSocket
    int pl = pending_level;
    if (pl >= 1 && pl <= NUM_LETTERS) {
        pending_level = -1;
        progress.level = pl;
        progress.level_trial_count = 0;
        progress.save();
        refresh_eligible();
        DBG.printf("\n=> Level %d set via web\n", pl);
        show_level_info();
        ws_notify_level(engine);

        // Show level letters on display
        char letters[NUM_LETTERS];
        int n;
        engine.introduced_letters(letters, &n);
        uint8_t cells[HT_CELLS] = {0};
        for (int i = 0; i < n && i < HT_CELLS; i++)
            cells[i] = char_to_cell(letters[i]);
        display_cells(cells, n);
        delay(1500);

        if (state != State::RESET) state = State::SHOW_ITEM;
    }

    switch (state) {
    case State::RESET:
        if (ht_reset()) {
            brl_connected = true;
            last_key_time = millis();
            ws_send("{\"t\":\"brl\",\"s\":true}");
            DBG.println("Connected to BrailleWave\n");
            refresh_eligible();
            show_level_info();
            ws_notify_level(engine);

            char letters[NUM_LETTERS];
            int n;
            engine.introduced_letters(letters, &n);
            uint8_t cells[HT_CELLS] = {0};
            for (int i = 0; i < n && i < HT_CELLS; i++)
                cells[i] = char_to_cell(letters[i]);
            display_cells(cells, n);

            DBG.println("Nav: left=repeat, right=skip/next, prev/next=quit");
            DBG.printf("Web: http://%s\n", WiFi.softAPIP().toString().c_str());
            if (WiFi.status() == WL_CONNECTED)
                DBG.printf("Web: http://%s\n", WiFi.localIP().toString().c_str());
            DBG.println();
            delay(2000);
            state = State::SHOW_ITEM;
        } else {
            delay(2000);
        }
        break;

    case State::SHOW_ITEM:
        select_and_present();
        break;

    case State::WAIT_INPUT:
        poll_keys();

        // Routing key = set level
        if (routing_key >= 0) {
            int new_level = routing_key + 1;
            if (new_level >= 1 && new_level <= NUM_LETTERS) {
                progress.level = new_level;
                progress.level_trial_count = 0;
                progress.save();
                refresh_eligible();
                DBG.printf("\n=> Level %d via routing key\n", new_level);
                show_level_info();
                ws_notify_level(engine);

                char letters[NUM_LETTERS];
                int n;
                engine.introduced_letters(letters, &n);
                uint8_t cells[HT_CELLS] = {0};
                for (int i = 0; i < n && i < HT_CELLS; i++)
                    cells[i] = char_to_cell(letters[i]);
                display_cells(cells, n);
                delay(1500);
                state = State::SHOW_ITEM;
            }
            routing_key = -1;
            break;
        }

        if (chord_ready) {
            chord_ready = false;

            // Nav left = repeat
            if (chord_nav == NAV_LEFT) {
                if (item_is_word) display_word(current_item);
                else display_letter(current_item[0]);
                DBG.println("(repeat)");
                chord_reset();
                break;
            }

            // Nav right = skip
            if (chord_nav == NAV_RIGHT) {
                DBG.println("(skip)");
                state = State::SHOW_ITEM;
                break;
            }

            // Nav prev/next = session summary
            if (chord_nav == NAV_PREV || chord_nav == NAV_NEXT) {
                if (engine.session.items_practiced > 0)
                    state = State::SUMMARY;
                else {
                    DBG.println("No items yet.");
                    chord_reset();
                }
                break;
            }

            // Space with no dots = skip
            if (chord_is_space && chord_dots == 0 && !item_is_word) {
                DBG.println("(skip)");
                state = State::SHOW_ITEM;
                break;
            }

            if (item_is_word) {
                // Word mode: accumulate chords
                if (chord_is_space && chord_dots == 0) {
                    // Space chord = submit what we have
                    // Pad with nulls for scoring
                    while (typed_pos < current_item_len)
                        typed_chars[typed_pos++] = 0;
                    goto score_word;
                }

                if (chord_dots != 0) {
                    char got = cell_to_char(chord_dots);
                    typed_chars[typed_pos++] = got ? got : 0;
                    DBG.printf("  [%d/%d] %c\n", typed_pos, current_item_len,
                               got ? got - 32 : '?');

                    // Show progress on display
                    display_word_progress(typed_chars, typed_pos);
                    ws_notify_word_progress(current_item, typed_chars, typed_pos);

                    if (typed_pos >= current_item_len) {
                        goto score_word;
                    }
                }
                chord_reset();
                break;

                score_word: {
                    typed_chars[typed_pos] = 0;
                    bool pos_correct[16];
                    bool correct = engine.score_word(current_item, typed_chars,
                                                     current_item_len, pos_correct);
                    last_correct = correct;
                    save_counter++;

                    if (correct) {
                        display_word(current_item);
                        DBG.printf("OK! %s\n", current_item);
                        ws_notify_correct(current_item, true);
                    } else {
                        // Show correct word on display
                        display_word(current_item);
                        DBG.printf("NO: typed ");
                        for (int i = 0; i < current_item_len; i++)
                            DBG.print(typed_chars[i] ? (char)(typed_chars[i] - 32) : '?');
                        DBG.printf(", answer: %s\n", current_item);
                        ws_notify_word_incorrect(current_item, typed_chars,
                                                 current_item_len, pos_correct);
                    }

                    ws_notify_stats(engine);
                    feedback_start = millis();
                    chord_reset();
                    state = State::FEEDBACK;

                    if (correct && engine.check_advancement(msg_buf, sizeof(msg_buf)))
                        pending_advance_msg = true;
                    break;
                }
            } else {
                // Letter mode: single chord scores immediately
                if (chord_dots != 0) {
                    char expected = current_item[0];
                    char got = cell_to_char(chord_dots);

                    bool correct = engine.score_letter(expected, got);
                    last_correct = correct;
                    save_counter++;

                    if (correct) {
                        uint8_t fb[3] = { char_to_cell(expected), 0, 0xC0 };
                        display_cells(fb, 3);
                        DBG.printf("OK! %c\n", expected - 32);
                        ws_notify_correct(current_item, false);
                    } else {
                        display_comparison(chord_dots, char_to_cell(expected));
                        char ds_exp[32];
                        dots_str(expected, ds_exp, sizeof(ds_exp));
                        if (got)
                            DBG.printf("NO: %c, answer: %c (%s)\n",
                                       got - 32, expected - 32, ds_exp);
                        else
                            DBG.printf("NO: 0x%02X, answer: %c (%s)\n",
                                       chord_dots, expected - 32, ds_exp);
                        ws_notify_letter_incorrect(expected, got, chord_dots,
                                                   char_to_cell(expected));
                    }

                    ws_notify_stats(engine);
                    feedback_start = millis();
                    chord_reset();
                    state = State::FEEDBACK;

                    if (correct && engine.check_advancement(msg_buf, sizeof(msg_buf)))
                        pending_advance_msg = true;
                }
            }
        }
        break;

    case State::FEEDBACK: {
        bool proceed = false;

        if (last_correct) {
            if (millis() - feedback_start >= 800) proceed = true;
        } else {
            // Wait for any key to continue
            poll_keys();
            if (chord_ready) {
                chord_ready = false;
                proceed = true;
                chord_reset();
            }
        }

        if (proceed) {
            if (save_counter >= 5) {
                progress.save();
                save_counter = 0;
                DBG.println("(saved)");
            }

            if (pending_advance_msg) {
                pending_advance_msg = false;
                DBG.printf("\n*** %s ***\n\n", msg_buf);
                ws_notify_advance(engine, msg_buf);
                // Show on braille display
                uint8_t cells[HT_CELLS] = {0};
                // Show the new letter big on cell 0
                char newest = engine.newest_letter();
                cells[0] = char_to_cell(newest);
                if (mirror_enabled) cells[HT_CELLS/2] = cells[0];
                ht_write_cells(cells);
                delay(3000);
                refresh_eligible();
                show_level_info();
                ws_notify_level(engine);
            }

            state = State::SHOW_ITEM;
        }
        break;
    }

    case State::SUMMARY:
        engine.print_summary(DBG);
        progress.save();
        DBG.println("\nSession saved. Press any key for new session.");
        ht_clear();
        display_word("done");

        chord_reset();
        while (true) {
            poll_keys();
            if (chord_ready) { chord_ready = false; break; }
            ws.cleanupClients();
            delay(20);
        }

        engine.session = SessionStats();
        engine.session.start_time = millis();
        ws_notify_stats(engine);
        state = State::SHOW_ITEM;
        break;
    }
}

#endif // LOOPBACK_TEST

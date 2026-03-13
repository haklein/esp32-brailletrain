/*
 * ESP32-S3 Braille Trainer with web UI
 *
 * Hardware:
 *   GPIO 7 (TX) -> MAX232 RX pin -> DB9 -> BrailleWave
 *   GPIO 6 (RX) <- MAX232 TX pin <- DB9 <- BrailleWave
 *   UART0 (CH340) -> PC serial monitor at 115200
 *
 * WiFi AP "BrailleTrain" -> http://192.168.4.1
 */

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

#include "braille_data.h"
#include "progress.h"
#include "engine.h"
#include "wordlist.h"
#include "html_content.h"

// =========================================================================
// Hardware
// =========================================================================

HardwareSerial DBG(0);

#define BRL_TX  7
#define BRL_RX  6
HardwareSerial BRL(1);

#define HT_PKT_RESET   0xFF
#define HT_PKT_OK      0xFE
#define HT_PKT_BRAILLE 0x01
#define HT_MODEL_WAVE   0x05

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

enum TrainMode { MODE_LETTERS, MODE_WORDS, MODE_MIXED };
static volatile int training_mode = MODE_LETTERS;

// Pending settings from WebSocket (processed in main loop)
static volatile int pending_level = -1;

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
            return true;
        }
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

// =========================================================================
// Display helpers (mirror + spacing)
// =========================================================================

static void display_cells(const uint8_t *src, int src_len) {
    uint8_t cells[HT_CELLS] = {0};
    for (int i = 0; i < src_len && i < HT_CELLS; i++)
        cells[i] = src[i];
    if (mirror_enabled) {
        int half = HT_CELLS / 2;
        for (int i = 0; i < half && i < src_len; i++)
            cells[half + i] = src[i];
    }
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

static void poll_keys() {
    while (BRL.available()) {
        int b = BRL.read();
        bool release = b & 0x80;
        uint8_t code = b & 0x7F;

        if (code >= 0x20 && code < 0x48) {
            if (!release) routing_key = code - 0x20;
            continue;
        }
        if (!lookup_key(code) && code >= 0x20) continue;

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
    doc["n"] = engine.session.items_practiced;
    doc["a"] = (int)(engine.session.accuracy() * 100);
    char buf[256];
    serializeJson(doc, buf, sizeof(buf));
    client->text(buf);
}

// =========================================================================
// Trainer state
// =========================================================================

static Progress progress;
static Engine engine;

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
#define MAX_ELIGIBLE 256
static const char *eligible[MAX_ELIGIBLE];
static int eligible_count = 0;

static void refresh_eligible() {
    char letters[NUM_LETTERS];
    int n;
    engine.introduced_letters(letters, &n);
    eligible_count = filter_words(letters, n, eligible, MAX_ELIGIBLE);
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
                    DBG.printf("Mode: %s\n", m);
                }
            } else if (strcmp(t, "opt") == 0) {
                const char *k = doc["k"];
                bool v = doc["v"] | false;
                if (k && strcmp(k, "mirror") == 0) {
                    mirror_enabled = v;
                    DBG.printf("Mirror: %s\n", v ? "on" : "off");
                } else if (k && strcmp(k, "spacing") == 0) {
                    word_spacing = v;
                    DBG.printf("Word spacing: %s\n", v ? "on" : "off");
                }
            }
        }
    }
}

// =========================================================================
// Web server setup
// =========================================================================

static void web_setup() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("BrailleTrain");
    DBG.printf("WiFi AP: BrailleTrain  IP: %s\n", WiFi.softAPIP().toString().c_str());

    ws.onEvent(on_ws_event);
    server.addHandler(&ws);

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/html", HTML_PAGE);
    });

    server.begin();
    DBG.println("Web server started on port 80");
}

// =========================================================================
// setup / loop
// =========================================================================

void setup() {
    DBG.begin(115200);
    delay(500);
    DBG.println("\n=== ESP32-S3 Braille Trainer ===\n");

    BRL.begin(19200, SERIAL_8O1, BRL_RX, BRL_TX);
    DBG.printf("UART1: 19200 8O1, TX=GPIO%d RX=GPIO%d\n", BRL_TX, BRL_RX);

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
    randomSeed(analogRead(0) ^ micros());

    web_setup();

    delay(100);
    while (BRL.available()) BRL.read();
}

void loop() {
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

    // Periodic WebSocket cleanup
    ws.cleanupClients();

    switch (state) {
    case State::RESET:
        if (ht_reset()) {
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
            DBG.printf("Web: http://%s\n\n", WiFi.softAPIP().toString().c_str());
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

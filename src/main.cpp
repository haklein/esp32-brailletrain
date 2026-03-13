/*
 * ESP32-S3 Braille Trainer — standalone trainer on BrailleWave via MAX232
 *
 * Hardware:
 *   GPIO 7 (TX) -> MAX232 RX pin -> DB9 -> BrailleWave
 *   GPIO 6 (RX) <- MAX232 TX pin <- DB9 <- BrailleWave
 *   UART0 (CH340) -> PC serial monitor at 115200
 *
 * Training flow:
 *   1. Display shows a letter on the braille cells
 *   2. User types the letter as a chord on the BrailleWave keyboard
 *   3. Correct/incorrect feedback on display + debug serial
 *   4. Auto-advancement when criteria are met
 *   5. Progress saved to LittleFS flash
 */

#include <Arduino.h>
#include <LittleFS.h>
#include "braille_data.h"
#include "progress.h"
#include "engine.h"

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------

HardwareSerial DBG(0);

#define BRL_TX  7
#define BRL_RX  6
HardwareSerial BRL(1);

#define HT_PKT_RESET   0xFF
#define HT_PKT_OK      0xFE
#define HT_PKT_BRAILLE 0x01
#define HT_MODEL_WAVE   0x05

// ---------------------------------------------------------------------------
// Key codes
// ---------------------------------------------------------------------------

struct KeyInfo {
    uint8_t code;
    const char *name;
    int dot;  // -1 if not a dot key
};

static const KeyInfo KEY_TABLE[] = {
    {0x03, "d7", 7},
    {0x07, "d3", 3},
    {0x0B, "d2", 2},
    {0x0F, "d1", 1},
    {0x13, "d4", 4},
    {0x17, "d5", 5},
    {0x1B, "d6", 6},
    {0x1F, "d8", 8},
    {0x10, "space",     -1},
    {0x0C, "nav_left",  -1},
    {0x14, "nav_right", -1},
    {0x08, "nav_prev",  -1},
    {0x04, "nav_next",  -1},
};
#define KEY_TABLE_SIZE (sizeof(KEY_TABLE) / sizeof(KEY_TABLE[0]))

#define KEY_SPACE     0x10
#define KEY_NAV_LEFT  0x0C
#define KEY_NAV_RIGHT 0x14
#define KEY_NAV_PREV  0x08
#define KEY_NAV_NEXT  0x04

// ---------------------------------------------------------------------------
// HT protocol
// ---------------------------------------------------------------------------

static bool ht_reset() {
    DBG.println("HT reset...");
    while (BRL.available()) BRL.read();

    BRL.write((uint8_t)HT_PKT_RESET);
    BRL.flush();
    delay(300);

    uint8_t buf[16];
    int count = 0;
    unsigned long deadline = millis() + 1000;
    while (millis() < deadline && count < 16) {
        if (BRL.available()) buf[count++] = BRL.read();
    }

    DBG.printf("  rx (%d bytes):", count);
    for (int i = 0; i < count; i++) DBG.printf(" 0x%02X", buf[i]);
    if (count == 0) DBG.print(" (none)");
    DBG.println();

    for (int i = 0; i < count; i++) {
        if (buf[i] == HT_PKT_OK) {
            int model = (i + 1 < count) ? buf[i + 1] : -1;
            if (model == HT_MODEL_WAVE) DBG.println("  BrailleWave identified!");
            return true;
        }
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

static void ht_write_text(const char *text) {
    uint8_t cells[HT_CELLS] = {0};
    for (int i = 0; i < HT_CELLS && text[i]; i++) {
        cells[i] = char_to_cell(text[i]);
    }
    ht_write_cells(cells);
}

static void ht_write_single(uint8_t cell, int position = 0) {
    uint8_t cells[HT_CELLS] = {0};
    if (position < HT_CELLS) cells[position] = cell;
    ht_write_cells(cells);
}

static void ht_clear() {
    uint8_t cells[HT_CELLS] = {0};
    ht_write_cells(cells);
}

// Show "got" on cell 0, blank, "expected" on cell 2 — for comparison
static void ht_show_comparison(uint8_t got, uint8_t expected) {
    uint8_t cells[HT_CELLS] = {0};
    cells[0] = got;
    cells[2] = expected;
    ht_write_cells(cells);
}

// ---------------------------------------------------------------------------
// Chord accumulator (key press/release -> completed chord)
// ---------------------------------------------------------------------------

static uint8_t pressed_keys[16];
static int pressed_n = 0;
static uint8_t chord_keys[16];
static int chord_n = 0;

// Chord result: set when all keys released after at least one press
static bool chord_ready = false;
static uint8_t chord_dots = 0;      // dot bitmask
static bool chord_is_space = false;
static uint8_t chord_nav = 0;       // nav key code, or 0

// Routing key: set on press (0-39), -1 = none
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

    bool has_dots = false;
    for (int i = 0; i < chord_n; i++) {
        const KeyInfo *ki = lookup_key(chord_keys[i]);
        if (ki) {
            if (ki->dot > 0) {
                chord_dots |= (1 << (ki->dot - 1));
                has_dots = true;
            } else if (chord_keys[i] == KEY_SPACE) {
                chord_is_space = true;
            } else {
                // nav key
                chord_nav = chord_keys[i];
            }
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

        // Routing keys: capture press, ignore release
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

// Clear any pending chord state
static void chord_reset() {
    chord_ready = false;
    chord_dots = 0;
    chord_is_space = false;
    chord_nav = 0;
    chord_n = 0;
    pressed_n = 0;
    routing_key = -1;
}

// ---------------------------------------------------------------------------
// Trainer state
// ---------------------------------------------------------------------------

static Progress progress;
static Engine engine;

enum class State { RESET, SHOW_INTRO, SHOW_LETTER, WAIT_INPUT, FEEDBACK, SUMMARY };
static State state = State::RESET;

static char current_letter = 0;
static unsigned long feedback_start = 0;
static unsigned long save_counter = 0;
static bool last_correct = false;

// For intro/advancement messages
static char msg_buf[256];
static bool pending_advance_msg = false;

static void show_level_info() {
    char letters[NUM_LETTERS];
    int n;
    engine.introduced_letters(letters, &n);

    DBG.printf("Level %d: ", engine.level());
    for (int i = 0; i < n; i++) {
        if (i > 0) DBG.print(", ");
        DBG.print((char)(letters[i] - 32));
    }
    DBG.println();
}

static void present_letter() {
    current_letter = engine.select_letter();
    ht_write_single(char_to_cell(current_letter));
    DBG.printf("\n? %c\n", current_letter - 32);
    chord_reset();
    state = State::WAIT_INPUT;
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------

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
        if (progress.load()) {
            DBG.printf("Progress loaded: level %d\n", progress.level);
        } else {
            DBG.println("No saved progress, starting fresh");
        }
    }

    engine.begin(progress);
    randomSeed(analogRead(0) ^ micros());

    delay(100);
    while (BRL.available()) BRL.read();
}

void loop() {
    switch (state) {
    case State::RESET:
        if (ht_reset()) {
            DBG.println("Connected to BrailleWave\n");
            show_level_info();

            // Show level letters briefly on display
            {
                char letters[NUM_LETTERS];
                int n;
                engine.introduced_letters(letters, &n);
                uint8_t cells[HT_CELLS] = {0};
                for (int i = 0; i < n && i < HT_CELLS; i++)
                    cells[i] = char_to_cell(letters[i]);
                ht_write_cells(cells);
            }
            DBG.println("Nav: left=repeat, right=skip, prev+next=quit\n");
            delay(2000);
            state = State::SHOW_LETTER;
        } else {
            delay(2000);
        }
        break;

    case State::SHOW_LETTER:
        present_letter();
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
                DBG.printf("\n=> Level %d set via routing key\n", new_level);
                show_level_info();

                // Show the level's letters on display
                char letters[NUM_LETTERS];
                int n;
                engine.introduced_letters(letters, &n);
                uint8_t cells[HT_CELLS] = {0};
                for (int i = 0; i < n && i < HT_CELLS; i++)
                    cells[i] = char_to_cell(letters[i]);
                ht_write_cells(cells);
                delay(1500);
                state = State::SHOW_LETTER;
            }
            routing_key = -1;
            break;
        }

        if (chord_ready) {
            chord_ready = false;

            // Nav: left = repeat current letter
            if (chord_nav == KEY_NAV_LEFT) {
                ht_write_single(char_to_cell(current_letter));
                DBG.printf("(repeat: %c)\n", current_letter - 32);
                chord_reset();
                break;
            }

            // Nav: right = skip
            if (chord_nav == KEY_NAV_RIGHT) {
                DBG.println("(skip)");
                state = State::SHOW_LETTER;
                break;
            }

            // Nav: prev+next together or just prev = quit/summary
            if (chord_nav == KEY_NAV_PREV || chord_nav == KEY_NAV_NEXT) {
                if (engine.session.items_practiced > 0) {
                    state = State::SUMMARY;
                } else {
                    DBG.println("No items practiced yet.");
                    chord_reset();
                }
                break;
            }

            // Space chord = skip (alternative)
            if (chord_is_space && chord_dots == 0) {
                DBG.println("(skip)");
                state = State::SHOW_LETTER;
                break;
            }

            // Dot chord = answer attempt
            if (chord_dots != 0) {
                char got = cell_to_char(chord_dots);
                char ds_got[32], ds_exp[32];
                dots_str(got ? got : '?', ds_got, sizeof(ds_got));
                dots_str(current_letter, ds_exp, sizeof(ds_exp));

                bool correct = engine.score_letter(current_letter, got);

                if (correct) {
                    // Show the letter briefly with full-cell flash
                    uint8_t cells[HT_CELLS] = {0};
                    cells[0] = char_to_cell(current_letter);
                    // "Correct" indicator: dot 7+8 on cell 2
                    cells[2] = 0xC0;
                    ht_write_cells(cells);
                    DBG.printf("OK! %c\n", current_letter - 32);
                } else {
                    // Show comparison: what they typed vs correct
                    ht_show_comparison(chord_dots, char_to_cell(current_letter));
                    if (got) {
                        DBG.printf("NO: typed %c (%s), answer: %c (%s)\n",
                                   got - 32, ds_got, current_letter - 32, ds_exp);
                    } else {
                        DBG.printf("NO: unknown chord 0x%02X, answer: %c (%s)\n",
                                   chord_dots, current_letter - 32, ds_exp);
                    }
                }

                save_counter++;
                last_correct = correct;
                feedback_start = millis();
                chord_reset();
                state = State::FEEDBACK;

                // Check advancement
                if (correct) {
                    if (engine.check_advancement(msg_buf, sizeof(msg_buf))) {
                        pending_advance_msg = true;
                    }
                }
            }
        }
        break;

    case State::FEEDBACK: {
        bool proceed = false;

        if (last_correct) {
            // Auto-advance after 800ms
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
            // Periodic save
            if (save_counter >= 5) {
                progress.save();
                save_counter = 0;
                DBG.println("(saved)");
            }

            // Show advancement message if pending
            if (pending_advance_msg) {
                pending_advance_msg = false;
                DBG.printf("\n*** %s ***\n\n", msg_buf);
                ht_write_text(msg_buf);
                delay(3000);
                show_level_info();
            }

            state = State::SHOW_LETTER;
        }
        break;
    }

    case State::SUMMARY:
        engine.print_summary(DBG);
        progress.save();
        DBG.println("\nSession saved. Press any key to start new session.");
        ht_write_text("done");

        // Wait for any keypress to restart
        chord_reset();
        while (true) {
            poll_keys();
            if (chord_ready) {
                chord_ready = false;
                break;
            }
            delay(20);
        }

        // Reset session stats for new session
        engine.session = SessionStats();
        engine.session.start_time = millis();
        state = State::SHOW_LETTER;
        break;
    }
}

/*
 * Training engine: item selection, scoring, auto-advancement.
 * Ported from brailletrain/engine.py
 */
#pragma once
#include <Arduino.h>
#include "braille_data.h"
#include "progress.h"

// Advancement thresholds
#define MIN_TRIALS_NEWEST   30
#define ACCURACY_NEWEST     0.85f
#define ACCURACY_OVERALL    0.80f
#define CONFUSABLE_ACCURACY 0.80f

struct SessionStats {
    int items_practiced = 0;
    int correct = 0;
    unsigned long start_time = 0;
    int letter_seen[26] = {};
    int letter_correct[26] = {};

    float accuracy() const {
        if (items_practiced == 0) return 0.0f;
        return (float)correct / items_practiced;
    }

    void record_letter(char lt, bool ok) {
        if (lt >= 'A' && lt <= 'Z') lt = lt - 'A' + 'a';
        int idx = lt - 'a';
        letter_seen[idx]++;
        if (ok) letter_correct[idx]++;
    }
};

class Engine {
public:
    Progress *progress = nullptr;
    SessionStats session;

    void begin(Progress &prog) {
        progress = &prog;
        session.start_time = millis();
        _contrast_queue_len = 0;
        _last_letter = 0;
    }

    int level() const { return progress->level; }

    int introduced_count() const { return progress->level; }

    void introduced_letters(char *buf, int *count) const {
        *count = min(progress->level, NUM_LETTERS);
        memcpy(buf, TEACH_ORDER, *count);
    }

    char newest_letter() const {
        return TEACH_ORDER[progress->level - 1];
    }

    // Select next letter for drill
    char select_letter() {
        // Contrast drill queue first
        if (_contrast_queue_len > 0) {
            char lt = _contrast_queue[0];
            // Shift queue
            for (int i = 1; i < _contrast_queue_len; i++)
                _contrast_queue[i-1] = _contrast_queue[i];
            _contrast_queue_len--;
            return lt;
        }

        char letters[NUM_LETTERS];
        int n;
        introduced_letters(letters, &n);

        float weights[NUM_LETTERS];
        float total = 0;
        unsigned long now = millis();

        for (int i = 0; i < n; i++) {
            LetterStats &st = progress->get_letter(letters[i]);
            float acc = st.accuracy();

            float error_boost = 1.0f - acc;

            float recency_boost;
            if (st.last_seen > 0 && now > st.last_seen) {
                float elapsed_sec = (now - st.last_seen) / 1000.0f;
                recency_boost = min(2.0f, elapsed_sec / 60.0f);
            } else {
                recency_boost = 2.0f;  // never seen
            }

            float confusion_boost = 1.0f;
            // Boost from predefined confusable pairs
            for (int j = 0; j < n; j++) {
                if (is_confusable(letters[i], letters[j])) {
                    LetterStats &ps = progress->get_letter(letters[j]);
                    if (ps.accuracy() < 0.8f || acc < 0.8f) {
                        confusion_boost = 1.5f;
                        break;
                    }
                }
            }
            // Boost from actual confusion history
            int total_confused = 0;
            for (int j = 0; j < 26; j++)
                total_confused += st.confused_with[j];
            if (st.seen_count > 5) {
                float conf_rate = (float)total_confused / st.seen_count;
                confusion_boost += conf_rate * 3.0f;
            }

            float w = 1.0f * (1.0f + error_boost) * recency_boost * confusion_boost;

            // Extra boost for newest letter
            if (letters[i] == newest_letter()) w *= 1.5f;

            weights[i] = w;
            total += w;
        }

        // Maybe queue contrast drill
        _maybe_queue_contrast(letters, n);

        // Weighted random selection
        float r = random(0, 10000) / 10000.0f * total;
        float cum = 0;
        for (int i = 0; i < n; i++) {
            cum += weights[i];
            if (r <= cum) {
                _last_letter = letters[i];
                return letters[i];
            }
        }
        _last_letter = letters[n-1];
        return letters[n-1];
    }

    // Score a letter response. Returns true if correct.
    bool score_letter(char expected, char got) {
        if (expected >= 'A' && expected <= 'Z') expected = expected - 'A' + 'a';
        if (got >= 'A' && got <= 'Z') got = got - 'A' + 'a';
        bool correct = (expected == got);

        progress->get_letter(expected).record(correct, correct ? 0 : got);
        progress->record_encounter(expected, correct);
        progress->level_trial_count++;

        session.items_practiced++;
        if (correct) session.correct++;
        session.record_letter(expected, correct);

        return correct;
    }

    // Score a word response per-character. Returns true if all correct.
    // pos_correct must have at least `len` entries.
    bool score_word(const char *expected, const char *got, int len, bool *pos_correct) {
        bool all_ok = true;
        for (int i = 0; i < len; i++) {
            char e = expected[i];
            char g = got[i];
            if (e >= 'A' && e <= 'Z') e = e - 'A' + 'a';
            if (g >= 'A' && g <= 'Z') g = g - 'A' + 'a';
            bool ok = (e == g && g != 0);
            if (pos_correct) pos_correct[i] = ok;

            progress->get_letter(e).record(ok, ok ? 0 : g);
            progress->record_encounter(e, ok);
            session.record_letter(e, ok);

            if (!ok) all_ok = false;
        }
        progress->level_trial_count++;
        session.items_practiced++;
        if (all_ok) session.correct++;
        return all_ok;
    }

    // Select a word from eligible list, weighted by letter weakness
    const char *select_word(const char * const *words, int n_words) {
        if (n_words == 0) return nullptr;

        // Two-pass: compute total weight, then pick
        float total = 0;
        for (int i = 0; i < n_words; i++) {
            float w = 1.0f;
            for (const char *p = words[i]; *p; p++) {
                LetterStats &st = progress->get_letter(*p);
                float acc = st.accuracy();
                w *= (2.0f - acc);
                // Boost for confused letters
                int conf = 0;
                for (int j = 0; j < 26; j++) conf += st.confused_with[j];
                if (st.seen_count > 5 && conf > 0)
                    w *= 1.0f + (float)conf / st.seen_count;
            }
            total += w;
        }

        float r = random(0, 10000) / 10000.0f * total;
        float cum = 0;
        for (int i = 0; i < n_words; i++) {
            float w = 1.0f;
            for (const char *p = words[i]; *p; p++) {
                LetterStats &st = progress->get_letter(*p);
                float acc = st.accuracy();
                w *= (2.0f - acc);
                int conf = 0;
                for (int j = 0; j < 26; j++) conf += st.confused_with[j];
                if (st.seen_count > 5 && conf > 0)
                    w *= 1.0f + (float)conf / st.seen_count;
            }
            cum += w;
            if (r <= cum) return words[i];
        }
        return words[n_words - 1];
    }

    // Decide whether to use a word in mixed mode
    bool should_use_word(int eligible_word_count) {
        if (eligible_word_count < 5) {
            return eligible_word_count > 0 && random(0, 100) < 20;
        }
        LetterStats &ns = progress->get_letter(newest_letter());
        if (ns.seen_count < 10) return random(0, 100) < 30;
        if (ns.accuracy() >= ACCURACY_NEWEST) return random(0, 100) < 70;
        return random(0, 100) < 50;
    }

    // Check advancement criteria. Returns true if advanced, writes message to buf.
    bool check_advancement(char *msg_buf, int msg_len) {
        if (progress->level >= NUM_LETTERS) {
            snprintf(msg_buf, msg_len, "All letters introduced!");
            return false;
        }

        char newest = newest_letter();
        char letters[NUM_LETTERS];
        int n;
        introduced_letters(letters, &n);

        // 1. Newest letter seen at least MIN_TRIALS_NEWEST times
        EncounterRecord enc_buf[MIN_TRIALS_NEWEST];
        int enc_count = progress->newest_letter_encounters(newest, MIN_TRIALS_NEWEST,
                                                          enc_buf, MIN_TRIALS_NEWEST);
        if (enc_count < MIN_TRIALS_NEWEST) return false;

        // 2. Accuracy on newest >= threshold
        int newest_correct = 0;
        for (int i = 0; i < enc_count; i++)
            if (enc_buf[i].correct) newest_correct++;
        float newest_acc = (float)newest_correct / enc_count;
        if (newest_acc < ACCURACY_NEWEST) return false;

        // 3. Overall accuracy >= threshold over last 50
        EncounterRecord recent_buf[50];
        int recent_count = progress->recent_encounters(50, letters, n, recent_buf, 50);
        if (recent_count < 50) return false;
        int overall_correct = 0;
        for (int i = 0; i < recent_count; i++)
            if (recent_buf[i].correct) overall_correct++;
        float overall_acc = (float)overall_correct / recent_count;
        if (overall_acc < ACCURACY_OVERALL) return false;

        // 4. Confusable pair check
        for (int p = 0; p < NUM_CONFUSABLE_PAIRS; p++) {
            char a = CONFUSABLE_PAIRS[p].a, b = CONFUSABLE_PAIRS[p].b;
            bool a_in = false, b_in = false;
            for (int i = 0; i < n; i++) {
                if (letters[i] == a) a_in = true;
                if (letters[i] == b) b_in = true;
            }
            if (!a_in || !b_in) continue;
            if (a != newest && b != newest) continue;

            for (int pass = 0; pass < 2; pass++) {
                char ch = (pass == 0) ? a : b;
                EncounterRecord cb[30];
                int cc = progress->newest_letter_encounters(ch, 30, cb, 30);
                if (cc >= 30) {
                    int cor = 0;
                    for (int i = 0; i < cc; i++) if (cb[i].correct) cor++;
                    if ((float)cor / cc < CONFUSABLE_ACCURACY) return false;
                }
            }
        }

        // All criteria met — advance!
        return _advance(msg_buf, msg_len);
    }

    // Print session summary to debug serial
    void print_summary(Print &out) {
        unsigned long elapsed = millis() - session.start_time;
        int mins = elapsed / 60000;
        int secs = (elapsed / 1000) % 60;

        out.println("\n--- Session Summary ---");
        out.printf("Items: %d  Accuracy: %d%%  Time: %dm%02ds\n",
                   session.items_practiced,
                   (int)(session.accuracy() * 100),
                   mins, secs);

        // Per-letter accuracy
        char letters[NUM_LETTERS];
        int n;
        introduced_letters(letters, &n);
        out.print("Per letter: ");
        for (int i = 0; i < n; i++) {
            int idx = letters[i] - 'a';
            if (session.letter_seen[idx] > 0) {
                int acc = session.letter_correct[idx] * 100 / session.letter_seen[idx];
                out.printf("%c:%d%% ", letters[i] - 32, acc);
            }
        }
        out.println();

        // Weakest
        char worst_ch = 0;
        float worst_acc = 1.0f;
        for (int i = 0; i < n; i++) {
            int idx = letters[i] - 'a';
            if (session.letter_seen[idx] >= 3) {
                float a = (float)session.letter_correct[idx] / session.letter_seen[idx];
                if (a < worst_acc) {
                    worst_acc = a;
                    worst_ch = letters[i];
                }
            }
        }
        if (worst_ch && worst_acc < 0.9f) {
            out.printf("Weakest: %c (%d%%)\n", worst_ch - 32, (int)(worst_acc * 100));
        }

        out.printf("Level: %d", progress->level);
        if (progress->level < NUM_LETTERS) {
            char next = TEACH_ORDER[progress->level];
            char ds[32];
            dots_str(next, ds, sizeof(ds));
            out.printf(", next: %c (%s)", next - 32, ds);
        }
        out.println("\n-----------------------");
    }

private:
    char _contrast_queue[4];
    int _contrast_queue_len = 0;
    char _last_letter = 0;

    void _maybe_queue_contrast(const char *letters, int n) {
        if (_contrast_queue_len > 0) return;
        if (session.items_practiced % 15 != 0 || session.items_practiced == 0) return;

        for (int p = 0; p < NUM_CONFUSABLE_PAIRS; p++) {
            char a = CONFUSABLE_PAIRS[p].a, b = CONFUSABLE_PAIRS[p].b;
            bool a_in = false, b_in = false;
            for (int i = 0; i < n; i++) {
                if (letters[i] == a) a_in = true;
                if (letters[i] == b) b_in = true;
            }
            if (a_in && b_in) {
                if (progress->get_letter(a).accuracy() < 0.85f ||
                    progress->get_letter(b).accuracy() < 0.85f) {
                    _contrast_queue[0] = a;
                    _contrast_queue[1] = b;
                    _contrast_queue_len = 2;
                    return;
                }
            }
        }
    }

    bool _advance(char *msg_buf, int msg_len) {
        int new_level = progress->level + 1;
        char next = TEACH_ORDER[new_level - 1];
        progress->level = new_level;
        progress->level_trial_count = 0;

        char ds[32];
        dots_str(next, ds, sizeof(ds));
        int pos = snprintf(msg_buf, msg_len, "Level %d! New: %c (%s)",
                           new_level, next - 32, ds);

        // Teaching message
        _teaching_message(next, new_level, msg_buf + pos, msg_len - pos);
        return true;
    }

    void _teaching_message(char letter, int level, char *buf, int buflen) {
        if (letter == 'o' && level == 4) {
            snprintf(buf, buflen, "\nO = E + dot 3. You'll see this pattern again.");
            return;
        }
        if (letter == 's' && level == 5) {
            snprintf(buf, buflen, "\nS = I + dot 3, same rule as O = E + dot 3.");
            return;
        }
        if (letter == 'w') {
            snprintf(buf, buflen, "\nW is an exception: dots 2,4,5,6. Memorize it.");
            return;
        }

        // Stage B: base + dot 3
        for (int i = 0; i < NUM_STAGE_B; i++) {
            if (STAGE_B[i].derived == letter && letter != 'o' && letter != 's') {
                char ds[32];
                dots_str(letter, ds, sizeof(ds));
                // Check if first stage B letter after o,s
                bool is_first = true;
                for (int j = 0; j < level - 1; j++) {
                    char prev = TEACH_ORDER[j];
                    for (int k = 0; k < NUM_STAGE_B; k++) {
                        if (STAGE_B[k].derived == prev && prev != 'o' && prev != 's') {
                            is_first = false;
                            break;
                        }
                    }
                    if (!is_first) break;
                }
                if (is_first) {
                    snprintf(buf, buflen,
                             "\nPattern: each next letter = A-J base + dot 3.");
                } else {
                    snprintf(buf, buflen, "\n%c = %c + dot 3 (%s)",
                             letter - 32, STAGE_B[i].base - 32, ds);
                }
                return;
            }
        }

        // Stage C: base + dots 3,6
        for (int i = 0; i < NUM_STAGE_C; i++) {
            if (STAGE_C[i].derived == letter) {
                char ds[32];
                dots_str(letter, ds, sizeof(ds));
                bool is_first = true;
                for (int j = 0; j < level - 1; j++) {
                    char prev = TEACH_ORDER[j];
                    for (int k = 0; k < NUM_STAGE_C; k++) {
                        if (STAGE_C[k].derived == prev) {
                            is_first = false;
                            break;
                        }
                    }
                    if (!is_first) break;
                }
                if (is_first) {
                    snprintf(buf, buflen,
                             "\nNew rule: A-E base + dots 3,6.");
                } else {
                    snprintf(buf, buflen, "\n%c = %c + dots 3,6 (%s)",
                             letter - 32, STAGE_C[i].base - 32, ds);
                }
                return;
            }
        }

        buf[0] = 0;  // no message
    }
};

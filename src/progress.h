/*
 * Progress persistence: per-letter stats, encounter records.
 * Ported from brailletrain/progress.py
 *
 * Uses LittleFS for JSON storage on ESP32 flash.
 */
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#define MAX_ENCOUNTERS 500  // ring buffer size (trim oldest)
#define MAX_CONFUSIONS 26   // per letter: confused_with[26]
#define PROGRESS_PATH "/progress.json"

struct LetterStats {
    int seen_count = 0;
    int correct_count = 0;
    int streak = 0;
    unsigned long last_seen = 0;  // millis()
    int confused_with[26] = {};   // index by (char - 'a')

    float accuracy() const {
        if (seen_count == 0) return 0.0f;
        return (float)correct_count / seen_count;
    }

    void record(bool correct, char confused = 0) {
        seen_count++;
        if (correct) {
            correct_count++;
            streak++;
        } else {
            streak = 0;
            if (confused >= 'a' && confused <= 'z')
                confused_with[confused - 'a']++;
        }
        last_seen = millis();
    }
};

struct EncounterRecord {
    char letter;
    bool correct;
    unsigned long timestamp;  // millis()
};

struct Progress {
    int level = 1;
    LetterStats letters[26] = {};
    EncounterRecord encounters[MAX_ENCOUNTERS] = {};
    int encounter_count = 0;
    int encounter_head = 0;  // ring buffer write position
    int level_trial_count = 0;

    LetterStats &get_letter(char ch) {
        if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a';
        return letters[ch - 'a'];
    }

    void record_encounter(char letter, bool correct) {
        if (letter >= 'A' && letter <= 'Z') letter = letter - 'A' + 'a';
        encounters[encounter_head] = {letter, correct, millis()};
        encounter_head = (encounter_head + 1) % MAX_ENCOUNTERS;
        if (encounter_count < MAX_ENCOUNTERS) encounter_count++;
    }

    // Get last n encounters for a specific letter, returns count found
    int newest_letter_encounters(char letter, int n, EncounterRecord *out, int out_max) const {
        if (letter >= 'A' && letter <= 'Z') letter = letter - 'A' + 'a';
        int found = 0;
        // Walk backwards from most recent
        for (int i = 0; i < encounter_count && found < n && found < out_max; i++) {
            int idx = (encounter_head - 1 - i + MAX_ENCOUNTERS) % MAX_ENCOUNTERS;
            if (encounters[idx].letter == letter) {
                out[found++] = encounters[idx];
            }
        }
        return found;
    }

    // Get last n encounters from introduced set, returns count found
    int recent_encounters(int n, const char *letter_set, int set_len,
                          EncounterRecord *out, int out_max) const {
        int found = 0;
        for (int i = 0; i < encounter_count && found < n && found < out_max; i++) {
            int idx = (encounter_head - 1 - i + MAX_ENCOUNTERS) % MAX_ENCOUNTERS;
            char lt = encounters[idx].letter;
            bool in_set = false;
            for (int j = 0; j < set_len; j++) {
                if (letter_set[j] == lt) { in_set = true; break; }
            }
            if (in_set) out[found++] = encounters[idx];
        }
        return found;
    }

    // Save to LittleFS as JSON
    bool save() const {
        JsonDocument doc;
        doc["level"] = level;
        doc["level_trial_count"] = level_trial_count;

        JsonObject letters_obj = doc["letters"].to<JsonObject>();
        for (int i = 0; i < 26; i++) {
            if (letters[i].seen_count == 0) continue;
            char key[2] = {(char)('a' + i), 0};
            JsonObject lo = letters_obj[key].to<JsonObject>();
            lo["seen"] = letters[i].seen_count;
            lo["correct"] = letters[i].correct_count;
            lo["streak"] = letters[i].streak;
            lo["last_seen"] = letters[i].last_seen;
            // Only save non-zero confusions
            JsonObject conf = lo["conf"].to<JsonObject>();
            for (int j = 0; j < 26; j++) {
                if (letters[i].confused_with[j] > 0) {
                    char ck[2] = {(char)('a' + j), 0};
                    conf[ck] = letters[i].confused_with[j];
                }
            }
        }

        // Save last N encounters (most recent)
        JsonArray enc_arr = doc["encounters"].to<JsonArray>();
        int to_save = min(encounter_count, MAX_ENCOUNTERS);
        for (int i = 0; i < to_save; i++) {
            // Oldest first
            int idx = (encounter_head - to_save + i + MAX_ENCOUNTERS) % MAX_ENCOUNTERS;
            JsonObject e = enc_arr.add<JsonObject>();
            char lk[2] = {encounters[idx].letter, 0};
            e["l"] = (const char *)lk;
            e["c"] = encounters[idx].correct ? 1 : 0;
            e["t"] = encounters[idx].timestamp;
        }

        File f = LittleFS.open(PROGRESS_PATH, "w");
        if (!f) return false;
        serializeJson(doc, f);
        f.close();
        return true;
    }

    // Load from LittleFS
    bool load() {
        File f = LittleFS.open(PROGRESS_PATH, "r");
        if (!f) return false;

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        f.close();
        if (err) return false;

        level = doc["level"] | 1;
        level_trial_count = doc["level_trial_count"] | 0;

        JsonObject letters_obj = doc["letters"].as<JsonObject>();
        for (JsonPair kv : letters_obj) {
            char ch = kv.key().c_str()[0];
            if (ch < 'a' || ch > 'z') continue;
            int idx = ch - 'a';
            JsonObject lo = kv.value().as<JsonObject>();
            letters[idx].seen_count = lo["seen"] | 0;
            letters[idx].correct_count = lo["correct"] | 0;
            letters[idx].streak = lo["streak"] | 0;
            letters[idx].last_seen = lo["last_seen"] | (unsigned long)0;
            JsonObject conf = lo["conf"].as<JsonObject>();
            for (JsonPair ck : conf) {
                char cc = ck.key().c_str()[0];
                if (cc >= 'a' && cc <= 'z')
                    letters[idx].confused_with[cc - 'a'] = ck.value().as<int>();
            }
        }

        JsonArray enc_arr = doc["encounters"].as<JsonArray>();
        encounter_count = 0;
        encounter_head = 0;
        for (JsonObject e : enc_arr) {
            if (encounter_count >= MAX_ENCOUNTERS) break;
            const char *ls = e["l"] | "?";
            encounters[encounter_head].letter = ls[0];
            encounters[encounter_head].correct = (e["c"] | 0) != 0;
            encounters[encounter_head].timestamp = e["t"] | (unsigned long)0;
            encounter_head = (encounter_head + 1) % MAX_ENCOUNTERS;
            encounter_count++;
        }
        return true;
    }
};

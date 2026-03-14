/*
 * Word list for braille training. Uses Oxford 5000 with frequency weights.
 * Filtered at runtime by introduced letters.
 */
#pragma once

#include "english_words.h"

// Filter words eligible at current level (all chars in introduced set, a-z only)
// Returns count of eligible words, fills `out` up to `out_max`
static int filter_words(const char *introduced, int intro_count,
                        const char **out, int out_max,
                        int min_len = 2, int max_len = 8) {
    int n = 0;
    for (size_t w = 0; w < OXFORD_WORD_COUNT && n < out_max; w++) {
        const char *word = words[w].word;
        int len = strlen(word);
        if (len < min_len || len > max_len) continue;

        bool ok = true;
        for (int i = 0; i < len; i++) {
            char ch = word[i];
            if (ch < 'a' || ch > 'z') { ok = false; break; }
            bool found = false;
            for (int j = 0; j < intro_count; j++) {
                if (introduced[j] == ch) { found = true; break; }
            }
            if (!found) { ok = false; break; }
        }
        if (ok) out[n++] = word;
    }
    return n;
}

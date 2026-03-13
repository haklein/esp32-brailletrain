/*
 * Static braille data: dot patterns, confusable pairs, teaching order.
 * Ported from brailletrain/braille_data.py
 */
#pragma once
#include <Arduino.h>

#define NUM_LETTERS 26
#define HT_CELLS    40

// Teaching order: e,a,i,o,s,h,b,c,d,f,g,j,t,n,r,l,k,m,p,q,u,y,v,x,z,w
static const char TEACH_ORDER[NUM_LETTERS] = {
    'e','a','i','o','s','h','b','c','d','f','g','j',
    't','n','r','l','k','m','p','q','u','y','v','x','z','w',
};

// Dot patterns per letter (1-indexed dots encoded as bitmask: dot N -> bit N-1)
// a=d1, b=d12, c=d14, etc.
static const uint8_t LETTER_CELL[26] = {
    0x01, // a: d1
    0x03, // b: d12
    0x09, // c: d14
    0x19, // d: d145
    0x11, // e: d15
    0x0B, // f: d124
    0x1B, // g: d1245
    0x13, // h: d125
    0x0A, // i: d24
    0x1A, // j: d245
    0x05, // k: d13
    0x07, // l: d123
    0x0D, // m: d134
    0x1D, // n: d1345
    0x15, // o: d135
    0x0F, // p: d1234
    0x1F, // q: d12345
    0x17, // r: d1235
    0x0E, // s: d234
    0x1E, // t: d2345
    0x25, // u: d136
    0x27, // v: d1236
    0x3A, // w: d2456
    0x2D, // x: d1346
    0x3D, // y: d13456
    0x35, // z: d1356
};

// Confusable pairs (indices into 'a'-based)
struct ConfusablePair {
    char a, b;
};

static const ConfusablePair CONFUSABLE_PAIRS[] = {
    {'d','f'}, {'e','i'}, {'h','j'}, {'m','n'},
    {'o','p'}, {'r','w'}, {'s','t'},
};
#define NUM_CONFUSABLE_PAIRS 7

// Stage B: letter = base + dot3
struct StageBase {
    char derived, base;
};

static const StageBase STAGE_B[] = {
    {'k','a'}, {'l','b'}, {'m','c'}, {'n','d'}, {'o','e'},
    {'p','f'}, {'q','g'}, {'r','h'}, {'s','i'}, {'t','j'},
};
#define NUM_STAGE_B 10

static const StageBase STAGE_C[] = {
    {'u','a'}, {'v','b'}, {'x','c'}, {'y','d'}, {'z','e'},
};
#define NUM_STAGE_C 5

// Convert char to cell byte
inline uint8_t char_to_cell(char c) {
    if (c >= 'a' && c <= 'z') return LETTER_CELL[c - 'a'];
    if (c >= 'A' && c <= 'Z') return LETTER_CELL[c - 'A'];
    return 0x00;
}

// Convert cell byte back to letter, or 0 if not found
inline char cell_to_char(uint8_t cell) {
    for (int i = 0; i < 26; i++) {
        if (LETTER_CELL[i] == cell) return 'a' + i;
    }
    return 0;
}

// Format dots as string like "dots 1,3,5" into buffer
inline void dots_str(char letter, char *buf, int buflen) {
    uint8_t cell = char_to_cell(letter);
    if (cell == 0) { snprintf(buf, buflen, "?"); return; }

    int pos = 0;
    int count = 0;
    for (int d = 1; d <= 8; d++) {
        if (cell & (1 << (d - 1))) count++;
    }
    pos += snprintf(buf + pos, buflen - pos, count == 1 ? "dot " : "dots ");
    bool first = true;
    for (int d = 1; d <= 8; d++) {
        if (cell & (1 << (d - 1))) {
            if (!first) pos += snprintf(buf + pos, buflen - pos, ",");
            pos += snprintf(buf + pos, buflen - pos, "%d", d);
            first = false;
        }
    }
}

// Check if two letters are a confusable pair
inline bool is_confusable(char a, char b) {
    for (int i = 0; i < NUM_CONFUSABLE_PAIRS; i++) {
        if ((CONFUSABLE_PAIRS[i].a == a && CONFUSABLE_PAIRS[i].b == b) ||
            (CONFUSABLE_PAIRS[i].a == b && CONFUSABLE_PAIRS[i].b == a))
            return true;
    }
    return false;
}

// Check if letter has a confusable partner in given set
inline bool has_confusable_in(char letter, const char *set, int set_len) {
    for (int i = 0; i < set_len; i++) {
        if (is_confusable(letter, set[i])) return true;
    }
    return false;
}

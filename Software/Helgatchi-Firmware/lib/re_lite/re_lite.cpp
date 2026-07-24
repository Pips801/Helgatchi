#include "re_lite.h"
#include <string.h>

namespace {

inline char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isWord(char c)  { char l = lc(c); return c == '_' || isDigit(c) || (l >= 'a' && l <= 'z'); }
inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// Does the class-escape letter `esc` (the char after a backslash) match `c`?
// A non-class escape (\. \- \\ ...) is treated as an escaped literal.
bool matchClassEsc(char esc, char c) {
    switch (esc) {
        case 'd': return  isDigit(c);
        case 'D': return !isDigit(c);
        case 'w': return  isWord(c);
        case 'W': return !isWord(c);
        case 's': return  isSpace(c);
        case 'S': return !isSpace(c);
        default:  return lc(esc) == lc(c);   // escaped literal
    }
}

// Length in pattern chars of the bracket element starting at p ('['), through
// the closing ']' (or end of string if unterminated). Must stay byte-for-byte
// in step with matchBracket's member walk.
int bracketSpan(const char* p) {
    const char* q = p + 1;
    if (*q == '^') q++;
    if (*q == ']') q++;                 // a ']' right after '[' / '[^' is literal
    while (*q && *q != ']') {
        if (*q == '\\' && q[1]) q += 2;
        else                    q++;
    }
    if (*q == ']') q++;
    return (int)(q - p);
}

// Does char `c` match the bracket element at p ('[')? Case-insensitive; walks
// members identically to bracketSpan.
bool matchBracket(const char* p, char c) {
    const char* q = p + 1;
    bool neg = false;
    if (*q == '^') { neg = true; q++; }
    bool matched = false;
    bool first = true;
    while (*q && (*q != ']' || first)) {
        first = false;
        char lo;
        if (*q == '\\' && q[1]) {
            char cls = q[1];
            q += 2;
            if (cls == 'd' || cls == 'D' || cls == 'w' ||
                cls == 'W' || cls == 's' || cls == 'S') {
                if (matchClassEsc(cls, c)) matched = true;
                continue;
            }
            lo = cls;   // escaped literal
        } else {
            lo = *q; q++;
        }
        if (*q == '-' && q[1] && q[1] != ']') {
            q++;                        // consume '-'
            char hi;
            if (*q == '\\' && q[1]) { hi = q[1]; q += 2; }
            else                    { hi = *q;  q++;    }
            char cl = lc(c);
            if ((c >= lo && c <= hi) || (cl >= lc(lo) && cl <= lc(hi))) matched = true;
        } else {
            if (lc(c) == lc(lo)) matched = true;
        }
    }
    return neg ? !matched : matched;
}

// Pattern-char length of the single element at `re` (excluding any quantifier).
int elemLen(const char* re) {
    if (re[0] == '\\') return re[1] ? 2 : 1;
    if (re[0] == '[')  return bracketSpan(re);
    return 1;   // '.' or literal
}

// Does the single element at `re` match char `c`? (`c` is never NUL here.)
bool elemMatch1(const char* re, char c) {
    if (re[0] == '\\') return matchClassEsc(re[1], c);
    if (re[0] == '[')  return matchBracket(re, c);
    if (re[0] == '.')  return true;
    return lc(re[0]) == lc(c);
}

bool matchhere(const char* re, const char* text);

// Match element `elem` (length `elen`) repeated, then `rest`. `min` is 0 for
// '*', 1 for '+'. Greedy with backtracking so a trailing anchor/element can
// still line up with the end of `text`.
bool matchstar(int min, const char* elem, const char* rest, const char* text) {
    const char* t = text;
    int count = 0;
    while (*t && elemMatch1(elem, *t)) { t++; count++; }
    for (int i = count; i >= min; i--) {
        if (matchhere(rest, text + i)) return true;
    }
    return false;
}

bool matchhere(const char* re, const char* text) {
    if (re[0] == '^') return matchhere(re + 1, text);          // redundant anchor
    if (re[0] == '\0') return *text == '\0';                   // full-match terminal
    if (re[0] == '$' && re[1] == '\0') return *text == '\0';

    const int  elen = elemLen(re);
    const char q    = re[elen];
    const char* rest = (q == '*' || q == '+' || q == '?') ? re + elen + 1 : re + elen;

    if (q == '*') return matchstar(0, re, rest, text);
    if (q == '+') return matchstar(1, re, rest, text);
    if (q == '?') {
        if (*text && elemMatch1(re, *text) && matchhere(rest, text + 1)) return true;
        return matchhere(rest, text);
    }
    if (*text && elemMatch1(re, *text)) return matchhere(rest, text + 1);
    return false;
}

}  // namespace

bool re_lite_full_match(const char* pattern, const char* text) {
    if (!pattern || !text) return false;
    return matchhere(pattern, text);
}

bool re_lite_valid(const char* p) {
    if (!p) return false;
    const int n = (int)strlen(p);
    if (n > RE_LITE_MAX_LEN) return false;

    bool prevElem = false;   // is there an operand a quantifier could bind to?
    int i = 0;
    while (p[i]) {
        const char c = p[i];
        if (c == '\\') {
            if (!p[i + 1]) return false;          // dangling backslash
            i += 2; prevElem = true; continue;
        }
        if (c == '[') {
            const int span = bracketSpan(p + i);
            if (span < 2 || p[i + span - 1] != ']') return false;   // unterminated
            i += span; prevElem = true; continue;
        }
        if (c == '*' || c == '+' || c == '?') {
            if (!prevElem) return false;          // quantifier with no operand
            prevElem = false; i++; continue;
        }
        if (c == '^' || c == '$') { i++; prevElem = false; continue; }
        i++; prevElem = true;                      // '.' or literal
    }
    return true;
}

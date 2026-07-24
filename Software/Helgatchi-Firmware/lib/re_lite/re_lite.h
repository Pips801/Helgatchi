#pragma once
#include <stddef.h>

// ---------------------------------------------------------------------------
// re_lite — a tiny, case-insensitive, full-match regex matcher.
//
// Purpose-built for RulesService pattern fields (name / ssid / oui_org /
// mfg_org). NOT a general regex library: it exists to back the rare
// "genuinely regex" tier after RulesService's literal/contains/prefix/suffix
// fast-paths have already handled every plain pattern. See docs/WRITING_RULES.md.
//
// Semantics:
//   - FULL match: the pattern must match the ENTIRE text (implicitly anchored
//     at both ends). A leading '^' / trailing '$' are accepted and redundant.
//     "contains" is written .*x.* , "starts-with" x.* , "ends-with" .*x .
//   - Case-insensitive throughout (literals and [ranges]).
//
// Supported syntax:
//   .            any single character
//   *  +  ?      quantify the preceding single element (greedy, backtracking)
//   \d \D        digit / non-digit
//   \w \W        word [A-Za-z0-9_] / non-word
//   \s \S        whitespace / non-whitespace
//   \<c>         escaped literal (\. \* \\ \[ ...)
//   [abc] [a-z]  character set;  [^...] negated set;  \d etc. allowed inside
//   ^  $         start / end anchor (redundant under full-match, tolerated)
//
// NOT supported: alternation '|' (rule value arrays are already OR'd),
// bounded repetition {m,n}, capture groups, backreferences.
//
// Recursion depth and backtracking are bounded by pattern length (capped at
// RE_LITE_MAX_LEN); patterns are trusted local config, not adversarial input.
// ---------------------------------------------------------------------------

static const int RE_LITE_MAX_LEN = 64;

// Match `pattern` against the whole of `text`. Returns true only on a full
// match. Null args return false.
bool re_lite_full_match(const char* pattern, const char* text);

// Best-effort syntax check: balanced '[' ']', no dangling '\', every
// quantifier has an operand, length within RE_LITE_MAX_LEN. Returns true if
// the pattern is usable by re_lite_full_match.
bool re_lite_valid(const char* pattern);

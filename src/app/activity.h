#pragma once
#include <Arduino.h>

// Pure state/activity tables and lookups: no I/O, no globals mutated. The word
// lists, per-clip timeouts and state->look mappings all live here.
namespace app {

// rotating "busy" verb, advanced in sync with the character animation loop
extern const char *WHIMSY[];
extern const int N_WHIMSY;

// Idle micro-behaviour: gently rotate a friendly line while standing by.
extern const char *IDLE_MSGS[];
extern const int N_IDLE;

// Map the hook's burst (tool calls/min) + active subagents to a 0/1/2 tier.
int intensityTier(int burst, int agents);

// the running "work" clips share one look (WORKING / coral / blue LED) and a verb
bool isWork(const char *st);

// How long an activity may go silent (no new hook event) before we treat it as
// stale/abandoned and drop back to calm (see the per-clip rationale in the .cpp).
uint32_t actTimeout(const char *st);

const char *actVerb(const char *st);

uint16_t stateColor(const char *st);

const char *stateLabel(const char *st);

} // namespace app

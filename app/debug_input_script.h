#pragma once
#include <string>

namespace debug_input_script {

// If the environment variable VIDEO_COMPARE_INPUT_SCRIPT is set to a readable
// file path, spawns a detached thread that reads the script and injects SDL
// events via SDL_PushEvent. No-op otherwise.
//
// Script format — one command per line, whitespace-separated tokens:
//
//   sleep <seconds>                    — sleep N seconds (float, e.g. 0.25)
//   keypress <key>                     — push KEY_DOWN then KEY_UP (short gap)
//   keypress <key> <count> <interval>  — press <count> times with <interval>s between
//   hold <key> <seconds>               — KEY_DOWN, sleep N, KEY_UP
//   keydown <key>                      — push KEY_DOWN only (no auto-release)
//   keyup   <key>                      — push KEY_UP only
//   modshift on|off                    — set/clear shift modifier for subsequent key events
//   modctrl  on|off                    — set/clear ctrl modifier
//   modalt   on|off                    — set/clear alt modifier
//   quit                               — push SDL_EVENT_QUIT
//   log <message>                      — log the rest of the line to stderr
//   #<anything>                        — comment (also honors //, ;)
//
// Recognized <key> names (case-insensitive):
//   space, escape (esc), return (enter), tab, backspace, minus (-), equals (+),
//   plus, period (.), comma (,), up, down, left, right, pageup, pagedown,
//   home, end, f1..f12, a..z, 0..9, and single-character keys for any of those.
//
// Example:
//   # Let playback settle, pause, step back 15 frames, quit.
//   sleep 5.0
//   keypress space
//   sleep 4.0
//   keypress - 15 0.12
//   sleep 0.5
//   quit
//
void start_from_env();

/**
 * Whether the scripted input thread is currently executing. Returns true from
 * the moment start_from_env() launches the worker until the script completes
 * (successfully or via early exit). Returns false when no script was loaded.
 * Safe to call from any thread.
 */
bool is_running();

}  // namespace debug_input_script

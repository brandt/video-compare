#pragma once
#include <SDL3/SDL.h>
#include <string>

// Shared primitives for driving video-compare's main loop via synthesized
// SDL events. Both the script-based harness (`debug_input_script`) and the
// interactive socket server (`debug_input_socket`) consume this module so
// the key-name vocabulary and event-push helpers stay in one place.
namespace debug_input_common {

/**
 * Keyboard modifier state accumulator. Set the individual flags and call
 * as_keymod() to obtain the SDL bitmask suitable for SDL_KeyboardEvent::mod.
 */
struct ModState {
  bool shift{false};
  bool ctrl{false};
  bool alt{false};
  bool gui{false};

  /** Combine the individual flags into an SDL_Keymod bitmask. */
  SDL_Keymod as_keymod() const;
};

/** Return an ASCII-lowercased copy of `s`. */
std::string to_lower(std::string s);

/**
 * Resolve a user-supplied key name (case-insensitive) to an SDL_Keycode.
 * Accepts single-character keys ('a', '-', '5'), named keys ('space',
 * 'escape', 'return', 'tab', 'pageup', etc.), and 'f1'..'f12'.
 * Returns SDLK_UNKNOWN if the name is not recognized.
 */
SDL_Keycode name_to_keycode(const std::string& raw);

/**
 * Resolve a mouse-button name (case-insensitive) to an SDL button constant.
 * Recognizes 'left', 'middle', 'right', 'x1', 'x2'.
 * Returns 0 if the name is not recognized.
 */
int name_to_mouse_button(const std::string& raw);

/**
 * Push a keyboard KEY_DOWN or KEY_UP event into the SDL event queue.
 * No-op if `key` is SDLK_UNKNOWN.
 */
void push_key_event(bool down, SDL_Keycode key, SDL_Keymod mod);

/** Push an SDL_EVENT_QUIT event into the SDL event queue. */
void push_quit();

/**
 * Push a mouse motion event at absolute window coordinates (x, y). `xrel`
 * and `yrel` are the relative movement SDL normally reports; they drive
 * right-drag pan behavior in Display.
 */
void push_mouse_motion(SDL_WindowID window_id, float x, float y, float xrel, float yrel, Uint32 button_state);

/** Push an SDL_EVENT_MOUSE_BUTTON_DOWN or _UP event at (x, y). */
void push_mouse_button(SDL_WindowID window_id, int button, bool down, float x, float y);

/** Push an SDL_EVENT_MOUSE_WHEEL event (vertical-only `dy`) at (x, y). */
void push_mouse_wheel(SDL_WindowID window_id, float dy, float x, float y);

/** Sleep the current thread for `seconds` (fractional). No-op if <= 0. */
void sleep_seconds(double seconds);

}  // namespace debug_input_common

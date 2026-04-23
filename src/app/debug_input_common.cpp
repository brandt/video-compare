#include "app/debug_input_common.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace debug_input_common {

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

SDL_Keymod ModState::as_keymod() const {
  Uint16 m = SDL_KMOD_NONE;
  if (shift) {
    m |= SDL_KMOD_SHIFT;
  }
  if (ctrl) {
    m |= SDL_KMOD_CTRL;
  }
  if (alt) {
    m |= SDL_KMOD_ALT;
  }
  if (gui) {
    m |= SDL_KMOD_GUI;
  }
  return static_cast<SDL_Keymod>(m);
}

SDL_Keycode name_to_keycode(const std::string& raw) {
  if (raw.empty()) {
    return SDLK_UNKNOWN;
  }
  const std::string name = to_lower(raw);

  // Single-character fast path.
  if (name.size() == 1) {
    const char c = name[0];
    if (c >= 'a' && c <= 'z') {
      return static_cast<SDL_Keycode>(SDLK_A + (c - 'a'));
    }
    if (c >= '0' && c <= '9') {
      return static_cast<SDL_Keycode>(SDLK_0 + (c - '0'));
    }
    switch (c) {
      case '-':
        return SDLK_MINUS;
      case '+':
      case '=':
        return SDLK_EQUALS;
      case '.':
        return SDLK_PERIOD;
      case ',':
        return SDLK_COMMA;
      case '/':
        return SDLK_SLASH;
      case '\\':
        return SDLK_BACKSLASH;
      case ';':
        return SDLK_SEMICOLON;
      case '[':
        return SDLK_LEFTBRACKET;
      case ']':
        return SDLK_RIGHTBRACKET;
      case '`':
        return SDLK_GRAVE;
      case ' ':
        return SDLK_SPACE;
      default:
        break;
    }
    return SDLK_UNKNOWN;
  }

  // Named keys.
  if (name == "space" || name == "spc") {
    return SDLK_SPACE;
  }
  if (name == "escape" || name == "esc") {
    return SDLK_ESCAPE;
  }
  if (name == "return" || name == "enter") {
    return SDLK_RETURN;
  }
  if (name == "tab") {
    return SDLK_TAB;
  }
  if (name == "backspace" || name == "bsp") {
    return SDLK_BACKSPACE;
  }
  if (name == "minus") {
    return SDLK_MINUS;
  }
  if (name == "plus" || name == "equals") {
    return SDLK_EQUALS;
  }
  if (name == "period" || name == "dot") {
    return SDLK_PERIOD;
  }
  if (name == "comma") {
    return SDLK_COMMA;
  }
  if (name == "up") {
    return SDLK_UP;
  }
  if (name == "down") {
    return SDLK_DOWN;
  }
  if (name == "left") {
    return SDLK_LEFT;
  }
  if (name == "right") {
    return SDLK_RIGHT;
  }
  if (name == "pageup" || name == "pgup") {
    return SDLK_PAGEUP;
  }
  if (name == "pagedown" || name == "pgdn") {
    return SDLK_PAGEDOWN;
  }
  if (name == "home") {
    return SDLK_HOME;
  }
  if (name == "end") {
    return SDLK_END;
  }
  if (name == "delete" || name == "del") {
    return SDLK_DELETE;
  }
  if (name == "insert" || name == "ins") {
    return SDLK_INSERT;
  }
  if (name == "grave" || name == "backtick" || name == "backquote") {
    return SDLK_GRAVE;
  }

  // F1..F12.
  if (name.size() >= 2 && name[0] == 'f') {
    try {
      const int n = std::stoi(name.substr(1));
      if (n >= 1 && n <= 12) {
        return static_cast<SDL_Keycode>(SDLK_F1 + (n - 1));
      }
    } catch (const std::exception&) {
      // Fall through to the unknown return below.
    }
  }

  return SDLK_UNKNOWN;
}

int name_to_mouse_button(const std::string& raw) {
  if (raw.empty()) {
    return 0;
  }
  const std::string name = to_lower(raw);
  if (name == "left" || name == "l" || name == "1") {
    return SDL_BUTTON_LEFT;
  }
  if (name == "middle" || name == "m" || name == "2") {
    return SDL_BUTTON_MIDDLE;
  }
  if (name == "right" || name == "r" || name == "3") {
    return SDL_BUTTON_RIGHT;
  }
  if (name == "x1" || name == "4") {
    return SDL_BUTTON_X1;
  }
  if (name == "x2" || name == "5") {
    return SDL_BUTTON_X2;
  }
  return 0;
}

void push_key_event(bool down, SDL_Keycode key, SDL_Keymod mod) {
  if (key == SDLK_UNKNOWN) {
    return;
  }
  SDL_Event ev{};
  // SDL_Event is a union; ev.type aliases ev.key.type and covers both.
  ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  ev.key.timestamp = SDL_GetTicksNS();
  // handle_event only filters windowID for SDL_EVENT_WINDOW_*, so 0 is fine.
  ev.key.windowID = 0;
  ev.key.key = key;
  ev.key.mod = mod;
  ev.key.down = down;
  ev.key.repeat = false;
  SDL_PushEvent(&ev);
}

void push_quit() {
  SDL_Event ev{};
  ev.type = SDL_EVENT_QUIT;
  ev.quit.timestamp = SDL_GetTicksNS();
  SDL_PushEvent(&ev);
}

void push_mouse_motion(SDL_WindowID window_id, float x, float y, float xrel, float yrel, Uint32 button_state) {
  SDL_Event ev{};
  ev.type = SDL_EVENT_MOUSE_MOTION;
  ev.motion.timestamp = SDL_GetTicksNS();
  ev.motion.windowID = window_id;
  ev.motion.which = 0;
  ev.motion.state = button_state;
  ev.motion.x = x;
  ev.motion.y = y;
  ev.motion.xrel = xrel;
  ev.motion.yrel = yrel;
  SDL_PushEvent(&ev);
}

void push_mouse_button(SDL_WindowID window_id, int button, bool down, float x, float y) {
  if (button == 0) {
    return;
  }
  SDL_Event ev{};
  ev.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
  ev.button.timestamp = SDL_GetTicksNS();
  ev.button.windowID = window_id;
  ev.button.which = 0;
  ev.button.button = static_cast<Uint8>(button);
  ev.button.down = down;
  ev.button.clicks = 1;
  ev.button.x = x;
  ev.button.y = y;
  SDL_PushEvent(&ev);
}

void push_mouse_wheel(SDL_WindowID window_id, float dy, float x, float y) {
  SDL_Event ev{};
  ev.type = SDL_EVENT_MOUSE_WHEEL;
  ev.wheel.timestamp = SDL_GetTicksNS();
  ev.wheel.windowID = window_id;
  ev.wheel.which = 0;
  ev.wheel.x = 0.0f;
  ev.wheel.y = dy;
  ev.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
  ev.wheel.mouse_x = x;
  ev.wheel.mouse_y = y;
  SDL_PushEvent(&ev);
}

void sleep_seconds(double seconds) {
  if (seconds <= 0.0) {
    return;
  }
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

}  // namespace debug_input_common

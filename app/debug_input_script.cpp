#include "app/debug_input_script.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

namespace debug_input_script {

namespace {

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// Map a user-supplied key name (case-insensitive) to an SDL_Keycode.
// Single-character inputs ('a', '-', '5') are handled by the char fallback.
// Returns SDLK_UNKNOWN if nothing matches.
SDL_Keycode name_to_keycode(const std::string& raw) {
  if (raw.empty()) return SDLK_UNKNOWN;
  const std::string name = to_lower(raw);

  // Single-character fast path.
  if (name.size() == 1) {
    const char c = name[0];
    if (c >= 'a' && c <= 'z') return static_cast<SDL_Keycode>(SDLK_A + (c - 'a'));
    if (c >= '0' && c <= '9') return static_cast<SDL_Keycode>(SDLK_0 + (c - '0'));
    switch (c) {
      case '-': return SDLK_MINUS;
      case '+': return SDLK_EQUALS;  // SDL usually doesn't have a bare '+' key; '=' works per display_input
      case '=': return SDLK_EQUALS;
      case '.': return SDLK_PERIOD;
      case ',': return SDLK_COMMA;
      case '/': return SDLK_SLASH;
      case '\\': return SDLK_BACKSLASH;
      case ';': return SDLK_SEMICOLON;
      case '[': return SDLK_LEFTBRACKET;
      case ']': return SDLK_RIGHTBRACKET;
      case '`': return SDLK_GRAVE;
      case ' ': return SDLK_SPACE;
      default: break;
    }
    return SDLK_UNKNOWN;
  }

  // Named keys.
  if (name == "space" || name == "spc")          return SDLK_SPACE;
  if (name == "escape" || name == "esc")         return SDLK_ESCAPE;
  if (name == "return" || name == "enter")       return SDLK_RETURN;
  if (name == "tab")                             return SDLK_TAB;
  if (name == "backspace" || name == "bsp")      return SDLK_BACKSPACE;
  if (name == "minus")                           return SDLK_MINUS;
  if (name == "plus" || name == "equals")        return SDLK_EQUALS;
  if (name == "period" || name == "dot")         return SDLK_PERIOD;
  if (name == "comma")                           return SDLK_COMMA;
  if (name == "up")                              return SDLK_UP;
  if (name == "down")                            return SDLK_DOWN;
  if (name == "left")                            return SDLK_LEFT;
  if (name == "right")                           return SDLK_RIGHT;
  if (name == "pageup" || name == "pgup")        return SDLK_PAGEUP;
  if (name == "pagedown" || name == "pgdn")      return SDLK_PAGEDOWN;
  if (name == "home")                            return SDLK_HOME;
  if (name == "end")                             return SDLK_END;
  if (name == "delete" || name == "del")         return SDLK_DELETE;
  if (name == "insert" || name == "ins")         return SDLK_INSERT;
  if (name == "grave" || name == "backtick" || name == "backquote") return SDLK_GRAVE;

  // F1..F12.
  if (name.size() >= 2 && name[0] == 'f') {
    try {
      const int n = std::stoi(name.substr(1));
      if (n >= 1 && n <= 12) return static_cast<SDL_Keycode>(SDLK_F1 + (n - 1));
    } catch (...) {
      /* fall through */
    }
  }

  return SDLK_UNKNOWN;
}

struct ModState {
  bool shift{false};
  bool ctrl{false};
  bool alt{false};

  SDL_Keymod as_keymod() const {
    Uint16 m = SDL_KMOD_NONE;
    if (shift) m |= SDL_KMOD_SHIFT;
    if (ctrl)  m |= SDL_KMOD_CTRL;
    if (alt)   m |= SDL_KMOD_ALT;
    return static_cast<SDL_Keymod>(m);
  }
};

void push_key_event(bool down, SDL_Keycode key, SDL_Keymod mod) {
  if (key == SDLK_UNKNOWN) return;
  SDL_Event ev{};
  // SDL_Event is a union; ev.type aliases ev.key.type and covers both.
  ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  ev.key.timestamp = SDL_GetTicksNS();
  ev.key.windowID = 0;  // handle_event only filters windowID for WINDOW_* events
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

void sleep_seconds(double sec) {
  if (sec <= 0.0) return;
  std::this_thread::sleep_for(std::chrono::duration<double>(sec));
}

// Split a line into whitespace-separated tokens, up to `max_tokens`. The
// remainder (if any) is returned as the last token unsplit. Trims surrounding
// whitespace on each token.
std::vector<std::string> tokenize(const std::string& line, std::size_t max_tokens = 8) {
  std::vector<std::string> out;
  std::istringstream iss(line);
  std::string tok;
  while (out.size() + 1 < max_tokens && iss >> tok) out.push_back(tok);
  if (iss >> tok) {
    std::string rest = tok;
    std::string tail;
    std::getline(iss, tail);
    rest += tail;
    out.push_back(rest);
  }
  return out;
}

bool parse_on_off(const std::string& tok, bool& out) {
  const std::string t = to_lower(tok);
  if (t == "on" || t == "true" || t == "1" || t == "yes")  { out = true;  return true; }
  if (t == "off" || t == "false" || t == "0" || t == "no") { out = false; return true; }
  return false;
}

void run_script(std::vector<std::string> lines) {
  const bool verbose = std::getenv("VIDEO_COMPARE_INPUT_SCRIPT_LOG") != nullptr;
  ModState mod;
  auto log = [&](const std::string& s) {
    if (verbose) std::cerr << "[input-script] " << s << std::endl;
  };

  for (std::size_t lineno = 0; lineno < lines.size(); ++lineno) {
    const std::string& raw = lines[lineno];
    // Trim leading whitespace.
    std::size_t i = 0;
    while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
    if (i == raw.size()) continue;                                             // blank
    if (raw[i] == '#' || (raw[i] == '/' && i + 1 < raw.size() && raw[i + 1] == '/') || raw[i] == ';') continue;  // comment

    const std::string line = raw.substr(i);
    auto tokens = tokenize(line, 8);
    if (tokens.empty()) continue;
    const std::string cmd = to_lower(tokens[0]);

    if (cmd == "sleep") {
      if (tokens.size() < 2) {
        std::cerr << "[input-script] line " << (lineno + 1) << ": sleep requires <seconds>" << std::endl;
        continue;
      }
      try {
        const double s = std::stod(tokens[1]);
        log("sleep " + std::to_string(s));
        sleep_seconds(s);
      } catch (...) {
        std::cerr << "[input-script] line " << (lineno + 1) << ": bad sleep value '" << tokens[1] << "'" << std::endl;
      }
    } else if (cmd == "keypress") {
      if (tokens.size() < 2) {
        std::cerr << "[input-script] line " << (lineno + 1) << ": keypress requires <key>" << std::endl;
        continue;
      }
      const SDL_Keycode key = name_to_keycode(tokens[1]);
      if (key == SDLK_UNKNOWN) {
        std::cerr << "[input-script] line " << (lineno + 1) << ": unknown key '" << tokens[1] << "'" << std::endl;
        continue;
      }
      int count = 1;
      double interval = 0.03;  // default 30ms between repeats (similar to key-repeat)
      if (tokens.size() >= 3) { try { count = std::stoi(tokens[2]); } catch (...) {} }
      if (tokens.size() >= 4) { try { interval = std::stod(tokens[3]); } catch (...) {} }
      log("keypress " + tokens[1] + " x" + std::to_string(count));
      const SDL_Keymod m = mod.as_keymod();
      for (int k = 0; k < count; ++k) {
        push_key_event(true, key, m);
        sleep_seconds(0.005);
        push_key_event(false, key, m);
        if (k + 1 < count) sleep_seconds(interval);
      }
    } else if (cmd == "hold") {
      if (tokens.size() < 3) {
        std::cerr << "[input-script] line " << (lineno + 1) << ": hold requires <key> <seconds>" << std::endl;
        continue;
      }
      const SDL_Keycode key = name_to_keycode(tokens[1]);
      if (key == SDLK_UNKNOWN) {
        std::cerr << "[input-script] line " << (lineno + 1) << ": unknown key '" << tokens[1] << "'" << std::endl;
        continue;
      }
      double hold_sec = 0.0;
      try { hold_sec = std::stod(tokens[2]); } catch (...) {}
      log("hold " + tokens[1] + " " + std::to_string(hold_sec) + "s");
      const SDL_Keymod m = mod.as_keymod();
      push_key_event(true, key, m);
      sleep_seconds(hold_sec);
      push_key_event(false, key, m);
    } else if (cmd == "keydown" || cmd == "keyup") {
      if (tokens.size() < 2) {
        std::cerr << "[input-script] line " << (lineno + 1) << ": " << cmd << " requires <key>" << std::endl;
        continue;
      }
      const SDL_Keycode key = name_to_keycode(tokens[1]);
      if (key == SDLK_UNKNOWN) {
        std::cerr << "[input-script] line " << (lineno + 1) << ": unknown key '" << tokens[1] << "'" << std::endl;
        continue;
      }
      log(cmd + " " + tokens[1]);
      push_key_event(cmd == "keydown", key, mod.as_keymod());
    } else if (cmd == "modshift" || cmd == "modctrl" || cmd == "modalt") {
      if (tokens.size() < 2) {
        std::cerr << "[input-script] line " << (lineno + 1) << ": " << cmd << " requires on|off" << std::endl;
        continue;
      }
      bool value = false;
      if (!parse_on_off(tokens[1], value)) {
        std::cerr << "[input-script] line " << (lineno + 1) << ": bad on|off value '" << tokens[1] << "'" << std::endl;
        continue;
      }
      if (cmd == "modshift")      mod.shift = value;
      else if (cmd == "modctrl")  mod.ctrl  = value;
      else                        mod.alt   = value;
      log(cmd + " " + (value ? "on" : "off"));
    } else if (cmd == "quit") {
      log("quit");
      push_quit();
    } else if (cmd == "log") {
      // Print the remainder of the line (including any whitespace).
      if (tokens.size() >= 2) std::cerr << "[input-script] " << tokens[1] << std::endl;
    } else {
      std::cerr << "[input-script] line " << (lineno + 1) << ": unknown command '" << cmd << "'" << std::endl;
    }
  }
  log("script complete");
}

}  // namespace

void start_from_env() {
  const char* path = std::getenv("VIDEO_COMPARE_INPUT_SCRIPT");
  if (path == nullptr || path[0] == '\0') return;

  std::ifstream file(path);
  if (!file) {
    std::cerr << "[input-script] cannot open '" << path << "'" << std::endl;
    return;
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(file, line)) lines.push_back(line);
  std::cerr << "[input-script] loaded " << lines.size() << " line(s) from " << path << std::endl;

  std::thread(
      [](std::vector<std::string> script) {
        run_script(std::move(script));
      },
      std::move(lines))
      .detach();
}

}  // namespace debug_input_script

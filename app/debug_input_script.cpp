#include "app/debug_input_script.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "app/debug_input_common.h"

namespace debug_input_script {

namespace {

using debug_input_common::ModState;
using debug_input_common::name_to_keycode;
using debug_input_common::push_key_event;
using debug_input_common::push_quit;
using debug_input_common::sleep_seconds;
using debug_input_common::to_lower;

// Tracks whether the scripted-input worker thread is currently running.
// Set true immediately before the thread is detached; set false by an RAII
// guard at the top of run_script so any return path clears it.
std::atomic<bool> g_script_running{false};

/** RAII guard that clears g_script_running when it goes out of scope. */
struct RunningGuard {
  ~RunningGuard() { g_script_running.store(false, std::memory_order_release); }
};

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
  // Clears the running flag on any return path (including exceptions).
  RunningGuard running_guard;
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

  // Publish running=true before launch so is_running() returns the truth
  // even if the caller queries before the worker schedules.
  g_script_running.store(true, std::memory_order_release);
  std::thread(
      [](std::vector<std::string> script) {
        run_script(std::move(script));
      },
      std::move(lines))
      .detach();
}

bool is_running() {
  return g_script_running.load(std::memory_order_acquire);
}

}  // namespace debug_input_script

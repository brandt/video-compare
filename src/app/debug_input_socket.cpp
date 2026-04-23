#include "app/debug_input_socket.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "app/debug_input_common.h"
#include "app/debug_input_script.h"
#include "app/video_compare.h"
#include "display/display.h"

namespace debug_input_socket {

namespace {

using json = nlohmann::json;

/** Path that was bound at start-up, stored so atexit can unlink it. */
std::mutex g_socket_path_mutex;
std::string g_socket_path;

bool g_verbose_logging{false};

/** Called at process exit to remove the socket file. */
void unlink_socket_path() {
  std::lock_guard<std::mutex> lock(g_socket_path_mutex);
  if (!g_socket_path.empty()) {
    unlink(g_socket_path.c_str());
    g_socket_path.clear();
  }
}

/** Emit `[input-socket] msg` to stderr, gated on VIDEO_COMPARE_INPUT_SOCK_LOG. */
void log_verbose(const std::string& msg) {
  if (g_verbose_logging) {
    std::cerr << "[input-socket] " << msg << std::endl;
  }
}

/** Emit `[input-socket] msg` to stderr unconditionally. */
void log_always(const std::string& msg) {
  std::cerr << "[input-socket] " << msg << std::endl;
}

/**
 * Write all bytes of `data` to `fd`, retrying on EINTR. Returns false if
 * the connection closes mid-write.
 */
bool send_all(int fd, const std::string& data) {
  const char* p = data.data();
  size_t remaining = data.size();
  while (remaining > 0) {
    const ssize_t n = ::send(fd, p, remaining, 0);
    if (n > 0) {
      p += n;
      remaining -= static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

/**
 * Read one \n-terminated line from `fd` into `out` (newline stripped).
 * Returns false on EOF / error.
 */
bool read_line(int fd, std::string& out) {
  out.clear();
  while (true) {
    char ch = 0;
    const ssize_t n = ::recv(fd, &ch, 1, 0);
    if (n == 0) {
      return false;
    }
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (ch == '\n') {
      if (!out.empty() && out.back() == '\r') {
        out.pop_back();
      }
      return true;
    }
    out.push_back(ch);
    // Cap line length to protect against runaway input.
    if (out.size() > 64 * 1024) {
      return false;
    }
  }
}

/** Build a success response JSON with a value payload. */
json ok_response(json value) {
  return json{{"ok", true}, {"value", std::move(value)}};
}

/** Build a success response JSON with no value payload. */
json ok_response_empty() {
  return json{{"ok", true}};
}

/** Build an error response JSON. */
json err_response(const std::string& message) {
  return json{{"ok", false}, {"error", message}};
}

/** Extract a required key of specified JSON type or throw. */
template <typename T>
T require_field(const json& req, const std::string& key) {
  auto it = req.find(key);
  if (it == req.end()) {
    throw std::runtime_error("missing required field '" + key + "'");
  }
  try {
    return it->get<T>();
  } catch (const std::exception& e) {
    throw std::runtime_error("field '" + key + "' has wrong type: " + e.what());
  }
}

/** Extract an optional key of specified JSON type, falling back to `fallback`. */
template <typename T>
T optional_field(const json& req, const std::string& key, T fallback) {
  auto it = req.find(key);
  if (it == req.end() || it->is_null()) {
    return fallback;
  }
  try {
    return it->get<T>();
  } catch (const std::exception&) {
    return fallback;
  }
}

/** Parse a JSON array of modifier name strings into a ModState. */
debug_input_common::ModState parse_mods(const json& req) {
  debug_input_common::ModState mods;
  auto it = req.find("mods");
  if (it == req.end() || !it->is_array()) {
    return mods;
  }
  for (const auto& entry : *it) {
    if (!entry.is_string()) {
      continue;
    }
    const std::string name = debug_input_common::to_lower(entry.get<std::string>());
    if (name == "shift") {
      mods.shift = true;
    } else if (name == "ctrl" || name == "control") {
      mods.ctrl = true;
    } else if (name == "alt" || name == "option") {
      mods.alt = true;
    } else if (name == "gui" || name == "cmd" || name == "super" || name == "meta") {
      mods.gui = true;
    }
  }
  return mods;
}

// -----------------------------------------------------------------------------
// Command handlers
// -----------------------------------------------------------------------------

/** Inject a keyboard event. */
json handle_key(const json& req) {
  const std::string action = debug_input_common::to_lower(require_field<std::string>(req, "action"));
  const std::string key_name = require_field<std::string>(req, "key");
  const SDL_Keycode code = debug_input_common::name_to_keycode(key_name);
  if (code == SDLK_UNKNOWN) {
    return err_response("unknown key name '" + key_name + "'");
  }
  const debug_input_common::ModState mods = parse_mods(req);
  const SDL_Keymod keymod = mods.as_keymod();

  // Update SDL's global modifier state so handlers that call SDL_GetModState
  // (wheel zoom sensitivity, shift-click seek scope) see the requested mods.
  SDL_SetModState(keymod);

  if (action == "press") {
    debug_input_common::push_key_event(true, code, keymod);
    debug_input_common::push_key_event(false, code, keymod);
  } else if (action == "down") {
    debug_input_common::push_key_event(true, code, keymod);
  } else if (action == "up") {
    debug_input_common::push_key_event(false, code, keymod);
  } else {
    return err_response("unknown key action '" + action + "' (expected press|down|up)");
  }
  return ok_response_empty();
}

/** Inject a mouse event. */
json handle_mouse(const json& req, const Session& session) {
  const std::string action = debug_input_common::to_lower(require_field<std::string>(req, "action"));
  SDL_Window* const window = session.display != nullptr ? session.display->get_main_window() : nullptr;
  const SDL_WindowID window_id = window != nullptr ? SDL_GetWindowID(window) : 0;

  // Same published-mod-state treatment as handle_key: callers that read
  // SDL_GetModState() (e.g. the shift-click scope in display_input) see the
  // requested modifiers at click time. Omitting `mods` leaves state alone.
  if (req.contains("mods")) {
    const debug_input_common::ModState mods = parse_mods(req);
    SDL_SetModState(mods.as_keymod());
  }

  if (action == "move") {
    const float x = require_field<double>(req, "x");
    const float y = require_field<double>(req, "y");
    if (window != nullptr) {
      // SDL_WarpMouseInWindow updates SDL's internal tracking AND emits a
      // native motion event, which reaches the main loop the same way a
      // real cursor move does. Prefer this over a raw SDL_PushEvent so
      // subsequent button events land at the right mouse_x_/mouse_y_.
      SDL_WarpMouseInWindow(window, x, y);
    } else {
      debug_input_common::push_mouse_motion(window_id, x, y, 0.0f, 0.0f, 0);
    }
    return ok_response_empty();
  }

  if (action == "button") {
    const std::string btn_name = require_field<std::string>(req, "button");
    const int btn = debug_input_common::name_to_mouse_button(btn_name);
    if (btn == 0) {
      return err_response("unknown mouse button '" + btn_name + "'");
    }
    const bool down = require_field<bool>(req, "down");
    const float x = optional_field<double>(req, "x", -1.0);
    const float y = optional_field<double>(req, "y", -1.0);
    // If the caller supplied coordinates, warp first so the mouse-button
    // handler (which reads Display's cached mouse_x_/y_) sees them.
    if (x >= 0.0f && y >= 0.0f && window != nullptr) {
      SDL_WarpMouseInWindow(window, x, y);
    }
    debug_input_common::push_mouse_button(window_id, btn, down, x, y);
    return ok_response_empty();
  }

  if (action == "wheel") {
    const float dy = require_field<double>(req, "dy");
    const float x = optional_field<double>(req, "x", -1.0);
    const float y = optional_field<double>(req, "y", -1.0);
    if (x >= 0.0f && y >= 0.0f && window != nullptr) {
      SDL_WarpMouseInWindow(window, x, y);
    }
    debug_input_common::push_mouse_wheel(window_id, dy, x, y);
    return ok_response_empty();
  }

  return err_response("unknown mouse action '" + action + "' (expected move|button|wheel)");
}

/** Pause the server thread (not the main loop) for a given number of seconds. */
json handle_sleep(const json& req) {
  const double seconds = require_field<double>(req, "seconds");
  debug_input_common::sleep_seconds(seconds);
  return ok_response_empty();
}

/** Resolve a single query field to a JSON value. */
json query_field(const std::string& field, const Session& session) {
  if (session.video_compare == nullptr || session.display == nullptr) {
    throw std::runtime_error("session not initialized");
  }
  if (field == "play_state") {
    return Display::play_state_label(session.display->get_play_state());
  }
  if (field == "left_pts") {
    const PlaybackStateSnapshot snap = session.video_compare->get_playback_state_snapshot();
    return snap.left_pts_us / 1'000'000.0;
  }
  if (field == "right_pts") {
    const PlaybackStateSnapshot snap = session.video_compare->get_playback_state_snapshot();
    return snap.right_pts_us / 1'000'000.0;
  }
  if (field == "effective_time_shift") {
    const PlaybackStateSnapshot snap = session.video_compare->get_playback_state_snapshot();
    return snap.effective_time_shift_us / 1'000'000.0;
  }
  if (field == "left_raw_pts") {
    // Raw (pre-shift) presentation timestamp in the file's own axis.
    // For LEFT, left.pts_ IS the raw pts (no shift applied), so this just
    // exposes it directly. Useful for frame-exact integration test asserts.
    const PlaybackStateSnapshot snap = session.video_compare->get_playback_state_snapshot();
    return snap.left_pts_us / 1'000'000.0;
  }
  if (field == "right_raw_pts") {
    // Raw right-side PTS reconstructed from the common-time field. The app
    // stores right.pts_ = right_raw - effective_shift, so inverting recovers
    // the encoded file's actual PTS (the quantity tests want to assert on).
    const PlaybackStateSnapshot snap = session.video_compare->get_playback_state_snapshot();
    return (snap.right_pts_us + snap.effective_time_shift_us) / 1'000'000.0;
  }
  if (field == "frame_number") {
    // compare() loop iteration at last snapshot publish. Non-zero proves
    // the main loop produced at least one rendered frame (guards the
    // --start-paused "black screen at launch" regression).
    const PlaybackStateSnapshot snap = session.video_compare->get_playback_state_snapshot();
    return static_cast<int64_t>(snap.frame_number);
  }
  if (field == "initialized") {
    // True once compare() has published real state (frames decoded, pts
    // known, etc.). False during the startup window before the first
    // iteration completes.
    const PlaybackStateSnapshot snap = session.video_compare->get_playback_state_snapshot();
    return snap.initialized;
  }
  if (field == "left_decoded_picture_number") {
    const PlaybackStateSnapshot snap = session.video_compare->get_playback_state_snapshot();
    return static_cast<int64_t>(snap.left_decoded_picture_number);
  }
  if (field == "right_decoded_picture_number") {
    const PlaybackStateSnapshot snap = session.video_compare->get_playback_state_snapshot();
    return static_cast<int64_t>(snap.right_decoded_picture_number);
  }
  if (field == "left_path") {
    return session.video_compare->get_left_path();
  }
  if (field == "right_path") {
    return session.video_compare->get_active_right_path();
  }
  if (field == "window_size") {
    return json{{"w", session.display->get_window_width()}, {"h", session.display->get_window_height()}};
  }
  if (field == "drawable_size") {
    return json{{"w", session.display->get_drawable_width()}, {"h", session.display->get_drawable_height()}};
  }
  if (field == "swap") {
    return session.display->get_swap_left_right();
  }
  if (field == "uptime") {
    return session.video_compare->get_uptime_seconds();
  }
  if (field == "script_running") {
    return debug_input_script::is_running();
  }
  throw std::runtime_error("unknown field '" + field + "'");
}

/** Handle a `get` request. */
json handle_get(const json& req, const Session& session) {
  const std::string field = require_field<std::string>(req, "field");
  try {
    return ok_response(query_field(field, session));
  } catch (const std::exception& e) {
    return err_response(e.what());
  }
}

/** Handle a `status` request: return a single object with every query field. */
json handle_status(const Session& session) {
  static constexpr const char* kAllFields[] = {
      "play_state",    "left_pts",      "right_pts",    "effective_time_shift",
      "left_raw_pts",  "right_raw_pts", "frame_number", "initialized",
      "left_decoded_picture_number", "right_decoded_picture_number",
      "left_path",     "right_path",    "window_size",  "drawable_size",
      "swap",          "uptime",        "script_running",
  };
  json result = json::object();
  for (const char* field : kAllFields) {
    try {
      result[field] = query_field(field, session);
    } catch (const std::exception& e) {
      result[field] = nullptr;
      result["_error_" + std::string(field)] = e.what();
    }
  }
  return ok_response(std::move(result));
}

/** Dispatch one parsed request to the appropriate handler. */
json dispatch(const json& req, const Session& session) {
  std::string cmd;
  try {
    cmd = require_field<std::string>(req, "cmd");
  } catch (const std::exception& e) {
    return err_response(e.what());
  }
  try {
    if (cmd == "key") {
      return handle_key(req);
    }
    if (cmd == "mouse") {
      return handle_mouse(req, session);
    }
    if (cmd == "sleep") {
      return handle_sleep(req);
    }
    if (cmd == "quit") {
      debug_input_common::push_quit();
      return ok_response_empty();
    }
    if (cmd == "get") {
      return handle_get(req, session);
    }
    if (cmd == "status") {
      return handle_status(session);
    }
    if (cmd == "ping") {
      return ok_response(std::string("pong"));
    }
    return err_response("unknown command '" + cmd + "'");
  } catch (const std::exception& e) {
    return err_response(e.what());
  }
}

/**
 * Serve a single connected client: read request lines, parse, dispatch,
 * write response lines, until the client disconnects.
 */
void serve_client(int conn_fd, const Session& session) {
  std::string line;
  while (read_line(conn_fd, line)) {
    if (line.empty()) {
      continue;
    }
    log_verbose("request: " + line);
    json response;
    try {
      const json req = json::parse(line);
      response = dispatch(req, session);
    } catch (const json::parse_error& e) {
      response = err_response(std::string("json parse error: ") + e.what());
    } catch (const std::exception& e) {
      response = err_response(e.what());
    }
    const std::string out = response.dump() + "\n";
    log_verbose("response: " + response.dump());
    if (!send_all(conn_fd, out)) {
      break;
    }
  }
}

/** Outer server loop: accept one connection at a time, serve, repeat. */
void server_loop(int listen_fd, Session session) {
  while (true) {
    const int conn_fd = ::accept(listen_fd, nullptr, nullptr);
    if (conn_fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      log_always(std::string("accept() failed: ") + std::strerror(errno));
      break;
    }
    log_verbose("client connected");
    serve_client(conn_fd, session);
    ::close(conn_fd);
    log_verbose("client disconnected");
  }
  ::close(listen_fd);
}

}  // namespace

void start_from_env(const Session& session) {
  const char* path = std::getenv("VIDEO_COMPARE_INPUT_SOCK");
  if (path == nullptr || path[0] == '\0') {
    return;
  }
  g_verbose_logging = std::getenv("VIDEO_COMPARE_INPUT_SOCK_LOG") != nullptr;

  // Abstract namespace (leading '\0') and oversized paths aren't supported here.
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const size_t max_path = sizeof(addr.sun_path) - 1;
  if (std::strlen(path) > max_path) {
    log_always(std::string("socket path too long (max ") + std::to_string(max_path) + " chars): " + path);
    return;
  }
  std::strncpy(addr.sun_path, path, max_path);

  const int listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    log_always(std::string("socket() failed: ") + std::strerror(errno));
    return;
  }

  // Stale socket file from a previous crashed run will cause bind() to fail
  // with EADDRINUSE; silently clear it so the common case just works.
  ::unlink(path);

  if (::bind(listen_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
    log_always(std::string("bind('") + path + "') failed: " + std::strerror(errno));
    ::close(listen_fd);
    return;
  }
  if (::listen(listen_fd, 1) < 0) {
    log_always(std::string("listen() failed: ") + std::strerror(errno));
    ::close(listen_fd);
    ::unlink(path);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(g_socket_path_mutex);
    g_socket_path = path;
  }
  // Best-effort cleanup at exit. The process may be killed uncleanly (e.g.
  // SIGKILL), in which case the next run's bind() will still clear the
  // stale file above.
  static std::once_flag atexit_flag;
  std::call_once(atexit_flag, [] { std::atexit(unlink_socket_path); });

  log_always(std::string("listening on ") + path);

  std::thread([listen_fd, session]() { server_loop(listen_fd, session); }).detach();
}

}  // namespace debug_input_socket

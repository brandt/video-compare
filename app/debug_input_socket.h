#pragma once

// Forward declarations keep this header cheap to include.
class VideoCompare;
class Display;

/**
 * Interactive debug-input server backed by a Unix domain socket.
 *
 * When the environment variable VIDEO_COMPARE_INPUT_SOCK is set to a path,
 * start_from_env() creates a listening AF_UNIX socket at that path and spawns
 * a detached worker thread that accepts one client at a time. The protocol
 * is line-oriented JSON (one request object per line, one response object
 * per line) -- see docs/design/input-socket.md for the command reference.
 *
 * Unlike the scripted harness (debug_input_script), this server supports
 * mouse events and state queries, so test drivers can branch on observed
 * playback state rather than over-estimating sleeps. The two systems coexist
 * and compose.
 *
 * Setting VIDEO_COMPARE_INPUT_SOCK_LOG=1 echoes each request and response to
 * stderr with an [input-socket] prefix, useful when debugging test drivers.
 */
namespace debug_input_socket {

/**
 * Read-only handles the server needs to execute commands. The pointers are
 * captured at start-up and must outlive the server thread (they refer to
 * objects owned by the main thread's VideoCompare instance).
 */
struct Session {
  const VideoCompare* video_compare{nullptr};
  Display* display{nullptr};
};

/**
 * Honor VIDEO_COMPARE_INPUT_SOCK: create the socket, bind/listen, and spawn
 * a detached worker thread. No-op if the env var is unset or empty. On
 * failure (bind fails, path already in use, etc.) logs to stderr and
 * returns without throwing -- the rest of the session runs normally.
 */
void start_from_env(const Session& session);

}  // namespace debug_input_socket

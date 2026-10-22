/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_HOST_IPC_H_
#define XENIA_UI_HOST_IPC_H_

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

namespace xe {
namespace ui {

struct RawImage;

// The channel to the program hosting the emulator (xbox-dash, see
// --host_ipc), which draws the system UI games ask for (keyboard, sign-in,
// message boxes...) in place of Xenia's own dialogs.
//
// One JSON object per line over a Unix socket, both ways:
//  - requests from Xenia: {"id": 1, "type": "keyboard", ...fields}, answered
//    by the host with {"reply_to": 1, ...fields};
//  - events from Xenia (no reply): {"type": "...", ...fields}.
class HostIpc {
 public:
  using Value = std::variant<bool, int64_t, std::string>;
  using Message = std::map<std::string, Value>;

  // The connection to the host, or null when not running hosted (no
  // --host_ipc) or the host can't be reached. Connects on the first call.
  static HostIpc* Get();

  // Sends a request and waits for the host's reply. Empty if the host went
  // away first. While any request is waiting, host UI is active: the host
  // draws over where the game is (see SetHostUIListener).
  std::optional<Message> Request(std::string_view type, Message fields);
  void Send(std::string_view type, Message fields);

  // Saves a frame of the game for the host to draw its UI over (the game is
  // hidden meanwhile) as a PNG next to the socket, and returns its path, or
  // empty on failure. The host deletes it once read.
  std::string SaveFrame(const RawImage& image);

  bool host_ui_active() const;
  // Called (from the thread that changed it) when host UI becomes active or
  // inactive. The embedded window hides itself meanwhile.
  void SetHostUIListener(std::function<void(bool active)> listener);

  static bool GetBool(const Message& message, const std::string& key);
  static std::string GetString(const Message& message, const std::string& key);

 private:
  explicit HostIpc(int socket);
  ~HostIpc() = delete;

  bool Write(std::string_view type, std::optional<uint64_t> id,
             const Message& fields);
  void ReadLoop();
  void OnLine(std::string_view line);
  void SetHostUIActive(bool active);

  int socket_;
  std::thread reader_;
  std::mutex write_mutex_;

  mutable std::mutex mutex_;
  std::condition_variable replied_;
  bool closed_ = false;
  uint64_t next_id_ = 1;
  uint64_t next_frame_ = 1;
  std::map<uint64_t, std::optional<Message>> pending_;
  std::function<void(bool active)> host_ui_listener_;
};

}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_HOST_IPC_H_

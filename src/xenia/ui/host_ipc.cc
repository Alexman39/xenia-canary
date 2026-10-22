/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/host_ipc.h"

#include <cstring>
#include <fstream>

#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/stringbuffer.h"
#include "third_party/rapidjson/include/rapidjson/writer.h"
#include "third_party/stb/stb_image_write.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/ui/presenter.h"

#if XE_PLATFORM_LINUX
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

DEFINE_transient_string(
    host_ipc, "",
    "Unix socket of the program hosting the emulator (xbox-dash), which then "
    "draws the system UI that games ask for.",
    "UI");

namespace xe {
namespace ui {

HostIpc* HostIpc::Get() {
  // Leaked on purpose: the reader thread may be blocked in a read until exit.
  static HostIpc* instance = []() -> HostIpc* {
    if (cvars::host_ipc.empty()) {
      return nullptr;
    }
#if XE_PLATFORM_LINUX
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    if (cvars::host_ipc.size() >= sizeof(address.sun_path)) {
      XELOGE("HostIpc: Socket path too long: {}", cvars::host_ipc);
      return nullptr;
    }
    std::strcpy(address.sun_path, cvars::host_ipc.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 ||
        connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))) {
      XELOGE("HostIpc: Can't connect to {}: {}", cvars::host_ipc,
             std::strerror(errno));
      if (fd >= 0) {
        close(fd);
      }
      return nullptr;
    }
    XELOGI("HostIpc: Connected to {}", cvars::host_ipc);
    return new HostIpc(fd);
#else
    XELOGE("HostIpc: Not supported on this platform");
    return nullptr;
#endif
  }();
  return instance;
}

HostIpc::HostIpc(int socket) : socket_(socket) {
  reader_ = std::thread([this]() { ReadLoop(); });
  reader_.detach();
}

std::optional<HostIpc::Message> HostIpc::Request(std::string_view type,
                                                 Message fields) {
  uint64_t id;
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return std::nullopt;
    }
    id = next_id_++;
    pending_.emplace(id, std::nullopt);
  }

  // A reply arriving before the wait starts is fine: it's in pending_.
  bool written = Write(type, id, fields);
  std::optional<Message> reply;
  bool hide_ended = false;
  {
    std::unique_lock lock(mutex_);
    if (written) {
      replied_.wait(lock, [this, id]() { return closed_ || pending_.at(id); });
    }
    reply = std::move(pending_.at(id));
    pending_.erase(id);
    if (pending_.empty() && host_ui_shown_) {
      host_ui_shown_ = false;
      hide_ended = true;
    }
  }
  if (hide_ended) {
    SetHostUIActive(false);
  }
  return reply;
}

void HostIpc::Send(std::string_view type, Message fields) {
  Write(type, std::nullopt, fields);
}

std::string HostIpc::SaveFrame(const RawImage& image) {
  if (!image.width || !image.height) {
    return {};
  }
  std::string path;
  {
    std::lock_guard lock(mutex_);
    path = fmt::format("{}.frame{}.png", cvars::host_ipc, next_frame_++);
  }
  // The capture's fourth channel is unused (X8): make it opaque.
  std::vector<uint8_t> pixels(image.data);
  for (uint32_t y = 0; y < image.height; ++y) {
    uint8_t* row = pixels.data() + y * image.stride;
    for (uint32_t x = 0; x < image.width; ++x) {
      row[x * 4 + 3] = 0xFF;
    }
  }
  std::ofstream file(path, std::ios::binary);
  if (!file || !stbi_write_png_to_func(
                   [](void* context, void* data, int size) {
                     static_cast<std::ofstream*>(context)->write(
                         static_cast<const char*>(data), size);
                   },
                   &file, int(image.width), int(image.height), 4, pixels.data(),
                   int(image.stride))) {
    XELOGE("HostIpc: Can't save a frame to {}", path);
    return {};
  }
  return path;
}

bool HostIpc::host_ui_active() const {
  std::lock_guard lock(mutex_);
  return host_ui_shown_;
}

void HostIpc::SetHostUIListener(std::function<void(bool active)> listener) {
  std::lock_guard lock(mutex_);
  host_ui_listener_ = std::move(listener);
}

void HostIpc::SetHostUIActive(bool active) {
  std::function<void(bool)> listener;
  {
    std::lock_guard lock(mutex_);
    listener = host_ui_listener_;
  }
  if (listener) {
    listener(active);
  }
}

bool HostIpc::GetBool(const Message& message, const std::string& key) {
  auto it = message.find(key);
  return it != message.end() && std::holds_alternative<bool>(it->second) &&
         std::get<bool>(it->second);
}

std::string HostIpc::GetString(const Message& message, const std::string& key) {
  auto it = message.find(key);
  if (it == message.end() || !std::holds_alternative<std::string>(it->second)) {
    return {};
  }
  return std::get<std::string>(it->second);
}

bool HostIpc::Write(std::string_view type, std::optional<uint64_t> id,
                    const Message& fields) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.StartObject();
  if (id) {
    writer.Key("id");
    writer.Uint64(*id);
  }
  writer.Key("type");
  writer.String(type.data(), rapidjson::SizeType(type.size()));
  for (const auto& [key, value] : fields) {
    writer.Key(key.c_str(), rapidjson::SizeType(key.size()));
    if (std::holds_alternative<bool>(value)) {
      writer.Bool(std::get<bool>(value));
    } else if (std::holds_alternative<int64_t>(value)) {
      writer.Int64(std::get<int64_t>(value));
    } else {
      const auto& string = std::get<std::string>(value);
      writer.String(string.data(), rapidjson::SizeType(string.size()));
    }
  }
  writer.EndObject();
  // JSON strings escape newlines, so a newline only ever ends a message.
  std::string line(buffer.GetString(), buffer.GetSize());
  line += '\n';

#if XE_PLATFORM_LINUX
  std::lock_guard lock(write_mutex_);
  for (size_t written = 0; written < line.size();) {
    ssize_t n = ::send(socket_, line.data() + written, line.size() - written,
                       MSG_NOSIGNAL);
    if (n <= 0) {
      if (n < 0 && errno == EINTR) {
        continue;
      }
      XELOGE("HostIpc: Write failed: {}", std::strerror(errno));
      return false;
    }
    written += size_t(n);
  }
  return true;
#else
  return false;
#endif
}

void HostIpc::ReadLoop() {
#if XE_PLATFORM_LINUX
  std::string received;
  char chunk[4096];
  while (true) {
    ssize_t n = ::read(socket_, chunk, sizeof(chunk));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    received.append(chunk, size_t(n));
    size_t start = 0;
    for (size_t end; (end = received.find('\n', start)) != std::string::npos;
         start = end + 1) {
      OnLine(std::string_view(received).substr(start, end - start));
    }
    received.erase(0, start);
  }
#endif
  XELOGI("HostIpc: Host disconnected");
  std::lock_guard lock(mutex_);
  closed_ = true;
  replied_.notify_all();
}

void HostIpc::OnLine(std::string_view line) {
  rapidjson::Document document;
  document.Parse(line.data(), line.size());
  if (document.HasParseError() || !document.IsObject()) {
    XELOGE("HostIpc: Bad message: {}", line);
    return;
  }
  auto reply_to = document.FindMember("reply_to");
  if (reply_to == document.MemberEnd() || !reply_to->value.IsUint64()) {
    auto type = document.FindMember("type");
    if (type != document.MemberEnd() && type->value.IsString() &&
        std::string_view(type->value.GetString()) == "ui_shown") {
      // The host has drawn its UI for a request, over the frame it was
      // given: only now the game can hide without a glimpse of what was
      // under it.
      bool hide = false;
      {
        std::lock_guard lock(mutex_);
        if (!pending_.empty() && !host_ui_shown_) {
          host_ui_shown_ = true;
          hide = true;
        }
      }
      if (hide) {
        SetHostUIActive(true);
      }
    }
    return;
  }
  Message message;
  for (const auto& member : document.GetObject()) {
    std::string key(member.name.GetString(), member.name.GetStringLength());
    const auto& value = member.value;
    if (value.IsBool()) {
      message.emplace(key, value.GetBool());
    } else if (value.IsInt64()) {
      message.emplace(key, value.GetInt64());
    } else if (value.IsString()) {
      message.emplace(key,
                      std::string(value.GetString(), value.GetStringLength()));
    }
  }
  std::lock_guard lock(mutex_);
  auto pending = pending_.find(reply_to->value.GetUint64());
  if (pending != pending_.end()) {
    pending->second = std::move(message);
    replied_.notify_all();
  }
}

}  // namespace ui
}  // namespace xe

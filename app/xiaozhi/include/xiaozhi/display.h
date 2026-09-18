#pragma once

#include <memory>
#include <string>

namespace xiaozhi {

class Display {
 public:
  virtual ~Display() = default;
  virtual bool Start() = 0;
  virtual void Stop() = 0;
  virtual void SetStatus(const std::string &status) = 0;
  virtual void SetConnected(bool connected) = 0;
  virtual void SetChatMessage(const std::string &role,
                              const std::string &content) = 0;
  virtual void SetEmotion(const std::string &emotion) = 0;
  virtual void ShowNotification(const std::string &text) = 0;
};

std::unique_ptr<Display> CreateNuttxLvglDisplay(
    const std::string &framebuffer_path, const std::string &input_path);

}  // namespace xiaozhi

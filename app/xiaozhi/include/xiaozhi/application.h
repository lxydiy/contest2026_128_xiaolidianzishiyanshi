#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "xiaozhi/audio.h"
#include "xiaozhi/device_state_machine.h"
#include "xiaozhi/display.h"
#include "xiaozhi/protocol.h"

namespace xiaozhi {

struct ApplicationConfig {
  std::string websocket_url;
  std::string token;
  std::string capture_path;
  std::string playback_path;
  std::string client_id_path;
  std::string framebuffer_path;
  std::string input_path;
  int protocol_version{1};
};

class Application {
 public:
  explicit Application(ApplicationConfig config);
  ~Application();

  int Run();
  void RequestStop();

 private:
  using Task = std::function<void()>;

  void Schedule(Task task);
  void StartListening(ListeningMode mode);
  void StopListening();
  void AbortSpeaking(AbortReason reason);
  void HandleJson(const cJSON *root);
  void SetError(const std::string &message);

  ApplicationConfig config_;
  DeviceStateMachine state_;
  std::unique_ptr<AudioDevice> audio_;
  std::unique_ptr<Display> display_;
  std::unique_ptr<Protocol> protocol_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Task> tasks_;
  bool stop_requested_{false};
};

}  // namespace xiaozhi

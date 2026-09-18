#include "xiaozhi/display.h"

#include <nuttx/config.h>
#include <lvgl/lvgl.h>
#include <pthread.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

extern "C" {
#include "lv_100ask_xz_ai_main.h"
}

namespace xiaozhi {
namespace {

int CreateGuiThread(pthread_t *thread, void *(*entry)(void *),
                    void *argument) {
  pthread_attr_t attributes;
  int result = pthread_attr_init(&attributes);
  if (result != 0) {
    return result;
  }

  result = pthread_attr_setstacksize(
      &attributes, CONFIG_CONTEST2026_128_XIAOZHI_GUI_STACKSIZE);
  if (result == 0) {
    result = pthread_create(thread, &attributes, entry, argument);
  }
  pthread_attr_destroy(&attributes);
  return result;
}

class NuttxLvglDisplay final : public Display {
 public:
  NuttxLvglDisplay(std::string framebuffer_path, std::string input_path)
      : framebuffer_path_(std::move(framebuffer_path)),
        input_path_(std::move(input_path)) {}

  ~NuttxLvglDisplay() override { Stop(); }

  bool Start() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (thread_started_) {
        return initialized_ok_;
      }
      running_ = true;
      initialization_done_ = false;
      initialized_ok_ = false;
    }

    if (CreateGuiThread(&thread_, ThreadEntry, this) != 0) {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
      return false;
    }
    pthread_setname_np(thread_, "xz-gui");
    thread_started_ = true;

    std::unique_lock<std::mutex> lock(mutex_);
    initialized_.wait(lock, [this]() { return initialization_done_; });
    return initialized_ok_;
  }

  void Stop() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!thread_started_) {
        return;
      }
      running_ = false;
    }
    wake_.notify_all();
    pthread_join(thread_, nullptr);
    thread_started_ = false;
  }

  void SetStatus(const std::string &status) override {
    Post([status]() { SetStateString(status.c_str()); });
  }

  void SetConnected(bool connected) override {
    Post([connected]() { SetWifi(connected ? 1 : 0); });
  }

  void SetChatMessage(const std::string &role,
                      const std::string &content) override {
    Post([role, content]() {
      std::string message;
      if (role == "user") {
        message = "You: " + content;
      } else {
        message = content;
      }
      SetText(message.c_str());
    });
  }

  void SetEmotion(const std::string &emotion) override {
    Post([emotion]() { ::SetEmotion(emotion.c_str()); });
  }

  void ShowNotification(const std::string &text) override {
    Post([text]() { SetText(text.c_str()); });
  }

 private:
  using Task = std::function<void()>;

  static void *ThreadEntry(void *argument) {
    static_cast<NuttxLvglDisplay *>(argument)->Run();
    return nullptr;
  }

  void Post(Task task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) {
        return;
      }
      tasks_.push_back(std::move(task));
    }
    wake_.notify_one();
  }

  void Run() {
    lv_nuttx_dsc_t descriptor;
    lv_nuttx_result_t result {};

    if (lv_is_initialized()) {
      std::fprintf(stderr, "xiaozhi UI: LVGL is already initialized\n");
      FinishInitialization(false);
      return;
    }

    lv_init();
    lv_nuttx_dsc_init(&descriptor);
    descriptor.fb_path = framebuffer_path_.c_str();
    if (!input_path_.empty()) {
      descriptor.input_path = input_path_.c_str();
    }
    lv_nuttx_init(&descriptor, &result);
    if (result.disp == nullptr) {
      std::fprintf(stderr, "xiaozhi UI: display initialization failed\n");
      lv_deinit();
      FinishInitialization(false);
      return;
    }

    lv_100ask_xz_ai_main();
    FinishInitialization(true);

    while (true) {
      std::deque<Task> pending;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
          break;
        }
        pending.swap(tasks_);
      }
      for (auto &task : pending) {
        task();
      }

      uint32_t delay_ms = lv_timer_handler();
      if (delay_ms == 0) {
        delay_ms = 1;
      } else if (delay_ms > 50) {
        delay_ms = 50;
      }

      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait_for(lock, std::chrono::milliseconds(delay_ms), [this]() {
        return !running_ || !tasks_.empty();
      });
    }

    lv_100ask_xz_ai_deinit();
    lv_nuttx_deinit(&result);
    lv_deinit();
  }

  void FinishInitialization(bool success) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      initialized_ok_ = success;
      initialization_done_ = true;
      if (!success) {
        running_ = false;
      }
    }
    initialized_.notify_all();
  }

  std::string framebuffer_path_;
  std::string input_path_;
  pthread_t thread_ {};
  bool thread_started_{false};
  bool running_{false};
  bool initialization_done_{false};
  bool initialized_ok_{false};
  std::mutex mutex_;
  std::condition_variable initialized_;
  std::condition_variable wake_;
  std::deque<Task> tasks_;
};

}  // namespace

std::unique_ptr<Display> CreateNuttxLvglDisplay(
    const std::string &framebuffer_path, const std::string &input_path) {
  return std::make_unique<NuttxLvglDisplay>(framebuffer_path, input_path);
}

}  // namespace xiaozhi

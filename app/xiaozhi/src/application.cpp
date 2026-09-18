#include "xiaozhi/application.h"

#include <netutils/cJSON.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <utility>

#include "xiaozhi/identity.h"

namespace xiaozhi {

Application::Application(ApplicationConfig config) : config_(std::move(config)) {
  audio_ = CreateNuttxAudioDevice(config_.capture_path, config_.playback_path);
  display_ =
      CreateNuttxLvglDisplay(config_.framebuffer_path, config_.input_path);
  protocol_ = std::make_unique<Protocol>(CreateNuttxWebSocketTransport());
}

Application::~Application() {
  if (audio_) {
    audio_->Stop();
  }
  if (protocol_) {
    protocol_->Close();
  }
  if (display_) {
    display_->Stop();
  }
}

void Application::Schedule(Task task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tasks_.push_back(std::move(task));
  }
  wake_.notify_one();
}

void Application::SetError(const std::string &message) {
  std::fprintf(stderr, "xiaozhi: %s\n", message.c_str());
  state_.TransitionTo(DeviceState::kError);
  audio_->SetCaptureEnabled(false);
  display_->ShowNotification(message);
}

void Application::StartListening(ListeningMode mode) {
  if (state_.state() == DeviceState::kSpeaking) {
    AbortSpeaking(AbortReason::kNone);
  }
  if (!state_.TransitionTo(DeviceState::kListening)) {
    return;
  }
  audio_->ClearPlayback();
  audio_->SetCaptureEnabled(true);
  protocol_->SendStartListening(mode);
}

void Application::StopListening() {
  audio_->SetCaptureEnabled(false);
  protocol_->SendStopListening();
  state_.TransitionTo(DeviceState::kIdle);
}

void Application::AbortSpeaking(AbortReason reason) {
  protocol_->SendAbortSpeaking(reason);
  audio_->ClearPlayback();
  state_.TransitionTo(DeviceState::kIdle);
}

void Application::HandleJson(const cJSON *root) {
  const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
  if (!cJSON_IsString(type)) {
    return;
  }

  if (std::strcmp(type->valuestring, "tts") == 0) {
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (cJSON_IsString(text)) {
      std::printf("xiaozhi: assistant: %s\n", text->valuestring);
      display_->SetChatMessage("assistant", text->valuestring);
    }
    if (!cJSON_IsString(state)) {
      return;
    }
    if (std::strcmp(state->valuestring, "start") == 0) {
      audio_->SetCaptureEnabled(false);
      audio_->ClearPlayback();
      state_.TransitionTo(DeviceState::kSpeaking);
      display_->SetEmotion("happy");
    } else if (std::strcmp(state->valuestring, "stop") == 0) {
      state_.TransitionTo(DeviceState::kIdle);
      StartListening(ListeningMode::kAutoStop);
    }
    return;
  }

  if (std::strcmp(type->valuestring, "stt") == 0) {
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (cJSON_IsString(text)) {
      std::printf("xiaozhi: user: %s\n", text->valuestring);
      display_->SetChatMessage("user", text->valuestring);
    }
    return;
  }

  if (std::strcmp(type->valuestring, "llm") == 0) {
    const cJSON *emotion = cJSON_GetObjectItemCaseSensitive(root, "emotion");
    if (cJSON_IsString(emotion)) {
      std::printf("xiaozhi: emotion: %s\n", emotion->valuestring);
      display_->SetEmotion(emotion->valuestring);
    }
    return;
  }

  if (std::strcmp(type->valuestring, "mcp") == 0) {
    const cJSON *payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    const cJSON *method = payload == nullptr
                              ? nullptr
                              : cJSON_GetObjectItemCaseSensitive(payload,
                                                                 "method");
    const cJSON *id = payload == nullptr
                          ? nullptr
                          : cJSON_GetObjectItemCaseSensitive(payload, "id");
    if (!cJSON_IsString(method) || id == nullptr) {
      return;
    }

    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "jsonrpc", "2.0");
    cJSON_AddItemToObject(reply, "id", cJSON_Duplicate(id, true));
    cJSON *result = cJSON_AddObjectToObject(reply, "result");
    if (std::strcmp(method->valuestring, "initialize") == 0) {
      cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
      cJSON_AddObjectToObject(result, "capabilities");
      cJSON *server = cJSON_AddObjectToObject(result, "serverInfo");
      cJSON_AddStringToObject(server, "name", "openvela-xiaozhi");
      cJSON_AddStringToObject(server, "version", "0.1.0");
    } else if (std::strcmp(method->valuestring, "tools/list") == 0) {
      cJSON_AddArrayToObject(result, "tools");
    } else {
      cJSON_DeleteItemFromObject(reply, "result");
      cJSON *error = cJSON_AddObjectToObject(reply, "error");
      cJSON_AddNumberToObject(error, "code", -32601);
      cJSON_AddStringToObject(error, "message", "Method not found");
    }

    char *text = cJSON_PrintUnformatted(reply);
    cJSON_Delete(reply);
    if (text != nullptr) {
      protocol_->SendMcpMessage(text);
      cJSON_free(text);
    }
  }
}

int Application::Run() {
  if (!display_->Start()) {
    std::fprintf(stderr, "xiaozhi: failed to start LVGL display\n");
    return 1;
  }

  state_.SetListener([this](DeviceState, DeviceState next) {
    std::printf("xiaozhi: status=%s\n", DeviceStateName(next));
    display_->SetStatus(DeviceStateName(next));
  });

  protocol_->SetCallbacks(
      [this](AudioPacket packet) {
        audio_->QueuePlayback(std::move(packet.payload));
      },
      [this](const cJSON *root) {
        cJSON *copy = cJSON_Duplicate(root, true);
        Schedule([this, copy]() {
          HandleJson(copy);
          cJSON_Delete(copy);
        });
      },
      [this]() {
        const int rate = protocol_->server_sample_rate();
        Schedule([this, rate]() {
          display_->SetConnected(true);
          audio_->SetOutputSampleRate(rate);
          state_.TransitionTo(DeviceState::kIdle);
          StartListening(ListeningMode::kAutoStop);
        });
      },
      [this]() {
        Schedule([this]() {
          display_->SetConnected(false);
          SetError("WebSocket connection closed");
        });
      },
      [this](const std::string &message) {
        Schedule([this, message]() { SetError(message); });
      });

  if (!audio_->Start(
          [this](std::vector<uint8_t> payload, uint32_t timestamp) {
            if (state_.state() != DeviceState::kListening) {
              return;
            }
            AudioPacket packet;
            packet.sample_rate = 16000;
            packet.frame_duration_ms = 60;
            packet.timestamp = timestamp;
            packet.payload = std::move(payload);
            protocol_->SendAudio(std::move(packet));
          },
          [this](const std::string &message) {
            Schedule([this, message]() { SetError(message); });
          })) {
    SetError("failed to start NuttX audio devices");
    return 1;
  }

  state_.TransitionTo(DeviceState::kConnecting);
  const std::string device_id = GetDeviceId();
  const std::string client_id =
      LoadOrCreateClientId(config_.client_id_path.c_str());
  if (!protocol_->Open(config_.websocket_url, config_.token, device_id,
                       client_id, config_.protocol_version)) {
    SetError("failed to start WebSocket transport");
    return 1;
  }

  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_requested_) {
    wake_.wait_for(lock, std::chrono::seconds(1),
                   [this]() { return stop_requested_ || !tasks_.empty(); });
    while (!tasks_.empty()) {
      Task task = std::move(tasks_.front());
      tasks_.pop_front();
      lock.unlock();
      task();
      lock.lock();
    }
  }

  lock.unlock();
  audio_->SetCaptureEnabled(false);
  protocol_->Close();
  audio_->Stop();
  state_.TransitionTo(DeviceState::kStopped);
  display_->Stop();
  return 0;
}

void Application::RequestStop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = true;
  }
  wake_.notify_one();
}

}  // namespace xiaozhi

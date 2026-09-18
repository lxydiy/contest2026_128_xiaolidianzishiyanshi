#include "xiaozhi/audio.h"

#include <nuttx/audio/audio.h>
#include <nuttx/config.h>
#include <opus.h>

#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <climits>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace xiaozhi {
namespace {

constexpr int kCaptureSampleRate = 16000;
constexpr int kFrameDurationMs = 60;
constexpr int kCaptureSamples =
    kCaptureSampleRate * kFrameDurationMs / 1000;
constexpr int kBitsPerSample = 16;
constexpr int kChannels = 1;
constexpr int kFallbackBufferCount = 4;
constexpr int kFallbackBufferSize =
    kCaptureSamples * sizeof(int16_t);

int CreateAudioThread(pthread_t *thread, void *(*entry)(void *),
                      void *argument) {
  pthread_attr_t attributes;
  int result = pthread_attr_init(&attributes);
  if (result != 0) {
    return result;
  }

  result = pthread_attr_setstacksize(
      &attributes, CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_STACKSIZE);
  if (result == 0) {
    result = pthread_create(thread, &attributes, entry, argument);
  }
  pthread_attr_destroy(&attributes);
  return result;
}

uint32_t MonotonicMilliseconds() {
  struct timespec now {};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint32_t>(now.tv_sec * 1000ULL + now.tv_nsec / 1000000);
}

struct AudioEndpoint {
  int fd{-1};
  mqd_t mq{static_cast<mqd_t>(-1)};
  std::string mq_name;
  std::vector<ap_buffer_s *> buffers;
#ifdef CONFIG_AUDIO_MULTI_SESSION
  void *session{nullptr};
#endif
};

unsigned long SessionArgument(AudioEndpoint &endpoint) {
#ifdef CONFIG_AUDIO_MULTI_SESSION
  return reinterpret_cast<unsigned long>(endpoint.session);
#else
  (void)endpoint;
  return 0;
#endif
}

void SetDescriptorSession(audio_buf_desc_s &descriptor,
                          AudioEndpoint &endpoint) {
#ifdef CONFIG_AUDIO_MULTI_SESSION
  descriptor.session = endpoint.session;
#else
  (void)descriptor;
  (void)endpoint;
#endif
}

void SetCapabilitiesSession(audio_caps_desc_s &descriptor,
                            AudioEndpoint &endpoint) {
#ifdef CONFIG_AUDIO_MULTI_SESSION
  descriptor.session = endpoint.session;
#else
  (void)descriptor;
  (void)endpoint;
#endif
}

int ConfigureEndpoint(AudioEndpoint &endpoint, const std::string &path,
                      uint8_t type, int sample_rate, const char *queue_suffix) {
  endpoint.fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (endpoint.fd < 0) {
    return -errno;
  }

#ifdef CONFIG_AUDIO_MULTI_SESSION
  if (ioctl(endpoint.fd, AUDIOIOC_RESERVE,
            reinterpret_cast<unsigned long>(&endpoint.session)) < 0) {
#else
  if (ioctl(endpoint.fd, AUDIOIOC_RESERVE, 0) < 0) {
#endif
    const int result = -errno;
    close(endpoint.fd);
    endpoint.fd = -1;
    return result;
  }

  struct audio_caps_desc_s capabilities {};
  SetCapabilitiesSession(capabilities, endpoint);
  capabilities.caps.ac_len = sizeof(audio_caps_s);
  capabilities.caps.ac_type = type;
  capabilities.caps.ac_subtype = AUDIO_FMT_PCM;
  capabilities.caps.ac_channels = kChannels;
  capabilities.caps.ac_chmap = 1;
  capabilities.caps.ac_controls.hw[0] = sample_rate;
  capabilities.caps.ac_controls.b[3] = sample_rate >> 16;
  capabilities.caps.ac_controls.b[2] = kBitsPerSample;
  if (ioctl(endpoint.fd, AUDIOIOC_CONFIGURE,
            reinterpret_cast<unsigned long>(&capabilities)) < 0) {
    return -errno;
  }

  struct ap_buffer_info_s info {};
  if (ioctl(endpoint.fd, AUDIOIOC_GETBUFFERINFO,
            reinterpret_cast<unsigned long>(&info)) < 0 ||
      info.nbuffers == 0 || info.buffer_size == 0) {
    info.nbuffers = kFallbackBufferCount;
    info.buffer_size = kFallbackBufferSize;
  }

  char name[32];
  std::snprintf(name, sizeof(name), "/xz-%lx-%s",
                static_cast<unsigned long>(reinterpret_cast<uintptr_t>(
                    &endpoint)),
                queue_suffix);
  endpoint.mq_name = name;
  struct mq_attr attributes {};
  attributes.mq_maxmsg = info.nbuffers + 4;
  attributes.mq_msgsize = sizeof(audio_msg_s);
  endpoint.mq = mq_open(endpoint.mq_name.c_str(),
                        O_RDWR | O_CREAT | O_NONBLOCK, 0644, &attributes);
  if (endpoint.mq == static_cast<mqd_t>(-1)) {
    return -errno;
  }
  if (ioctl(endpoint.fd, AUDIOIOC_REGISTERMQ,
            static_cast<unsigned long>(endpoint.mq)) < 0) {
    return -errno;
  }

  endpoint.buffers.resize(info.nbuffers, nullptr);
  for (auto &buffer : endpoint.buffers) {
    struct audio_buf_desc_s descriptor {};
    SetDescriptorSession(descriptor, endpoint);
    descriptor.numbytes = info.buffer_size;
    descriptor.u.pbuffer = &buffer;
    const int result = ioctl(endpoint.fd, AUDIOIOC_ALLOCBUFFER,
                             reinterpret_cast<unsigned long>(&descriptor));
    if (result != sizeof(descriptor)) {
      return result < 0 ? -errno : -ENOMEM;
    }
  }
  return 0;
}

int Enqueue(AudioEndpoint &endpoint, ap_buffer_s *buffer, bool capture) {
  struct audio_buf_desc_s descriptor {};
  SetDescriptorSession(descriptor, endpoint);
  buffer->curbyte = 0;
  buffer->flags = 0;
  if (capture) {
    buffer->nbytes = buffer->nmaxbytes;
  }
  descriptor.numbytes = buffer->nbytes;
  descriptor.u.buffer = buffer;
  if (ioctl(endpoint.fd, AUDIOIOC_ENQUEUEBUFFER,
            reinterpret_cast<unsigned long>(&descriptor)) < 0) {
    return -errno;
  }
  return 0;
}

void WakeEndpoint(AudioEndpoint &endpoint) {
  if (endpoint.mq == static_cast<mqd_t>(-1)) {
    return;
  }
  struct audio_msg_s message {};
  message.msg_id = AUDIO_MSG_STOP;
  mq_send(endpoint.mq, reinterpret_cast<const char *>(&message),
          sizeof(message), 0);
}

void CloseEndpoint(AudioEndpoint &endpoint) {
  if (endpoint.fd < 0) {
    return;
  }
  ioctl(endpoint.fd, AUDIOIOC_STOP, SessionArgument(endpoint));
  if (endpoint.mq != static_cast<mqd_t>(-1)) {
    ioctl(endpoint.fd, AUDIOIOC_UNREGISTERMQ,
          static_cast<unsigned long>(endpoint.mq));
  }
  for (auto *buffer : endpoint.buffers) {
    if (buffer == nullptr) {
      continue;
    }
    struct audio_buf_desc_s descriptor {};
    SetDescriptorSession(descriptor, endpoint);
    descriptor.u.buffer = buffer;
    ioctl(endpoint.fd, AUDIOIOC_FREEBUFFER,
          reinterpret_cast<unsigned long>(&descriptor));
  }
  endpoint.buffers.clear();
  ioctl(endpoint.fd, AUDIOIOC_RELEASE, SessionArgument(endpoint));
  close(endpoint.fd);
  endpoint.fd = -1;
  if (endpoint.mq != static_cast<mqd_t>(-1)) {
    mq_close(endpoint.mq);
    mq_unlink(endpoint.mq_name.c_str());
    endpoint.mq = static_cast<mqd_t>(-1);
  }
}

class NuttxAudioDevice final : public AudioDevice {
 public:
  NuttxAudioDevice(std::string capture_path, std::string playback_path)
      : capture_path_(std::move(capture_path)),
        playback_path_(std::move(playback_path)) {}

  ~NuttxAudioDevice() override { Stop(); }

  bool Start(EncodedCallback encoded, ErrorCallback error) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (running_) {
        return true;
      }
      encoded_callback_ = std::move(encoded);
      error_callback_ = std::move(error);
      running_ = true;
    }
    if (CreateAudioThread(&capture_thread_, CaptureEntry, this) != 0) {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
      return false;
    }
    pthread_setname_np(capture_thread_, "xz-capture");
    capture_started_ = true;
    if (CreateAudioThread(&playback_thread_, PlaybackEntry, this) != 0) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
      }
      WakeEndpoint(capture_);
      pthread_join(capture_thread_, nullptr);
      capture_started_ = false;
      return false;
    }
    pthread_setname_np(playback_thread_, "xz-playback");
    playback_started_ = true;
    return true;
  }

  void Stop() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_ && !capture_started_ && !playback_started_) {
        return;
      }
      running_ = false;
      capture_enabled_ = false;
      playback_queue_.clear();
    }
    playback_wake_.notify_all();
    WakeEndpoint(capture_);
    WakeEndpoint(playback_);
    if (capture_started_) {
      pthread_join(capture_thread_, nullptr);
      capture_started_ = false;
    }
    if (playback_started_) {
      pthread_join(playback_thread_, nullptr);
      playback_started_ = false;
    }
  }

  void SetCaptureEnabled(bool enabled) override {
    std::lock_guard<std::mutex> lock(mutex_);
    capture_enabled_ = enabled;
  }

  void SetOutputSampleRate(int sample_rate) override {
    if (sample_rate != 8000 && sample_rate != 12000 && sample_rate != 16000 &&
        sample_rate != 24000 && sample_rate != 48000) {
      ReportError("unsupported Opus output sample rate");
      return;
    }

    /* Opus can decode every supported stream rate to 16 kHz.  Keep the
     * physical ES8311 link at the capture rate because its ADC and DAC share
     * one codec clock; independently switching playback to (for example)
     * 24 kHz would also change the microphone sampling clock.
     */
  }

  void QueuePlayback(std::vector<uint8_t> opus) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_) {
        return;
      }
      if (playback_queue_.size() >= 32) {
        playback_queue_.pop_front();
      }
      playback_queue_.push_back(std::move(opus));
    }
    playback_wake_.notify_one();
  }

  void ClearPlayback() override {
    std::lock_guard<std::mutex> lock(mutex_);
    playback_queue_.clear();
  }

 private:
  static void *CaptureEntry(void *argument) {
    static_cast<NuttxAudioDevice *>(argument)->CaptureLoop();
    return nullptr;
  }

  static void *PlaybackEntry(void *argument) {
    static_cast<NuttxAudioDevice *>(argument)->PlaybackLoop();
    return nullptr;
  }

  bool Running() {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
  }

  void CaptureLoop() {
    int opus_error = OPUS_OK;
    OpusEncoder *encoder = opus_encoder_create(
        kCaptureSampleRate, kChannels, OPUS_APPLICATION_VOIP, &opus_error);
    if (encoder == nullptr || opus_error != OPUS_OK) {
      ReportError("opus_encoder_create failed");
      return;
    }
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));

    const int setup = ConfigureEndpoint(capture_, capture_path_,
                                        AUDIO_TYPE_INPUT, kCaptureSampleRate,
                                        "cap");
    if (setup < 0) {
      CloseEndpoint(capture_);
      opus_encoder_destroy(encoder);
      ReportError("cannot configure capture device " + capture_path_);
      return;
    }
    for (auto *buffer : capture_.buffers) {
      if (Enqueue(capture_, buffer, true) < 0) {
        CloseEndpoint(capture_);
        opus_encoder_destroy(encoder);
        ReportError("cannot enqueue capture buffer");
        return;
      }
    }
    if (ioctl(capture_.fd, AUDIOIOC_START, SessionArgument(capture_)) < 0) {
      CloseEndpoint(capture_);
      opus_encoder_destroy(encoder);
      ReportError("cannot start capture device");
      return;
    }

    std::vector<int16_t> pending;
    pending.reserve(kCaptureSamples * 2);
    std::vector<uint8_t> encoded(512);
#ifdef CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_DIAGNOSTICS
    uint32_t meter_start = MonotonicMilliseconds();
    uint64_t meter_sum = 0;
    size_t meter_samples = 0;
    size_t meter_frames = 0;
    size_t meter_bytes = 0;
    int32_t meter_peak = 0;
#endif
    while (Running()) {
      struct audio_msg_s message {};
      unsigned int priority = 0;
      const ssize_t received =
          mq_receive(capture_.mq, reinterpret_cast<char *>(&message),
                     sizeof(message), &priority);
      if (received < 0) {
        if (errno == EAGAIN) {
          usleep(5000);
          continue;
        }
        if (errno == EINTR) {
          continue;
        }
        break;
      }
      if (message.msg_id == AUDIO_MSG_STOP) {
        break;
      }
      if (message.msg_id != AUDIO_MSG_DEQUEUE || message.u.ptr == nullptr) {
        continue;
      }

      auto *buffer = static_cast<ap_buffer_s *>(message.u.ptr);
      bool enabled;
      EncodedCallback callback;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled = capture_enabled_;
        callback = encoded_callback_;
      }
      if (enabled) {
        const auto *samples = reinterpret_cast<const int16_t *>(buffer->samp);
        const size_t sample_count = buffer->nbytes / sizeof(int16_t);
#ifdef CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_DIAGNOSTICS
        for (size_t index = 0; index < sample_count; ++index) {
          const int32_t value = samples[index];
          const int32_t magnitude = value < 0 ? -value : value;
          if (magnitude > meter_peak) {
            meter_peak = magnitude;
          }
          meter_sum += magnitude;
        }
        meter_samples += sample_count;
#endif
        pending.insert(pending.end(), samples, samples + sample_count);
        while (pending.size() >= kCaptureSamples) {
          const int bytes = opus_encode(encoder, pending.data(),
                                        kCaptureSamples, encoded.data(),
                                        encoded.size());
          if (bytes > 0 && callback) {
#ifdef CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_DIAGNOSTICS
            ++meter_frames;
            meter_bytes += bytes;
#endif
            callback(std::vector<uint8_t>(encoded.begin(),
                                          encoded.begin() + bytes),
                     MonotonicMilliseconds());
          }
          pending.erase(pending.begin(), pending.begin() + kCaptureSamples);
        }
      } else {
        pending.clear();
      }
#ifdef CONFIG_CONTEST2026_128_XIAOZHI_AUDIO_DIAGNOSTICS
      const uint32_t meter_now = MonotonicMilliseconds();
      if (enabled && meter_now - meter_start >= 1000) {
        const unsigned long long mean = meter_samples == 0
                                            ? 0
                                            : meter_sum / meter_samples;
        std::printf("xiaozhi audio: samples=%zu peak=%ld mean=%llu "
                    "opus_frames=%zu opus_bytes=%zu\n",
                    meter_samples, static_cast<long>(meter_peak), mean,
                    meter_frames, meter_bytes);
        meter_start = meter_now;
        meter_sum = 0;
        meter_samples = 0;
        meter_frames = 0;
        meter_bytes = 0;
        meter_peak = 0;
      }
#endif
      if (Enqueue(capture_, buffer, true) < 0) {
        break;
      }
    }

    CloseEndpoint(capture_);
    opus_encoder_destroy(encoder);
  }

  void PlaybackLoop() {
    int configured_rate = 0;
    OpusDecoder *decoder = nullptr;
    std::deque<ap_buffer_s *> free_buffers;
    std::vector<int16_t> decoded(5760);
    bool endpoint_ready = false;

    while (Running()) {
      std::vector<uint8_t> packet;
      int wanted_rate;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        playback_wake_.wait_for(lock, std::chrono::milliseconds(10), [this]() {
          return !running_ || !playback_queue_.empty();
        });
        if (!running_) {
          break;
        }
        wanted_rate = output_sample_rate_;
        if (!playback_queue_.empty()) {
          packet = std::move(playback_queue_.front());
          playback_queue_.pop_front();
        }
      }

      if (!endpoint_ready || wanted_rate != configured_rate) {
        if (endpoint_ready) {
          CloseEndpoint(playback_);
          free_buffers.clear();
          endpoint_ready = false;
        }
        if (decoder != nullptr) {
          opus_decoder_destroy(decoder);
          decoder = nullptr;
        }
        int opus_error = OPUS_OK;
        decoder = opus_decoder_create(wanted_rate, kChannels, &opus_error);
        if (decoder == nullptr || opus_error != OPUS_OK) {
          ReportError("opus_decoder_create failed");
          break;
        }
        if (ConfigureEndpoint(playback_, playback_path_, AUDIO_TYPE_OUTPUT,
                              wanted_rate, "play") < 0) {
          CloseEndpoint(playback_);
          ReportError("cannot configure playback device " + playback_path_);
          break;
        }
        for (auto *buffer : playback_.buffers) {
          free_buffers.push_back(buffer);
        }
        if (ioctl(playback_.fd, AUDIOIOC_START,
                  SessionArgument(playback_)) < 0) {
          ReportError("cannot start playback device");
          break;
        }
        configured_rate = wanted_rate;
        endpoint_ready = true;
      }

      struct audio_msg_s message {};
      unsigned int priority = 0;
      while (mq_receive(playback_.mq, reinterpret_cast<char *>(&message),
                        sizeof(message), &priority) == sizeof(message)) {
        if (message.msg_id == AUDIO_MSG_DEQUEUE && message.u.ptr != nullptr) {
          free_buffers.push_back(static_cast<ap_buffer_s *>(message.u.ptr));
        }
      }

      if (packet.empty()) {
        continue;
      }
      const int samples = opus_decode(decoder, packet.data(), packet.size(),
                                      decoded.data(), decoded.size(), 0);
      if (samples < 0) {
        ReportError("invalid Opus playback packet");
        continue;
      }

      const uint8_t *source = reinterpret_cast<const uint8_t *>(decoded.data());
      size_t remaining = samples * sizeof(int16_t);
      while (remaining > 0 && Running()) {
        if (free_buffers.empty()) {
          usleep(5000);
          while (mq_receive(playback_.mq, reinterpret_cast<char *>(&message),
                            sizeof(message), &priority) == sizeof(message)) {
            if (message.msg_id == AUDIO_MSG_DEQUEUE &&
                message.u.ptr != nullptr) {
              free_buffers.push_back(
                  static_cast<ap_buffer_s *>(message.u.ptr));
            }
          }
          continue;
        }
        ap_buffer_s *buffer = free_buffers.front();
        free_buffers.pop_front();
        const size_t bytes =
            remaining < buffer->nmaxbytes ? remaining : buffer->nmaxbytes;
        std::memcpy(buffer->samp, source, bytes);
        buffer->nbytes = bytes;
        buffer->curbyte = 0;
        if (Enqueue(playback_, buffer, false) < 0) {
          ReportError("cannot enqueue playback buffer");
          remaining = 0;
          break;
        }
        source += bytes;
        remaining -= bytes;
      }
    }

    if (endpoint_ready) {
      CloseEndpoint(playback_);
    }
    if (decoder != nullptr) {
      opus_decoder_destroy(decoder);
    }
  }

  void ReportError(const std::string &message) {
    ErrorCallback callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      callback = error_callback_;
    }
    if (callback) {
      callback(message);
    } else {
      std::fprintf(stderr, "xiaozhi audio: %s\n", message.c_str());
    }
  }

  std::string capture_path_;
  std::string playback_path_;
  std::mutex mutex_;
  std::condition_variable playback_wake_;
  bool running_{false};
  bool capture_enabled_{false};
  bool capture_started_{false};
  bool playback_started_{false};
  int output_sample_rate_{kCaptureSampleRate};
  pthread_t capture_thread_{};
  pthread_t playback_thread_{};
  AudioEndpoint capture_;
  AudioEndpoint playback_;
  std::deque<std::vector<uint8_t>> playback_queue_;
  EncodedCallback encoded_callback_;
  ErrorCallback error_callback_;
};

}  // namespace

std::unique_ptr<AudioDevice> CreateNuttxAudioDevice(
    const std::string &capture_path, const std::string &playback_path) {
  return std::make_unique<NuttxAudioDevice>(capture_path, playback_path);
}

int RunNuttxAudioCaptureTest(const std::string &capture_path, int seconds) {
  AudioEndpoint capture;
  int result = ConfigureEndpoint(capture, capture_path, AUDIO_TYPE_INPUT,
                                 kCaptureSampleRate, "test");
  if (result < 0) {
    std::fprintf(stderr,
                 "xiaozhi audio test: configure %s failed: %d (%s)\n",
                 capture_path.c_str(), -result, std::strerror(-result));
    CloseEndpoint(capture);
    return result;
  }

  for (auto *buffer : capture.buffers) {
    result = Enqueue(capture, buffer, true);
    if (result < 0) {
      std::fprintf(stderr,
                   "xiaozhi audio test: enqueue failed: %d (%s)\n",
                   -result, std::strerror(-result));
      CloseEndpoint(capture);
      return result;
    }
  }

  if (ioctl(capture.fd, AUDIOIOC_START, SessionArgument(capture)) < 0) {
    result = -errno;
    std::fprintf(stderr, "xiaozhi audio test: start failed: %d (%s)\n",
                 -result, std::strerror(-result));
    CloseEndpoint(capture);
    return result;
  }

  const uint32_t started = MonotonicMilliseconds();
  const uint32_t deadline = started + static_cast<uint32_t>(seconds * 1000);
  uint32_t next_report = started + 1000;
  int64_t sum = 0;
  size_t samples_seen = 0;
  int32_t minimum = INT16_MAX;
  int32_t maximum = INT16_MIN;
  int report_number = 1;

  while (true) {
    struct audio_msg_s message {};
    unsigned int priority = 0;
    const ssize_t received =
        mq_receive(capture.mq, reinterpret_cast<char *>(&message),
                   sizeof(message), &priority);
    if (received == sizeof(message)) {
      if (message.msg_id == AUDIO_MSG_STOP) {
        std::fprintf(stderr,
                     "xiaozhi audio test: driver stopped capture early\n");
        result = -EIO;
        break;
      }
      if (message.msg_id == AUDIO_MSG_DEQUEUE && message.u.ptr != nullptr) {
        auto *buffer = static_cast<ap_buffer_s *>(message.u.ptr);
        const auto *samples =
            reinterpret_cast<const int16_t *>(buffer->samp);
        const size_t count = buffer->nbytes / sizeof(int16_t);
        for (size_t index = 0; index < count; ++index) {
          const int32_t value = samples[index];
          if (value < minimum) {
            minimum = value;
          }
          if (value > maximum) {
            maximum = value;
          }
          sum += value;
        }
        samples_seen += count;
        result = Enqueue(capture, buffer, true);
        if (result < 0) {
          std::fprintf(stderr,
                       "xiaozhi audio test: re-enqueue failed: %d (%s)\n",
                       -result, std::strerror(-result));
          break;
        }
      }
    } else if (errno != EAGAIN && errno != EINTR) {
      result = -errno;
      std::fprintf(stderr,
                   "xiaozhi audio test: message receive failed: %d (%s)\n",
                   -result, std::strerror(-result));
      break;
    }

    const uint32_t now = MonotonicMilliseconds();
    if (static_cast<int32_t>(now - next_report) >= 0) {
      const long long average =
          samples_seen == 0 ? 0 : sum / static_cast<int64_t>(samples_seen);
      std::printf("xiaozhi audio: second=%d samples=%zu min=%ld max=%ld "
                  "avg=%lld\n",
                  report_number++, samples_seen,
                  static_cast<long>(samples_seen == 0 ? 0 : minimum),
                  static_cast<long>(samples_seen == 0 ? 0 : maximum), average);
      next_report += 1000;
      sum = 0;
      samples_seen = 0;
      minimum = INT16_MAX;
      maximum = INT16_MIN;
    }

    if (static_cast<int32_t>(now - deadline) >= 0) {
      break;
    }

    if (received < 0) {
      usleep(5000);
    }
  }

  std::printf("xiaozhi audio test: stopping capture\n");
  CloseEndpoint(capture);
  std::printf("xiaozhi audio test: capture stopped\n");
  return result < 0 ? result : 0;
}

}  // namespace xiaozhi

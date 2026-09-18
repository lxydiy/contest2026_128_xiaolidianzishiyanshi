#include <nuttx/config.h>

#include <unistd.h>

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "xiaozhi/application.h"
#include "xiaozhi/audio.h"

namespace {

int RunAudioLoopback(int argc, char *argv[]) {
  long seconds = 10;
  if (argc > 2) {
    char *end = nullptr;
    seconds = std::strtol(argv[2], &end, 10);
    if (end == nullptr || *end != '\0' || seconds < 1 || seconds > 300) {
      std::fprintf(stderr,
                   "usage: xiaozhi --audio-loopback [seconds: 1..300]\n");
      return 1;
    }
  }

  std::printf("xiaozhi: audio capture test running for %ld seconds "
              "(playback disabled)\n",
              seconds);
  const int result = xiaozhi::RunNuttxAudioCaptureTest(
      CONFIG_CONTEST2026_128_XIAOZHI_CAPTURE_DEVPATH,
      static_cast<int>(seconds));
  std::printf("xiaozhi: audio capture test %s\n",
              result == 0 ? "complete" : "failed");
  return result == 0 ? 0 : 1;
}

}  // namespace

extern "C" int main(int argc, char *argv[]) {
  if (argc > 1 && std::strcmp(argv[1], "--audio-loopback") == 0) {
    return RunAudioLoopback(argc, argv);
  }

  xiaozhi::ApplicationConfig config;
  config.websocket_url = CONFIG_CONTEST2026_128_XIAOZHI_WEBSOCKET_URL;
  config.token = CONFIG_CONTEST2026_128_XIAOZHI_TOKEN;
  config.capture_path = CONFIG_CONTEST2026_128_XIAOZHI_CAPTURE_DEVPATH;
  config.playback_path = CONFIG_CONTEST2026_128_XIAOZHI_PLAYBACK_DEVPATH;
  config.client_id_path = CONFIG_CONTEST2026_128_XIAOZHI_CLIENT_ID_PATH;
  config.framebuffer_path = CONFIG_CONTEST2026_128_XIAOZHI_FB_DEVPATH;
  config.input_path = CONFIG_CONTEST2026_128_XIAOZHI_INPUT_DEVPATH;
  config.protocol_version = CONFIG_CONTEST2026_128_XIAOZHI_PROTOCOL_VERSION;

  if (argc > 1) {
    config.websocket_url = argv[1];
  }
  if (argc > 2) {
    config.token = argv[2];
  }

  std::printf("xiaozhi: OpenVela/NuttX client starting\n");
  xiaozhi::Application application(std::move(config));
  return application.Run();
}

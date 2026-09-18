#pragma once

#include <string>

namespace xiaozhi {

std::string GetDeviceId();
std::string LoadOrCreateClientId(const char *path);

}  // namespace xiaozhi

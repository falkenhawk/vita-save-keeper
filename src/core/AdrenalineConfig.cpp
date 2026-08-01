#include "core/AdrenalineConfig.hpp"

#include <cstddef>
#include <cstdint>

namespace vsm {
namespace {

// user/main.h: int magic[2], then four ints, then ms_location - all little-endian.
constexpr std::uint32_t kAdrenalineConfigMagic1 = 0x31483943u;
constexpr std::uint32_t kAdrenalineConfigMagic2 = 0x334F4E33u;
constexpr std::size_t kMsLocationOffset = 24;

// Index order is the menu's option order and must not be rearranged.
constexpr const char *kMsLocations[] = {
    "ux0:pspemu", "ur0:pspemu", "imc0:pspemu", "xmc0:pspemu", "uma0:pspemu",
};

std::uint32_t read_u32le(const std::vector<unsigned char> &data, std::size_t offset) {
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24U);
}

} // namespace

std::string adrenaline_ms_location(const std::vector<unsigned char> &config_blob) {
  if (config_blob.size() < kMsLocationOffset + 4) {
    return {};
  }
  if (read_u32le(config_blob, 0) != kAdrenalineConfigMagic1 ||
      read_u32le(config_blob, 4) != kAdrenalineConfigMagic2) {
    return {};
  }
  const std::uint32_t index = read_u32le(config_blob, kMsLocationOffset);
  if (index >= sizeof(kMsLocations) / sizeof(kMsLocations[0])) {
    return {};
  }
  return kMsLocations[index];
}

} // namespace vsm

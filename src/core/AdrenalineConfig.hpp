#pragma once

#include <string>
#include <vector>

namespace vsm {

// The pspemu root Adrenaline is configured to mount as its Memory Stick ("uma0:pspemu"), read
// from the raw bytes of its settings file (ux0:app/PSPEMUCFW/adrenaline.bin - a plain
// AdrenalineConfig struct; layout and magic values from Adrenaline's user/main.h). Empty when
// the blob is truncated, carries the wrong magic, or indexes outside the known locations - the
// caller falls back to probing the filesystem instead of trusting a corrupt file.
std::string adrenaline_ms_location(const std::vector<unsigned char> &config_blob);

} // namespace vsm

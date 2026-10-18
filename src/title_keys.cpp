// Song (mogg) decryption keys.
//
// Encrypted moggs (versions 12-16) are keyed through XeKeysSetKey with one of four
// obscured 16-byte keys stored at kObscuredKeyTable. A real 360's hypervisor
// deobscures them with a console secret this project does not have, so the user
// supplies the deobscured table (--title_keys=<64-byte file>, never shipped) and it
// replaces the obscured one in guest memory before the game starts.

#include "title_keys.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_STRING(title_keys, "", "Blitz",
                      "64-byte file with the deobscured mogg key table (songs need it)");

namespace blitz {

namespace {
// Read by sub_823DE0C0 (index = mogg version 12-13/14/15/16 -> 0..3) and handed to
// the XeKeysSetKey wrapper sub_82727218. Fixed for the XEX this exe was generated from.
constexpr uint32_t kObscuredKeyTable = 0x8280C568;
constexpr size_t kKeyTableSize = 64;
}  // namespace

void ApplyTitleKeys(rex::memory::Memory* memory) {
  const std::string path = REXCVAR_GET(title_keys);
  if (path.empty()) {
    REXLOG_WARN("title_keys: not set; songs and music cannot be decrypted");
    return;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    REXLOG_ERROR("title_keys: cannot open {}", path);
    return;
  }
  std::vector<uint8_t> keys((std::istreambuf_iterator<char>(file)), {});
  if (keys.size() != kKeyTableSize) {
    REXLOG_ERROR("title_keys: {} must be exactly {} bytes (got {})", path, kKeyTableSize,
                 keys.size());
    return;
  }
  std::memcpy(memory->TranslateVirtual<uint8_t*>(kObscuredKeyTable), keys.data(), keys.size());
  REXLOG_INFO("title_keys: applied {}", path);
}

}  // namespace blitz

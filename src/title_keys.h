// Song (mogg) decryption keys: see title_keys.cpp.
#pragma once

namespace rex::memory {
class Memory;
}

namespace blitz {

// Replaces the obscured mogg key table with --title_keys, if set.
void ApplyTitleKeys(rex::memory::Memory* memory);

}  // namespace blitz

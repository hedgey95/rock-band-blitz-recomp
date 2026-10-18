// Custom songs: Blitz checks each DLC chart (songs/<name>/<name>.mid) against
// a table of SHA-1s for Harmonix's own songs. A fan-made chart is never in it,
// so for files on a custom-songs mount (the SDK's --custom_songs_dir) both
// checks pass; every other file is checked as before.
//   sub_82767868(file): looks the file's checksum up ("No checksum found")
//   sub_827678F0(file, sha1): compares it ("Checksum failure")
// The checked file's path is at +204: "<content root>:/songs/...".
#include <cstdlib>
#include <string_view>

#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

namespace {

bool FromCustomSong(uint32_t file, uint8_t* base) {
  const uint32_t name = _byteswap_ulong(*reinterpret_cast<uint32_t*>(base + file + 204));
  const std::string_view path(reinterpret_cast<const char*>(base + name));
  const size_t colon = path.find(':');
  return colon != std::string_view::npos &&
         REX_KERNEL_STATE()->content_manager()->IsCustomSongRoot(path.substr(0, colon));
}

}  // namespace

REX_EXTERN(__imp__sub_82767868);
REX_HOOK_RAW(sub_82767868) {
  if (FromCustomSong(ctx.r3.u32, base)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_82767868(ctx, base);
}

REX_EXTERN(__imp__sub_827678F0);
REX_HOOK_RAW(sub_827678F0) {
  if (FromCustomSong(ctx.r3.u32, base)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_827678F0(ctx, base);
}

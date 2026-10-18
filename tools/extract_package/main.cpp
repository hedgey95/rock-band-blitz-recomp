// Unpacks an Xbox 360 STFS package (LIVE/PIRS/CON, e.g. an XBLA title) into a
// host directory, using the SDK's own StfsContainerDevice.
//   extract_package <package file> <output dir>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <queue>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>

using namespace rex::filesystem;
using rex::X_STATUS;  // X_STATUS_SUCCESS expands to an unqualified cast

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <package file> <output dir>\n", argv[0]);
    return 2;
  }
  const std::filesystem::path package = rex::to_path(argv[1]);
  const std::filesystem::path out_dir = rex::to_path(argv[2]);
  if (!std::filesystem::is_regular_file(package)) {
    std::fprintf(stderr, "not a file: %s\n", argv[1]);
    return 2;
  }

  StfsContainerDevice device("", package);
  if (!device.Initialize()) {
    std::fprintf(stderr, "not a readable STFS package: %s\n", argv[1]);
    return 1;
  }

  size_t files = 0, bytes = 0, failures = 0;
  std::vector<uint8_t> buffer;
  std::queue<Entry*> queue;
  queue.push(device.ResolvePath(""));
  while (!queue.empty()) {
    Entry* dir = queue.front();
    queue.pop();
    for (const auto& child : dir->children()) {
      Entry* entry = child.get();
      // Entry paths are package-relative; refuse anything that climbs out of out_dir.
      const auto rel = rex::to_path(entry->path()).relative_path().lexically_normal();
      if (rel.empty() || *rel.begin() == "..") {
        std::fprintf(stderr, "skipping unsafe path: %s\n", entry->path().c_str());
        ++failures;
        continue;
      }
      const auto dest = out_dir / rel;
      if (entry->attributes() & kFileAttributeDirectory) {
        std::filesystem::create_directories(dest);
        queue.push(entry);
        continue;
      }
      std::filesystem::create_directories(dest.parent_path());
      File* in = nullptr;
      if (entry->Open(FileAccess::kFileReadData, &in) != X_STATUS_SUCCESS) {
        std::fprintf(stderr, "open failed: %s\n", entry->path().c_str());
        ++failures;
        continue;
      }
      buffer.resize(entry->size());
      size_t read = 0;
      const bool ok = in->ReadSync(std::span<uint8_t>(buffer.data(), buffer.size()), 0, &read) ==
                          X_STATUS_SUCCESS &&
                      read == buffer.size();
      in->Destroy();
      FILE* out = ok ? rex::filesystem::OpenFile(dest, "wb") : nullptr;
      if (!out || std::fwrite(buffer.data(), 1, read, out) != read || std::fclose(out) != 0) {
        std::fprintf(stderr, "copy failed: %s\n", entry->path().c_str());
        ++failures;
        continue;
      }
      ++files;
      bytes += read;
    }
  }

  // Self-check: an XBLA package must yield a default.xex with the XEX2 magic.
  char magic[4] = {};
  if (FILE* xex = rex::filesystem::OpenFile(out_dir / "default.xex", "rb")) {
    std::fread(magic, 1, 4, xex);
    std::fclose(xex);
  }
  const bool has_xex = std::memcmp(magic, "XEX2", 4) == 0;
  std::printf("%zu files, %zu bytes, %zu failures, default.xex %s\n", files, bytes, failures,
              has_xex ? "OK" : "MISSING");
  return (failures == 0 && has_xex) ? 0 : 1;
}

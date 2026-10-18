// Content packages (song DLC, exports) you copied off your own console:
// --install_content=<package file or folder of them> extracts each into the
// user data folder, under its own title (Rock Band 1-3 packages are what Blitz
// plays, via the XEX's alternate title IDs). Licence bits come only from the
// package's licences that name --content_owner_ids (see the SDK patch).
#include "content_install.h"

#include <filesystem>
#include <system_error>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

REXCVAR_DEFINE_STRING(install_content, "", "Blitz",
                      "Content package (or folder of packages) from your own console to install");

namespace blitz {

void InstallContentIfRequested(rex::system::KernelState* kernel) {
  const std::filesystem::path path = REXCVAR_GET(install_content);
  if (path.empty()) return;
  std::error_code ec;
  std::vector<std::filesystem::path> packages;
  if (std::filesystem::is_directory(path, ec)) {
    for (auto& entry : std::filesystem::recursive_directory_iterator(path, ec))
      if (entry.is_regular_file()) packages.push_back(entry.path());
  } else {
    packages.push_back(path);
  }
  for (auto& package : packages) {
    const auto result = kernel->content_manager()->InstallContent(package);
    if (result) REXLOG_ERROR("install_content: {} failed ({:X})", package.string(), result);
    else REXLOG_INFO("install_content: {}", package.string());
  }
}

}  // namespace blitz

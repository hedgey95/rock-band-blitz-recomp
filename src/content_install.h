// Installs content packages from your own console: see content_install.cpp.
#pragma once

namespace rex::system {
class KernelState;
}

namespace blitz {

void InstallContentIfRequested(rex::system::KernelState* kernel);

}  // namespace blitz

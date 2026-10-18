// blitz - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/rex_app.h>

#include "input_tape.h"
#include "title_keys.h"
#include "diagnostics.h"
#include "renderer/native_graphics.h"

class BlitzApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<BlitzApp>(new BlitzApp(ctx, "blitz",
        PPCImageConfig));
  }

  void OnPostInitLogging() override {
    // The XBLA package's license table grants mask bit 0 (full game) to any
    // console/profile; the SDK default of 0 would run the trial. Explicit
    // settings (config file, environment, command line) still win.
    if (rex::cvar::GetFlagSource("license_mask") == rex::cvar::Source::kDefault)
      rex::cvar::SetFlagByName("license_mask", "1");
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Default pads plus the optional --input_replay player (synthetic, merged into user 0).
    config.input_factory = [](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
      auto input = rex::input::CreateDefaultInputSystem(tool_mode);
      if (auto script = blitz::CreateReplayInputDriver()) input->AddDriver(std::move(script));
      return input;
    };
    config.graphics = blitz::CreateNativeGraphicsIfRequested();
  }

  void OnPostLoadXexImage() override {
    blitz::ApplyTitleKeys(runtime()->memory());
    blitz::DumpImageIfRequested(runtime()->memory(), PPCImageConfig.image_base,
                                PPCImageConfig.image_size);
  }

  void OnPostSetup() override {
    blitz::StartStackDumpTimer();
    auto* graphics = runtime()->graphics_system();
    blitz::StartScreenshotTimer(graphics ? graphics->presenter() : nullptr);
  }

  // Override virtual hooks for customization:
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};

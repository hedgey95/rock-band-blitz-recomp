// blitz - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/rex_app.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include <imgui.h>

#include "input_tape.h"
#include "title_keys.h"
#include "content_install.h"
#include "hub.h"
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
    blitz::ApplyKeyboardDefaults();
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    // The player's settings (blitz.toml next to the exe) stay out of test runs:
    // scripts/run-scripted.ps1 passes --config_file=none.
    const std::string file = rex::cvar::GetFlagByName("config_file");
    if (file == "none") paths.config_path.clear();
    else if (!file.empty()) paths.config_path = file;
    config_path_ = paths.config_path;
    // The SDK takes the game folder from the command line before it loads the
    // config, so read it from blitz.toml here (the command line still wins).
    std::error_code error;
    if (paths.game_data_root.empty() && std::filesystem::exists(config_path_, error)) {
      rex::cvar::LoadConfig(config_path_);
      if (const std::string game = rex::cvar::GetFlagByName("game_data_root"); !game.empty())
        paths.game_data_root = game;
    }
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    hub_ = std::make_unique<blitz::Hub>(drawer, config_path_, user_data_root() / "custom_songs");
    rex::ui::RegisterBind("bind_blitz_hub", "F1", "Toggle the Blitz hub", [this] { hub_->Toggle(); });
  }

  void OnShutdown() override { rex::ui::UnregisterBind("bind_blitz_hub"); }

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
    blitz::InstallContentIfRequested(runtime()->kernel_state());
    blitz::DumpImageIfRequested(runtime()->memory(), PPCImageConfig.image_base,
                                PPCImageConfig.image_size);
  }

  void OnPostSetup() override {
    // Game input pauses while an overlay has the mouse or a text box (the SDK's
    // own check only knows its overlays, so typing in the hub moved the game).
    if (auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system()); input && imgui_drawer())
      input->SetActiveCallback([this] {
        const ImGuiIO& io = imgui_drawer()->GetIO();
        return !io.WantTextInput && !io.WantCaptureMouse;
      });
    blitz::StartStackDumpTimer();
    auto* graphics = runtime()->graphics_system();
    blitz::StartScreenshotTimer(graphics ? graphics->presenter() : nullptr);
  }

  std::filesystem::path config_path_;
  std::unique_ptr<blitz::Hub> hub_;

  // Override virtual hooks for customization:
  // void OnLoadXexImage(std::string& xex_image) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};

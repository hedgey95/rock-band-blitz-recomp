// The Blitz hub (F1, and on first launch): the settings a player changes.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <rex/ui/imgui_dialog.h>

namespace blitz {

// Blitz's keyboard layout (left note F, right note J, lanes D/K, arrows for
// menus, Enter/Backspace/Escape) and keyboard on, unless the config or
// command line already set them. Call once the config is loaded.
void ApplyKeyboardDefaults();

class Hub final : public rex::ui::ImGuiDialog {
 public:
  // config_path: blitz.toml, saved on every change; default_songs_dir: where
  // custom songs are read from when --custom_songs_dir is empty.
  Hub(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
      std::filesystem::path default_songs_dir);

  void Toggle() { visible_ = !visible_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void Set(const char* cvar, const std::string& value);
  void KeyRow(const char* action, const char* cvar);
  void CountSongs();

  std::filesystem::path config_path_, default_songs_dir_;
  bool visible_ = false;
  std::vector<std::string> audio_devices_;  // playback endpoints, named as SDL names them
  std::vector<std::string> restart_;        // settings changed that apply after a restart
  std::string capturing_;                   // keybind cvar waiting for a key
  char songs_dir_[1024] = {};
  size_t song_files_ = 0;
};

}  // namespace blitz

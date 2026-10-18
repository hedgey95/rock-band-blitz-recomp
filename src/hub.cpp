// The Blitz hub: songs folder, audio output, renderer, fullscreen and keyboard
// controls in one window (F1; it also opens on first launch). Every change is
// saved to blitz.toml next to the exe, which the SDK loads at startup, so the
// game starts from a double-click.
#include "hub.h"

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <shellapi.h>
// Defines PKEY_Device_FriendlyName here rather than needing a GUID library.
#include <initguid.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/settings_overlay.h>  // ImGuiKeyToVirtualKey

REXCVAR_DEFINE_BOOL(hub_shown, false, "Blitz", "The hub has been shown once (it opens on first launch)");
REXCVAR_DEFINE_STRING(config_file, "", "Blitz",
                      "Settings file (default: blitz.toml next to the exe; none: defaults only, for test runs)");

namespace blitz {

namespace {

// Blitz plays its left note with the left stick or the d-pad; the stick keeps
// it apart from the arrows, which drive the d-pad in menus.
constexpr const char* kLeftNote = "keybind_lstick_up";

struct KeyDefault {
  const char* cvar;
  const char* keys;
};
constexpr KeyDefault kKeyDefaults[] = {
    {kLeftNote, "F"},
    {"keybind_a", "J,Return,Space"},        // right note; confirm
    {"keybind_left_shoulder", "D"},          // lane left
    {"keybind_right_shoulder", "K"},         // lane right
    {"keybind_start", "Escape"},             // pause
    {"keybind_b", "Backspace"},              // back
    {"keybind_y", "Y"},                      // song list shortcuts
    {"keybind_x", ""},
    {"keybind_back", "Tab"},
    {"keybind_dpad_up", "Up"},
    {"keybind_dpad_down", "Down"},
    {"keybind_dpad_left", "Left"},
    {"keybind_dpad_right", "Right"},
    // The SDK's default layout (WASD stick, arrows on the right stick, F/K
    // stick clicks, Q/E/I/O triggers) would collide with the keys above.
    {"keybind_lstick_down", ""},
    {"keybind_lstick_left", ""},
    {"keybind_lstick_right", ""},
    {"keybind_lstick_press", ""},
    {"keybind_rstick_up", ""},
    {"keybind_rstick_down", ""},
    {"keybind_rstick_left", ""},
    {"keybind_rstick_right", ""},
    {"keybind_rstick_press", ""},
    {"keybind_left_trigger", ""},
    {"keybind_right_trigger", ""},
};

std::string Utf8(const wchar_t* text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  std::string out(size > 0 ? size - 1 : 0, '\0');
  if (size > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}

// Active playback endpoints by friendly name, which is what SDL (and so
// --audio_device) calls them.
std::vector<std::string> PlaybackDevices() {
  std::vector<std::string> names;
  const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* enumerator = nullptr;
  IMMDeviceCollection* devices = nullptr;
  UINT count = 0;
  if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) &&
      SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)) &&
      SUCCEEDED(devices->GetCount(&count))) {
    for (UINT i = 0; i < count; ++i) {
      IMMDevice* device = nullptr;
      IPropertyStore* properties = nullptr;
      if (SUCCEEDED(devices->Item(i, &device)) && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
          names.push_back(Utf8(name.pwszVal));
        PropVariantClear(&name);
      }
      if (properties) properties->Release();
      if (device) device->Release();
    }
  }
  if (devices) devices->Release();
  if (enumerator) enumerator->Release();
  if (SUCCEEDED(init)) CoUninitialize();
  return names;
}

std::string Flag(const char* name) { return rex::cvar::GetFlagByName(name); }

// What blitz.toml holds: the player's settings only. (The SDK's SaveConfig
// writes every non-default setting, one-off command-line flags included, such
// as --audio_mute or --window_hidden.)
constexpr const char* kSavedSettings[] = {
    "game_data_root", "title_keys",   "gpu_plugin", "native_renderer", "native_shader_dir",
    "custom_songs_dir", "content_owner_ids", "audio_device", "fullscreen", "mnk_mode", "hub_shown",
};

std::string TomlValue(const char* name) {
  const auto* info = rex::cvar::GetFlagInfo(name);
  const std::string value = Flag(name);
  if (!info || info->type != rex::cvar::FlagType::String) return value;
  std::string quoted = "\"";
  for (char c : value) {
    if (c == '\\' || c == '"') quoted += '\\';
    quoted += c;
  }
  return quoted + "\"";
}

void SaveSettings(const std::filesystem::path& path) {
  if (path.empty()) return;  // --config_file=none
  std::string toml = "# Rock Band Blitz settings, written by the hub (F1).\n";
  auto add = [&](const char* name) {
    if (rex::cvar::GetFlagInfo(name)) toml += std::string(name) + " = " + TomlValue(name) + "\n";
  };
  for (const char* name : kSavedSettings) add(name);
  for (const auto& key : kKeyDefaults) add(key.cvar);
  std::ofstream file(path, std::ios::binary);
  file << toml;
  if (!file) REXLOG_ERROR("hub: could not save {}", path.string());
}

// The first key of a binding ("J,Return" -> "J"), or "-" for none.
std::string FirstKey(const std::string& keys) {
  const std::string first = keys.substr(0, keys.find(','));
  return first.empty() ? "-" : first;
}

}  // namespace

void ApplyKeyboardDefaults() {
  for (const auto& key : kKeyDefaults)
    if (rex::cvar::GetFlagSource(key.cvar) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(key.cvar, key.keys);
  if (rex::cvar::GetFlagSource("mnk_mode") == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName("mnk_mode", "true");
}

Hub::Hub(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path, std::filesystem::path default_songs_dir)
    : ImGuiDialog(drawer), config_path_(std::move(config_path)), default_songs_dir_(std::move(default_songs_dir)) {
  const std::string songs = Flag("custom_songs_dir");
  std::strncpy(songs_dir_, songs.c_str(), sizeof(songs_dir_) - 1);
  audio_devices_ = PlaybackDevices();
  CountSongs();
  if (!REXCVAR_GET(hub_shown)) {
    visible_ = true;
    Set("hub_shown", "true");
  }
}

void Hub::Set(const char* cvar, const std::string& value) {
  rex::cvar::SetFlagByName(cvar, value);
  SaveSettings(config_path_);
}

void Hub::CountSongs() {
  const std::filesystem::path dir = songs_dir_[0] ? std::filesystem::path(songs_dir_) : default_songs_dir_;
  std::error_code error;
  song_files_ = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir, error))
    if (entry.is_regular_file(error)) ++song_files_;
}

void Hub::KeyRow(const char* action, const char* cvar) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(action);
  ImGui::TableNextColumn();
  ImGui::PushID(cvar);
  const std::string keys = Flag(cvar);
  if (capturing_ == cvar) {
    ImGui::Button("Press a key...", ImVec2(130, 0));
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      capturing_.clear();
    } else {
      for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        if (k == ImGuiKey_Escape || !ImGui::IsKeyPressed(static_cast<ImGuiKey>(k))) continue;
        const std::string name = rex::ui::VirtualKeyToString(rex::ui::ImGuiKeyToVirtualKey(static_cast<ImGuiKey>(k)));
        if (!name.empty()) {
          // The new key replaces the first one; extra keys (Enter for confirm) stay.
          const size_t comma = keys.find(',');
          Set(cvar, comma == std::string::npos ? name : name + keys.substr(comma));
        }
        capturing_.clear();
        break;
      }
    }
  } else if (ImGui::Button(FirstKey(keys).c_str(), ImVec2(130, 0))) {
    capturing_ = cvar;
  }
  ImGui::PopID();
}

void Hub::OnDraw(ImGuiIO& io) {
  if (!visible_) return;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
  ImGui::SetNextWindowBgAlpha(0.94f);
  if (!ImGui::Begin("Rock Band Blitz##hub", &visible_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  ImGui::TextDisabled("F1 opens and closes this window. Changes are saved as you make them.");

  ImGui::SeparatorText("Songs");
  ImGui::TextUnformatted("Custom songs folder");
  ImGui::SetNextItemWidth(380);
  if (ImGui::InputTextWithHint("##songs", default_songs_dir_.string().c_str(), songs_dir_, sizeof(songs_dir_))) {
    Set("custom_songs_dir", songs_dir_);
    CountSongs();
    restart_.push_back("songs folder");
  }
  ImGui::SameLine();
  if (ImGui::Button("Open")) {
    const std::filesystem::path dir = songs_dir_[0] ? std::filesystem::path(songs_dir_) : default_songs_dir_;
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    ShellExecuteW(nullptr, L"explore", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
  ImGui::TextDisabled("%zu file%s there. Drop Magma or Onyx customs in; new songs appear after a restart.",
                      song_files_, song_files_ == 1 ? "" : "s");

  ImGui::SeparatorText("Audio");
  const std::string device = Flag("audio_device");
  const std::string shown = device.empty() ? "Automatic (skips a controller's speaker)" : device;
  ImGui::SetNextItemWidth(380);
  if (ImGui::BeginCombo("Output", shown.c_str())) {
    if (ImGui::Selectable("Automatic (skips a controller's speaker)", device.empty())) {
      Set("audio_device", "");
      restart_.push_back("audio output");
    }
    for (const auto& name : audio_devices_) {
      if (ImGui::Selectable(name.c_str(), name == device)) {
        Set("audio_device", name);
        restart_.push_back("audio output");
      }
    }
    ImGui::EndCombo();
  }

  ImGui::SeparatorText("Video");
  bool native = Flag("native_renderer") == "true";
  if (ImGui::RadioButton("Native renderer", native) && !native) {
    Set("native_renderer", "true");
    restart_.push_back("renderer");
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Xenia reference renderer", !native) && native) {
    Set("native_renderer", "false");
    restart_.push_back("renderer");
  }
  bool fullscreen = Flag("fullscreen") == "true";
  if (ImGui::Checkbox("Fullscreen", &fullscreen)) Set("fullscreen", fullscreen ? "true" : "false");

  ImGui::SeparatorText("Controls");
  bool keyboard = Flag("mnk_mode") == "true";
  if (ImGui::Checkbox("Keyboard (a controller works at the same time)", &keyboard))
    Set("mnk_mode", keyboard ? "true" : "false");
  if (keyboard && ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn("##action", ImGuiTableColumnFlags_WidthFixed, 200);
    KeyRow("Left note", kLeftNote);
    KeyRow("Right note / confirm", "keybind_a");
    KeyRow("Lane left", "keybind_left_shoulder");
    KeyRow("Lane right", "keybind_right_shoulder");
    KeyRow("Pause", "keybind_start");
    KeyRow("Back", "keybind_b");
    KeyRow("Menu up", "keybind_dpad_up");
    KeyRow("Menu down", "keybind_dpad_down");
    KeyRow("Menu left", "keybind_dpad_left");
    KeyRow("Menu right", "keybind_dpad_right");
    ImGui::EndTable();
    if (ImGui::Button("Reset keys")) {
      for (const auto& key : kKeyDefaults) rex::cvar::SetFlagByName(key.cvar, key.keys);
      SaveSettings(config_path_);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Click a key to change it; Escape cancels.");
  }

  if (!restart_.empty()) {
    std::sort(restart_.begin(), restart_.end());
    restart_.erase(std::unique(restart_.begin(), restart_.end()), restart_.end());
    std::string list;
    for (const auto& item : restart_) list += (list.empty() ? "" : ", ") + item;
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Restart the game to apply: %s.", list.c_str());
  }
  ImGui::End();
}

}  // namespace blitz

# Playing

Double-click `blitz.exe`. Its settings live in `blitz.toml` next to it; the
hub writes that file, so the command line is only needed for development.

## The hub (F1)

F1 opens and closes it; it also opens on first launch. Changes save as you
make them.

- **Songs:** the custom songs folder (Magma or Onyx customs; see
  `content-packages.md`), how many files are in it, and a button to open it.
  New songs appear after a restart.
- **Audio:** the output device. Automatic is Windows' default, except that it
  skips a controller's speaker (a USB DualSense becomes the default output).
  Applies after a restart.
- **Video:** native renderer or the Xenia reference renderer (after a restart),
  and fullscreen (at once).
- **Controls:** keyboard on or off, and its keys. A controller works at the
  same time.

## Keyboard

| Action | Key |
|---|---|
| Left note | F |
| Right note, confirm | J (also Enter, Space) |
| Lane left / right | D / K |
| Pause | Escape |
| Back | Backspace |
| Menus | Arrow keys |
| Song list shortcuts | Y |

Click a key in the hub to change it (Escape cancels). The left note is the
left stick pushed up: Blitz plays the left note on the stick or the d-pad, and
the stick keeps it apart from the arrows, which drive the d-pad in menus.
(Measured: on the One Week tape, A alone scores 400; A with d-pad up or with
the stick up both score 1,400.)

## For development

Command-line flags override `blitz.toml`. `--config_file=<path>` uses another
settings file and `--config_file=none` none at all; `scripts/run-scripted.ps1`
passes `none` (and `--hub_shown=true`), so test runs never see a player's
settings. The SDK's own overlays remain: F3 debug, F4 every setting, Backtick
console.

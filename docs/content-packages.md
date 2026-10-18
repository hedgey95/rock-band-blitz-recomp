# Songs beyond the base game

Blitz plays songs from Rock Band 1–3 content packages: its XEX lists those
titles (45410829, 45410869, 45410914) as alternate title IDs. Nothing here is
shipped; you bring the files.

## Custom songs

Put custom song packages (CON files built with Magma, the community authoring
tool) in a folder and point the game at it:

```
blitz.exe --custom_songs_dir=D:\customs
```

The default is `custom_songs` inside the user data folder. Packages are read
in place; nothing is copied or extracted, and the game cannot change or delete
them. New files show up the next time the game builds its song list (at boot).

Blitz checks every DLC chart against a built-in table of checksums for
Harmonix's own songs. A custom chart is never in that table, so
`src/custom_song_checks.cpp` skips that check for files from the custom-songs
folder only.

Only packages that say they were built with Magma are loaded from this folder.
Repackaged official DLC is not a custom and is skipped.

## Your own DLC

Packages copied off your console (Settings → System → Storage) install with:

```
blitz.exe --install_content=<package or folder> --content_owner_ids=<your XUID>,<your console ID>
```

A package gets its licence only if one of its licences names an ID you list,
as on the console. Unlicensed songs don't appear in the song list.

Verified on 2026-10-18 with one Magma custom (NERO, "Promises"): it is listed,
loads and plays to the end of the tape on both renderers. Its mogg is
unencrypted. Console-copied DLC is not tested yet (none available).

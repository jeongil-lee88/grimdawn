# Grim Dawn Korean Voice Pack — source of the two DLLs

Source code and build instructions for the two small DLLs shipped in the **Grim Dawn Korean Voice Pack** on Nexus Mods.
Everything else in the pack is data: `mods/Conversations.arc`, `mods/Sound.arc` (Korean WAV audio),
`mods/database.arz` (volume / distance values of the game's own monster-shout sound records) and
`mods/lore_audio/*.wav` + text lists. None of those are executable.

Supported game: Grim Dawn 1.3.1.1, Steam build 25813250, x64.

## The DLLs

### `x64/dinput8.dll` — loader (`proxy/dinput8-proxy.c`, 121 lines)
- Exports the six functions of `dinput8.dll` (`DirectInput8Create`, `DllCanUnloadNow`, `DllGetClassObject`,
  `DllRegisterServer`, `DllUnregisterServer`, `GetdfDIJoystick`) and forwards each call to the real
  `System32\dinput8.dll`.
- `DllMain` only starts a thread. The thread waits until `Game.dll` and `Engine.dll` are loaded, checks the
  SHA-256 of `Grim Dawn.exe`, `Game.dll` and `Engine.dll` against the supported build (via `bcrypt.dll`),
  and only then calls `LoadLibraryW` on known module names next to it:
  `x64\GrimDawnKoreanVoice\native-lorevo.dll` (this pack) and, if the user also installed it, the modules of my
  separate map mod (GrimDawnNativeMap) in `x64\GrimDawnNativeMap`. Missing modules are skipped.
- Writes only its own log, `dinput8-proxy.log`.

### `x64/GrimDawnKoreanVoice/native-lorevo.dll` — lore-note narration (`native-lorevo/`)
- Plays a Korean WAV when a lore note is read (inventory, journal page, or a small clickable scroll icon drawn on
  the world where an already-read note was found).
- Hooks exported Grim Dawn functions with [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2, bundled in
  `third_party/minhook`): `LocalizationManager::LocalizeWithoutParams`, `ItemNote::Use`,
  `Player::AddItemToLoreCodex`, `ControllerPlayerStateTalkToNpc::OnBegin`, a few `ControllerPlayerState*::OnBegin`
  (to know when a conversation is over) and `ActivityManager::Render` (draws the note icons with the game's own
  `GraphicsCanvas::RenderRect`). The game window procedure is subclassed only to catch clicks on those icons.
- Audio is played with `waveOut*` from the system `winmm.dll`.
- Reads `mods/lore_audio/lore_voice_ko.txt`, `lore_spots.txt`, the WAV files, optional `lore_volume.txt`, and the
  game's `options.txt` (volume). Writes only its own log, `lore-voice.log`.
- No network access, no registry access, no file writes other than the log.

## Build

Run `build.bat` from an **x64 Native Tools Command Prompt for VS 2022** (MSVC 14.44.35207 used for the release),
with **TinyCC 0.9.27 (x86_64-win32)** on `PATH` or `set TCC=C:\path\to\tcc.exe`:

```
build.bat
```

Output: `build\native-lorevo.dll` and `build\dinput8.dll`. Both builds are reproducible (`/Brepro` for MSVC,
TinyCC has no timestamps), so the files are byte-identical to the released ones:

| File | SHA-256 |
|---|---|
| `native-lorevo.dll` | `fc2c6b7c560b103209199fb0c736fea4ff95196dba8b80db569115e883c51623` |
| `dinput8.dll` | `ec58034a4e22f574bf61c26ef9570ca240661c4c54bdb8f1139d4057f1449725` |

Manual steps (what `build.bat` does):

```
cl /nologo /c /O2 /MD /W3 /GS- /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /Brepro /I third_party\minhook\include ^
   third_party\minhook\src\buffer.c third_party\minhook\src\hook.c third_party\minhook\src\trampoline.c ^
   third_party\minhook\src\hde\hde64.c native-lorevo\native-lorevo.c
link /nologo /DLL /MACHINE:X64 /OPT:REF /INCREMENTAL:NO /Brepro /OUT:native-lorevo.dll ^
   native-lorevo.obj buffer.obj hook.obj trampoline.obj hde64.obj kernel32.lib user32.lib shell32.lib shlwapi.lib
tcc -shared -o dinput8.dll proxy\dinput8-proxy.c
```

## License

The source is published so the DLLs can be reviewed and rebuilt. MinHook keeps its own BSD-2 license
(`third_party/minhook/LICENSE.txt`). The Korean voice recordings in the mod are not part of this repository.
Grim Dawn © Crate Entertainment; this is an unofficial fan mod.

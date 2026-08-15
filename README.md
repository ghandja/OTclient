# CrystalOTC

A game client for the CrystalOTC server (protocol 15.30). The `src` base was
taken from [Kokekanon](https://github.com/kokekanon)'s fork of
[OTClient - Redemption](https://github.com/mehah/otclient) (OTCR) and extended
with a set of in-house features:

- **Custom Vulkan renderer** - atlas-batched sprite pipeline: one 2D-array atlas,
  content-hash deduplication, chunked storage for oversized textures, and a
  MAILBOX-first presentation mode (uncapped FPS without tearing). The client can
  start in pure-Vulkan mode without ever creating an OpenGL context.
- **Memory-optimized asset lifecycle** - lazy module UI construction, batched GL
  texture deletion, pixel-copy garbage collection with disk reload on first draw,
  heap trimming after the appearances parse, and live memory diagnostics
  (`[mem]` / `[boot]` / `[gc]` log lines).
- **Protocol 15.30 support** with protobuf appearances and per-version asset
  directories (`data/things/1530/`).

## Game assets (`data/things`)

The client assets (sprites/appearances) are **not** part of this repository -
`data/things/` is gitignored because of its size. Download the content here:

**[data/things - Google Drive](https://drive.google.com/file/d/1pKOYzTEn9D6dOEB4FDd3gZQt7CyRQ9F1/view?usp=drive_link)**

Extract it into `data/things/` so you end up with:

- `data/things/1530/` - protocol 15.30 assets (protobuf appearances,
  `catalog-content.json`, sprite sheets) plus the `satellite/` tiles used by the
  Cyclopedia Map in Surface View.
- `data/things/1530_mapview/` - a second tile set for the Cyclopedia Map's
  "Map View" toggle (classic minimap palette with building outlines, generated
  by `tools/build_mapview_tiles.py`). Both folders are required.

## Building (Windows)

Requirements: Visual Studio 2022 (v143 toolset), [vcpkg](https://github.com/microsoft/vcpkg)
with `VCPKG_ROOT` set. Dependencies are restored automatically from `vcpkg.json`.

```
msbuild vc18\otclient.vcxproj /p:Configuration=DirectX /p:Platform=x64
```

The executable is produced as `CrystalOTC.exe` in the repository root.
Select the renderer with `renderBackend = vulkan` or `gl` in `config.ini`
(also available in-game under Options -> Graphics).

## Building (Linux / Docker / Android)

The upstream CMake build is preserved - see `CMakeLists.txt`, `Dockerfile`
and the scripts in the repository root. The Vulkan renderer is currently
Windows-only; other platforms use the OpenGL path.

## Credits and license

CrystalOTC is a derivative of [OTClient - Redemption](https://github.com/mehah/otclient)
by mehah and contributors (the `src` base came from
[Kokekanon](https://github.com/kokekanon)'s OTCR fork), which itself descends from
[OTClient](https://github.com/edubart/otclient) by edubart and contributors.

Licensed under the MIT License - see [LICENSE](LICENSE).

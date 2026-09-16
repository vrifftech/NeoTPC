# NeoTPC

[![CI](https://github.com/vrifftech/NeoTPC/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoTPC/actions/workflows/ci.yml)

NeoTPC is a cross-platform texture viewer, editor, and converter for BioWare/Odyssey texture assets. It is aimed primarily at KotOR modding and includes both a wxWidgets desktop application and a command-line tool.

## Features

- Open **TPC, TXB, TGA, DDS, PNG, JPEG, BMP, and TXI** files.
- Export textures as **TPC, TGA, DDS, PNG, JPEG, or BMP**.
- View mipmaps, cubemaps, animation layers, RGB/alpha channels, and transparency.
- Zoom, pan, fit-to-window, and compare related images side by side.
- Perform basic pixel operations such as alpha editing and image flipping.
- Read and edit embedded or standalone **TXI** metadata.
- Validate TXI and browse a searchable TXI directive reference grid.
- Preview conversion settings before exporting.
- Batch-convert folders with recursive scanning, overwrite controls, and collision handling.
- Use `neotpc-cli` for scripts and automated conversion workflows.

TXB files are supported as input only and should be exported to another format after editing.

## Basic workflow

1. Open a texture with **File → Open**.
2. Inspect the image on the **Texture** tab.
3. Edit or validate metadata on the **TXI** tab.
4. Use the **Reference** tab to look up TXI directives.
5. Open **File → Export / Convert** or press **Ctrl+E** to configure an output format and destination.
6. Optionally preview the encoded result, then choose **Export copy**.

**Save** preserves the current document format. Use **Export / Convert** when changing formats or encodings.

For multiple files, use **File → Batch conversion**.

## Supported formats

| Format | Read | Write | Notes |
| --- | :---: | :---: | --- |
| TPC | Yes | Yes | RGB/RGBA/grayscale, DXT1/BC1, DXT5/BC3, mipmaps, embedded TXI |
| TXB | Yes | No | Xbox texture input; export to another format |
| TGA | Yes | Yes | True-color, grayscale, paletted, RLE |
| DDS | Yes | Yes | Standard and BioWare DDS, mipmaps and cubemaps |
| PNG | Yes | Yes | Uses libspng |
| JPEG | Yes | Yes | Uses a libjpeg-compatible library; alpha is discarded on export |
| BMP | Yes | Yes | Common paletted and true-color variants |
| TXI | Yes | Yes | Standalone metadata editing and validation |

## Command line

Examples:

```text
neotpc-cli info diffuse.tpc
neotpc-cli convert diffuse.tga diffuse.tpc --compression dxt5
neotpc-cli convert texture.tpc texture.png
neotpc-cli batch source converted --format tpc --recursive
neotpc-cli txi-validate diffuse.tpc
```

Run:

```text
neotpc-cli --help
```

for the complete command and conversion options.

## Building

NeoTPC uses C++17, CMake, wxWidgets, `neoshared`, libspng, and a libjpeg-compatible library. The repository includes a vcpkg manifest for its external dependencies.

Keep NeoTPC and `neoshared` beside each other:

```text
workspace/
  neoshared/
  NeoTPC/
  vcpkg/        # optional, but recommended
```

The build scripts automatically look for `../neoshared` and `../vcpkg`.

### Linux

Install a compiler, CMake, Ninja, and wxWidgets, then run:

```sh
./scripts/build.sh \
  --vcpkg-root ../vcpkg \
  --wx ON \
  --require-wx ON
```

For a CLI-only build:

```sh
./scripts/build.sh \
  --vcpkg-root ../vcpkg \
  --wx OFF \
  --cli ON
```

### macOS

For the application bundle:

```sh
./scripts/build-macos.sh --vcpkg-root ../vcpkg
```

### Windows

From a Visual Studio Developer PowerShell:

```powershell
.\scripts\build.ps1 `
  -Wx ON `
  -RequireWx ON `
  -VcpkgRoot ..\vcpkg
```

If dependencies are already provided by the system, the native build scripts support `--no-vcpkg` / `-NoVcpkg`. The WebAssembly build manages its pinned vcpkg checkout automatically.

## Repository layout

- `src/core` — document and application state
- `src/texture` — NeoTPC-specific image conversion support
- `src/wx` — desktop UI
- `src/cli` — command-line application
- `scripts` — build helpers
- `neoshared` — separate sibling repository containing shared texture, TXI, and UI infrastructure

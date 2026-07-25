# NeoTPC

[![CI](https://github.com/vrifftech/NeoTPC/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoTPC/actions/workflows/ci.yml)

NeoTPC is a standalone C++17/wxWidgets viewer, TXI inspector, and
converter for Odyssey texture assets.

## Features

- Opens TPC, Xbox TXB, TGA, DDS, PNG, JPEG/JPG, BMP, and TXI files. Container
  signatures are detected from content, so valid textures can still open when
  their extension is missing or wrong.
- Checkerboard RGBA, opaque RGB, alpha grayscale, and alpha red-mask views.
- Zoom, pan, fit-to-window, actual-pixel display, cube-face selection,
  per-layer mip-level selection, and animation-frame playback.
- **Open Conflicting Images** scans the current file's folder for image files
  with the same case-insensitive base name, including numbered duplicates and
  `copy` suffixes. It displays every match beside the editable image in
  independently resizable, read-only comparison panes. Layer, mip, channel,
  fit, and toolbar zoom selections are applied across the panes. Loading is
  performed on a background worker, remains cancellable, and is bounded to 64
  panes, 128 MiB of encoded input per candidate, and 256 MiB of retained
  decoded comparison data.
- Converts pixel-bearing inputs to TPC, TGA, DDS, PNG, JPEG, or BMP.
- Reads, edits, validates, embeds, and exports TXI metadata, with a searchable
  directive reference.
- Reads and preserves authored mip chains. Writes uncompressed, grayscale,
  Xbox Morton-swizzled BGRA, DXT1/BC1, and DXT5/BC3 TPC data; DDS additionally
  supports DXT3/BC2 and standard six-face cubemaps.
- Reads standard DDS BGRA/BGR, A1R5G5B5, R5G6B5, ARGB4444,
  DXT1/DXT3/DXT5, pitched rows, mipmap, and full or partial cubemap layouts,
  plus BioWare DDS.
- Normalizes Odyssey TPC cubemap face order/orientation for viewing and reverses
  that normalization on export. A square 6:1 vertical TGA strip is recognized
  as a cubemap even without `cube 1` metadata.
- Parses counted TXI font-coordinate blocks structurally, accepts common legacy
  aliases such as `decal1`, and validates the broader engine/tool key catalog.
- Performs recursive batch conversion with relative paths, sidecar awareness,
  overwrite policy, progress/cancel support, and collision-safe names.
- Replaces an image and its TXI sidecar as a rollback-capable transaction.
- Includes `neotpc-cli` for automation and conversion pipelines.
- Uses the same Neo dark-mode settings/status-bar behavior, release helper,
  scripts, static Windows runtime policy, icon layout, and install conventions
  as the other Neo tools.

## Repository boundary

NeoTPC consumes settings and common wxWidgets UI infrastructure from the separate `neoshared` repository through the `neoshared::wx` target. Texture-domain code, renderer data, the TXI catalog, and the batch workflow remain in NeoTPC because no other tool currently needs them. PNG and JPEG implementations are external dependencies declared in `vcpkg.json`; codec source is not vendored in this repository.

Clone the repositories as siblings:

```text
workspace/
  neoshared/
  NeoTPC/
```

CMake automatically detects `../neoshared`. For another layout, pass `--neoshared-root /path/to/neoshared` to `build.sh`, `-NeoSharedRoot C:\path\to\neoshared` to `build.ps1`, or set `NEOSHARED_ROOT` directly.

NeoTPC does not currently publish a C++ SDK. Codec and application headers live under `src/` and are private implementation details; the install target contains only the applications and required documentation.

## Format support

| Format | Read | Write | Coverage |
| --- | --- | --- | --- |
| TPC | Yes | Yes | Raw RGB/RGBA/grayscale, Xbox 0x0C Morton-swizzled BGRA, DXT1, DXT5, complete mip chains, embedded TXI, canonical cube/animation layers |
| TXB | Yes | No | Xbox swizzled BGRA/grayscale and DXT1/DXT5 input with embedded TXI; convert to another format |
| TGA | Yes | Yes | Paletted, true-color, grayscale, and RLE input; RGBA output; 6:1 cubemap inference |
| DDS | Yes | Yes | Standard and BioWare headers; BGRA/BGR with stored row pitch, A1R5G5B5, R5G6B5, ARGB4444, DXT1/DXT3/DXT5, mipmaps, and full or partial cubemaps |
| PNG | Yes | Yes | External libspng dependency; zlib is resolved transitively |
| JPEG | Yes | Yes | External libjpeg-compatible dependency; the vcpkg manifest selects libjpeg-turbo; output drops alpha |
| BMP | Yes | Yes | Paletted 1/4/8-bit and 16/24/32-bit true-color input |
| TXI | Yes | Yes | Source editing, typed validation, searchable catalog |

## Build

NeoTPC requires a C++17 compiler, CMake, the sibling `neoshared` repository,
libspng, and a libjpeg-compatible implementation. The committed `vcpkg.json`
pins the dependency registry and selects libspng plus libjpeg-turbo. On Windows
it also selects wxWidgets. No manual `vcpkg install` command is required;
configuration with the vcpkg toolchain installs the manifest automatically.

Recommended sibling layout:

```text
workspace/
  neoshared/
  NeoTPC/
  vcpkg/
```

Bootstrap vcpkg once:

```sh
git clone https://github.com/microsoft/vcpkg.git ../vcpkg
bash ../vcpkg/bootstrap-vcpkg.sh -disableMetrics
```

Linux GUI build. wxWidgets comes from the system package while the image codecs
come from the pinned vcpkg manifest:

```sh
sudo apt install build-essential cmake ninja-build pkg-config libwxgtk3.2-dev
bash ./scripts/build.sh \
  --vcpkg-root ../vcpkg \
  --vcpkg-triplet x64-linux \
  --wx ON \
  --require-wx ON \
  --jobs "$(nproc)"
```

Linux CLI/core-only build:

```sh
bash ./scripts/build.sh \
  --vcpkg-root ../vcpkg \
  --vcpkg-triplet x64-linux \
  --wx OFF \
  --cli ON \
  --jobs "$(nproc)"
```

Windows GUI build from a Visual Studio 2026 Developer PowerShell:

```powershell
git clone https://github.com/microsoft/vcpkg.git ..\vcpkg
..\vcpkg\bootstrap-vcpkg.bat -disableMetrics

.\scripts\build.ps1 `
  -Wx ON `
  -RequireWx ON `
  -VcpkgRoot ..\vcpkg `
  -VcpkgTriplet x64-windows-static `
  -Parallel ([Environment]::ProcessorCount)
```

`--no-vcpkg` or `-NoVcpkg` remains available for environments that already
provide a CMake package exporting `spng::spng` or `spng::spng_static`, a
`JPEG::JPEG` target, and wxWidgets when the GUI is enabled.

Run the codec tests with:

```sh
bash ./scripts/build.sh \
  --build-dir build-tests \
  --vcpkg-root ../vcpkg \
  --vcpkg-triplet x64-linux \
  --wx OFF \
  --clean \
  -- \
  -DNEOTPC_BUILD_TESTS=ON
ctest --test-dir build-tests --output-on-failure
```

## CLI examples

```text
neotpc-cli info diffuse.tpc
neotpc-cli convert xbox-texture.txb xbox-texture.tpc --compression swizzled-bgra
neotpc-cli convert diffuse.tga diffuse.tpc --compression dxt5 --dxt-quality high --bicubic
neotpc-cli convert translucent.tga translucent.dds --compression dxt3
neotpc-cli convert mask.png mask.dds --compression dxt1 --dxt1-alpha-threshold 128
neotpc-cli batch source converted --format tpc --recursive --compression auto
neotpc-cli txi-validate diffuse.tpc
```

Run `neotpc-cli --help` for all conversion and alpha/TXI mutation options.

## Comparing conflicting images

Open the image you want to treat as the editable source, then choose **File →
Open Conflicting Images**, press **Ctrl+Shift+O**, or use the button on the
Texture page. For `DXUNtex.tga`, NeoTPC will group same-folder names such as
`dxuntex.tpc`, `dxUnTex(1).tga`, `dxuntex(2) - copy.tpc`, and
`dxuNteX- copy.tga`. TXI sidecars and unrelated longer stems are excluded.
Each candidate is decoded on a bounded background worker so the progress
dialog stays responsive. Drag any divider between panes to resize the
comparison layout.
Use **File → Close Image Comparison** to return to a single editable pane.



## Continuous integration

GitHub Actions checks out `vrifftech/neoshared` beside this repository, reads the pinned vcpkg baseline from `vcpkg.json`, and checks out that exact vcpkg revision. It then builds the full wxWidgets application on Ubuntu 24.04 and Windows Server 2025 with Visual Studio 2026. Codec dependencies are restored through the vcpkg manifest and GitHub Actions binary cache. Successful non-pull-request runs publish staged Linux and Windows artifacts.

The shared dependency defaults to `neoshared/main`. Set the repository Actions variable `NEOSHARED_REF` to a release tag or commit SHA to pin normal CI builds. A manual workflow run can override the ref, and the workflow accepts the `neoshared-updated` repository-dispatch event for cross-repository compatibility checks.

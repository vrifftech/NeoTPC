# Third-party notices

NeoTPC does not vendor third-party codec source. The dependency declarations and
pinned vcpkg registry revision are recorded in `vcpkg.json`. When NeoTPC is
configured through vcpkg, installation also copies the authoritative vcpkg
`copyright` files into `share/doc/NeoTPC/third-party`.

## libspng

PNG import and export use libspng, distributed under the BSD 2-Clause License.
libspng depends on zlib for compression and decompression.

## libjpeg-turbo

The vcpkg manifest supplies libjpeg-turbo for JPEG import and export through the
standard libjpeg API. libjpeg-turbo is distributed under the Independent JPEG
Group License and a modified 3-clause BSD license, with some components under
the zlib license.

This software is based in part on the work of the Independent JPEG Group.

## wxWidgets

The desktop interface uses wxWidgets under the wxWindows Library Licence,
Version 3.1. The Windows vcpkg dependency is declared in `vcpkg.json`; Linux
builds may use the platform wxWidgets development package.

## zlib

zlib is a transitive dependency of libspng and may also be used by wxWidgets.
It is distributed under the zlib License.

Binary distributors using a non-vcpkg dependency source must include the exact
license and notice files supplied by those dependency builds.

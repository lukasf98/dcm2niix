# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Prime Directive: Do No Harm

dcm2niix handles real-world DICOM data from dozens of vendors, many of which have idiosyncratic or outright incorrect implementations of the DICOM standard. The codebase is full of intentional kludges, special cases, and workarounds that exist because specific scanners produce data that requires them. Before modifying any code:

- **Assume every quirk is load-bearing.** Vendor-specific branches, magic constants, commented explanations, and compiler directives are clues to hard-won workarounds. Do not simplify, refactor, or remove them without understanding what real-world data they handle.
- **Make surgical, minimal changes.** Touch only what is necessary. Avoid refactoring surrounding code, reorganizing conditionals, or "cleaning up" logic adjacent to your change.
- **Preserve compatibility with malformed data.** Correctness here means handling what scanners actually produce, not what the DICOM spec says they should produce.
- **When in doubt, don't change it.** If you cannot determine why a piece of code exists, leave it alone.

## Project Overview

dcm2niix converts DICOM medical images to NIfTI format. Written in C/C++ with no required external dependencies. Supports 20+ scanner vendors (Siemens, GE, Philips, Canon, UIH, etc.) and multiple compressed transfer syntaxes (JPEG, JPEG-LS, JPEG2000).

## Build Commands

### Quick build (CMake, recommended for full features):
```bash
mkdir build && cd build
cmake -DZLIB_IMPLEMENTATION=Cloudflare -DUSE_JPEGLS=ON -DUSE_OPENJPEG=ON ..
make
```
Output binary: `build/bin/dcm2niix`

### Simple build (Make, no optional decoders):
```bash
cd console && make
```

### Makefile targets (from console/):
- `make` — optimized release build
- `make debug` — debug symbols, no optimization
- `make sanitize` — AddressSanitizer build
- `make jp2` — with OpenJPEG (JPEG2000)
- `make turbo` — with TurboJPEG (lossy JPEG)
- `make wasm` — WebAssembly build

### Key CMake options:
- `-DZLIB_IMPLEMENTATION=Miniz|System|Cloudflare` — compression backend (default: Miniz bundled)
- `-DUSE_OPENJPEG=OFF|GitHub|System` — JPEG2000 decompression
- `-DUSE_JPEGLS=ON|OFF` — JPEG-LS decompression (CharLS, bundled in console/charls/)
- `-DUSE_TURBOJPEG=ON|OFF` — lossy JPEG via libjpeg-turbo
- `-DBATCH_VERSION=ON` — build dcm2niibatch (requires yaml-cpp)

## Testing

No automated test suite. Validate manually against QA datasets in `dcm_qa/`, `dcm_qa_nih/`, `dcm_qa_uih/`:
```bash
./dcm2niix -v 2 ../dcm_qa/
```

## Architecture

All source is in `console/`. Key files:

- **main_console.cpp** — entry point, CLI argument parsing, orchestrates conversion
- **nii_dicom.cpp** (~374k lines) — DICOM parser. Reads headers, extracts metadata into `TDICOMdata` struct, handles all vendor-specific tag parsing. This is the core of the project
- **nii_dicom.h** — defines `TDICOMdata` (280+ fields), `TDTI4D` (diffusion/4D params), `TCSAdata` (Siemens CSA). All key data structures live here
- **nii_dicom_batch.cpp** — batch processing: groups DICOMs into series, merges slices into 3D/4D volumes, assembles the final NIfTI
- **nifti1_io_core.cpp** — NIfTI writer, BIDS JSON sidecar generation, image reorientation
- **nii_foreign.cpp** — non-DICOM format support (Philips PAR/REC)
- **nii_ortho.cpp** — slice reorientation, cropping, resampling
- **jpg_0XC3.cpp** — lossless JPEG decoder for DICOM transfer syntax
- **ujpeg.cpp** — bundled NanoJPEG (lossy JPEG fallback)

Bundled libraries (no external install needed): miniz (zlib), cJSON, NanoJPEG, CharLS (in `console/charls/`).

### Data flow
1. `main_console.cpp` parses CLI args into `TDCMopts` struct
2. `nii_dicom_batch.cpp` scans input directory, calls `nii_dicom.cpp` to parse each DICOM file
3. `nii_dicom.cpp` populates `TDICOMdata` with metadata and decompresses pixel data if needed
4. `nii_dicom_batch.cpp` groups files by series, assembles volumes
5. `nifti1_io_core.cpp` writes NIfTI files and JSON sidecars

### Feature macros (preprocessor)
`myEnableJPEGLS`, `myTurboJPEG`, `myEnableJasper`, `myDisableOpenJPEG`, `myEnableJNIFTI`, `myDisableMiniZ`, `myDisableClassicJPEG`

## Git Workflow

- **master** — stable releases only, no PRs accepted
- **development** — active development branch, PRs go here
- PRs should target `development`, not `master`

## Code Style

Formatted with clang-format:
```bash
clang-format -i -style="{BasedOnStyle: LLVM, IndentWidth: 4, IndentCaseLabels: false, TabWidth: 4, UseTab: Always, ColumnLimit: 0}" *.cpp *.h
```

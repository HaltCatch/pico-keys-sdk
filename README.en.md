# Pico Keys SDK

[中文](README.md) · [English](README.en.md)

Shared foundations for Pico Keys firmware: USB, board support, storage and crypto-library integration. Used as a pinned submodule by FIDO, OpenPGP and combined firmware, with hardware and features selected by the parent build.

## Features

- Board support and build options for Pico/RP2350 and ESP32 targets.
- Shared low-level USB and storage interfaces for firmware applications.
- Pinned integrations of Mbed TLS, TinyCBOR and libcvc.
- CMake integration through the parent project with common feature switches.

## Quick start

A C/C++ toolchain, CMake and Git; hardware targets require Pico SDK or ESP-IDF. Desktop integration tests also require SDL2 and Python 3.

```sh
git clone https://github.com/HaltCatch/t-dongle-key.git
cd t-dongle-key
bash tools/checkout-deps.sh
bash tools/run-desktop.sh --test
```

## Configuration

| Setting | Purpose |
| --- | --- |
| `CMakeLists.txt / cmake/` | SDK build and feature options. |
| `config/` | Board and crypto-library settings. |
| `.gitmodules` | Pinned third-party modules. |

## Usage

Import `picokeys_sdk_import.cmake` from the parent project and select the board and application features through build options. This is not a standalone desktop program; see [T-Dongle Key](https://github.com/HaltCatch/t-dongle-key) for an integration build. Upstream: [polhenarejos/pico-keys-sdk](https://github.com/polhenarejos/pico-keys-sdk).

## Project structure

- `src/` — Shared implementation
- `config/` — Configuration
- `cmake/` — Build integration

## License

[AGPL-3.0](LICENSE). See [LICENSE_SCOPE.md](LICENSE_SCOPE.md) for the scope of original material and third-party components, and the accompanying notices for third-party copyrights and licenses.

# Pico Keys SDK

[中文](README.md) · [English](README.en.md)

Pico Keys 固件的公共基础模块，统一 USB、板级支持、存储与密码库接入。通常作为 FIDO、OpenPGP 或组合固件的固定版本子模块使用，由父工程选择硬件与功能。

## 功能

- 板级支持：Pico/RP2350 与 ESP32 相关配置和构建选项。
- USB 与存储：供固件应用共享的底层接口。
- 密码库集成：固定的 Mbed TLS、TinyCBOR 和 libcvc 子模块。
- CMake 接入：通过父工程导入 SDK，统一功能开关。

## 快速开始

C/C++ 工具链、CMake、Git；目标板需要对应的 Pico SDK 或 ESP-IDF。桌面集成测试需要 SDL2 与 Python 3。

```sh
git clone https://github.com/HaltCatch/t-dongle-key.git
cd t-dongle-key
bash tools/checkout-deps.sh
bash tools/run-desktop.sh --test
```

## 配置

| 配置项 | 说明 |
| --- | --- |
| `CMakeLists.txt / cmake/` | SDK 构建与功能选项。 |
| `config/` | 板型与密码库配置。 |
| `.gitmodules` | 固定的第三方模块。 |

## 使用

在父工程中导入 `picokeys_sdk_import.cmake`，通过构建参数选择目标板和应用功能。该仓库不是独立的桌面程序，完整集成入口见 [T-Dongle Key](https://github.com/HaltCatch/t-dongle-key)。上游项目为 [polhenarejos/pico-keys-sdk](https://github.com/polhenarejos/pico-keys-sdk)。

## 项目结构

- `src/` — 共享实现
- `config/` — 配置
- `cmake/` — 构建接入

## 许可

[AGPL-3.0](LICENSE)。原创部分与第三方组件的适用范围见 [LICENSE_SCOPE.md](LICENSE_SCOPE.md)；各第三方版权和许可见相应声明文件。

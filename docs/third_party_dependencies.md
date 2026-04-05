# 第三方依赖清单与确认记录

## 1. 已使用依赖

1. `gulrak/filesystem`
- 路径：`third_party/filesystem`
- 版本：仓库内固定版本（按当前检出）
- 许可证：MIT
- 用途：在 C++11 下提供跨平台文件系统 API
- 对外暴露：否（仅内部实现）

2. `zlib`
- 路径：`third_party/zlib`
- 版本：仓库内固定版本（当前为 1.3.2 工程）
- 许可证：zlib
- 用途：默认压缩与解压能力（deflate/inflate）
- 对外暴露：否（仅公共行为，非头文件暴露）

## 1.1 已确认待接入依赖（用户已提供）

1. `boringssl`
- 路径：`third_party/boringssl`
- 用途：后续加密功能实现（当前仅预留接口）
- 状态：已确认，未启用

2. `lz4`
- 路径：`third_party/lz4`
- 用途：后续高吞吐压缩算法适配
- 状态：已接入默认 lz4 codec 工厂；当前采用 `pzip` 自定义 ZIP method，仅保证 `libpzip` 自身读写兼容

3. `zstd`
- 路径：`third_party/zstd`
- 用途：后续高压缩比算法适配
- 状态：已确认，已接入默认 zstd codec 工厂（可通过 C API 显式启用）

## 2. 依赖确认模板（新增依赖必须填写）

- 依赖名称：
- 许可证：
- 目标平台支持：Windows / macOS / Linux
- 版本来源：tag / commit / release
- 引入方式：bundled / system package
- 对外 API/ABI 影响：
- 安全与维护评估：
- 确认人：
- 确认日期：

## 3. 当前策略

- 默认优先 `third_party` 内置依赖，避免环境漂移。
- 新增依赖前必须得到确认，确认后再编码实现。
- 禁止将第三方类型泄漏到 `include/pzip.h`。

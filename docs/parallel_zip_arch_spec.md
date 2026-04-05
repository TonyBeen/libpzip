# 并行压缩库重构架构与规范（v1）

## 1. 目标与范围

本规范用于指导当前遗留 C++ 压缩库重构为：

- 对外仅暴露 `C` 接口。
- 对外只提供一个公共头文件（建议名：`pzip.h`）。
- 支持并行压缩。
- 压缩算法由外部注入（插件/回调方式）。
- 支持 `Windows / macOS / Linux`。
- 代码风格遵循 Google C/C++ Style。

本阶段优先交付架构设计与规范，不直接承诺一次性完成全部实现。

## 2. 总体架构

采用三层架构，确保 API 稳定、内部可演进。

1. `Public C API Layer`（稳定 ABI）
2. `Core Engine Layer`（任务调度、并行流水线、I/O）
3. `Codec Adapter Layer`（外部压缩算法接入）

### 2.1 层次职责

- Public C API Layer:
  - 提供句柄化 API（`pzip_ctx_t*`）。
  - 负责参数校验、错误码映射、生命周期管理。
  - 不暴露 STL、异常、模板、类定义。
- Core Engine Layer:
  - 文件枚举、任务拆分、线程池调度、结果归并、ZIP 容器写入。
  - 提供统一 `Job` 模型，支持顺序模式和并行模式。
- Codec Adapter Layer:
  - 接入外部算法（zstd/lz4/deflate/custom）。
  - 定义统一算法函数签名，屏蔽算法差异。

### 2.2 并行模型

采用「读-压-写」三级流水线：

1. Reader 线程：读取文件块，生成 `InputChunk`。
2. Worker 线程池：调用外部算法压缩，生成 `CompressedChunk`。
3. Writer 单线程：按 entry 顺序写 ZIP 本体与目录，保证格式正确。

设计原则：

- 同一文件内部可按 chunk 并行（可配置是否启用）。
- 多文件天然并行（默认优先）。
- 写阶段保持顺序一致性，避免中央目录错乱。
- 所有共享状态仅在核心层集中管理，不把并发细节泄漏到 C API。

### 2.3 默认算法与外部算法

- 库内置 `zlib`、`zstd`、`lz4` 编解码能力。
- 未显式设置 codec 时，默认使用 zstd（ZIP method=93）。
- 通过 `pzip_set_codec` 可注入外部算法并覆盖默认 codec。
- 可通过 `pzip_make_default_zlib_codec` 获取默认 zlib 的 codec 函数表。
- 已提供 `pzip_make_default_zstd_codec`，可显式切换到 zstd（ZIP method=93）。
- 已提供 `pzip_make_default_lz4_codec`，使用 `pzip` 自定义 method 写入归档；可通过 C API 或示例 CLI 显式启用。
- `lz4` 当前为 `pzip` 自定义 method，`libpzip` 可读写，但不承诺与通用 ZIP 工具互操作。

### 2.4 加密功能预留设计

- 已在 C API 预留加密函数表与配置接口：
  - `pzip_encryption_vtable_t`
  - `pzip_set_encryption(...)`
  - `pzip_set_encryption_enabled(...)`
  - `pzip_encryption_config_t`
  - `pzip_set_encryption_config(...)`
  - `pzip_get_encryption_config(...)`
- 当前版本仅预留接口，不启用实际加密流程。
- 当开启加密且无实现时，运行阶段返回 `PZIP_E_NOT_SUPPORTED`。
- 后续计划优先基于 `third_party/boringssl` 实现加密适配层。

## 3. 公共 C API 设计（单头文件）

公共头文件建议：`include/pzip.h`

### 3.1 设计原则

- 纯 C99 兼容接口。
- 稳定 ABI：避免在公开结构体中暴露可变字段。
- 通过不透明句柄（opaque handle）访问上下文。
- 所有 API 返回统一错误码 `pzip_status_t`。
- 禁止跨边界抛 C++ 异常。

### 3.2 宏与导出约定

```c
/* pzip.h */
#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
  #if defined(PZIP_BUILD_SHARED)
    #if defined(PZIP_IMPLEMENTATION)
      #define PZIP_API __declspec(dllexport)
    #else
      #define PZIP_API __declspec(dllimport)
    #endif
  #else
    #define PZIP_API
  #endif
#else
  #define PZIP_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ... C API ... */

#ifdef __cplusplus
}
#endif
```

### 3.3 核心类型与回调

```c
typedef struct pzip_ctx pzip_ctx_t;

typedef enum pzip_status {
  PZIP_OK = 0,
  PZIP_E_INVALID_ARG = -22,
  PZIP_E_NO_MEMORY = -12,
  PZIP_E_IO = -5,
  PZIP_E_INTERNAL = -10000,
  PZIP_E_CODEC = -10001,
  PZIP_E_NOT_SUPPORTED = -10002
} pzip_status_t;

typedef struct pzip_buffer {
  const uint8_t* data;
  size_t size;
} pzip_buffer_t;

typedef struct pzip_codec_vtable {
  uint32_t abi_version;
  void* (*create)(void* user);
  void (*destroy)(void* codec_ctx);
  pzip_status_t (*compress)(
      void* codec_ctx,
      const uint8_t* src,
      size_t src_size,
      uint8_t* dst,
      size_t dst_cap,
      size_t* dst_size);
  size_t (*bound)(void* codec_ctx, size_t src_size);
  const char* (*name)(void* codec_ctx);
} pzip_codec_vtable_t;

typedef struct pzip_options {
  uint32_t abi_version;
  uint32_t thread_count;      /* 0=auto */
  uint32_t chunk_size_kb;     /* default 256 */
  uint32_t enable_solid_mode; /* 0/1 */
  uint32_t reserve[12];       /* 前向兼容 */
} pzip_options_t;
```

### 3.4 API 草案

```c
PZIP_API pzip_status_t pzip_create(
    const pzip_options_t* options,
    pzip_ctx_t** out_ctx);

PZIP_API pzip_status_t pzip_destroy(pzip_ctx_t* ctx);

PZIP_API pzip_status_t pzip_set_codec(
    pzip_ctx_t* ctx,
    const pzip_codec_vtable_t* codec,
    void* codec_user);

PZIP_API pzip_status_t pzip_open_archive(
    pzip_ctx_t* ctx,
    const char* archive_path);

PZIP_API pzip_status_t pzip_add_path(
    pzip_ctx_t* ctx,
    const char* src_path,
    const char* entry_prefix);

PZIP_API pzip_status_t pzip_run(pzip_ctx_t* ctx);

PZIP_API pzip_status_t pzip_cancel(pzip_ctx_t* ctx);

PZIP_API pzip_status_t pzip_close_archive(pzip_ctx_t* ctx);

PZIP_API pzip_status_t pzip_last_error(
    const pzip_ctx_t* ctx,
    int32_t* code,
    const char** message);

PZIP_API const char* pzip_version_string(void);
```

### 3.5 ABI/兼容约束

- `pzip_options_t`、`pzip_codec_vtable_t` 必须包含 `abi_version`。
- 通过 `reserve[]` 保留扩展位，避免破坏 ABI。
- 新增 API 只能追加，禁止修改既有函数签名。
- 同主版本内二进制兼容（`1.x` 兼容 `1.0`）。

## 4. 并行执行细则

### 4.1 任务拆分

- 输入单元：`FileTask`。
- 执行单元：`ChunkTask`（包含文件 ID、chunk 索引、偏移、长度）。
- 输出单元：`WriteTask`（包含顺序号和压缩数据）。

### 4.2 调度策略

- 线程池采用工作窃取或 MPMC 队列。
- 默认 `thread_count = min(逻辑核数, 16)`。
- 小文件合并策略：小于阈值（例如 32KB）按批处理减少调度开销。

### 4.3 内存策略

- 使用对象池/缓冲池复用 chunk buffer。
- 全局内存上限可配（例如默认 256MB）。
- 超限时启用背压：Reader 降速或阻塞，防止 OOM。

### 4.4 可取消与超时

- `pzip_cancel` 设置原子取消标志。
- Worker 在 chunk 边界检查取消状态。
- API 可扩展超时参数（v2），v1 先实现取消。

### 4.5 当前实现状态（2026-04）

- 已实现 Reader 流式分块读取（`chunk_size_kb`），不再要求整文件预读到单个大缓冲。
- 已实现有界队列 + 聚合区内存背压：当待聚合 chunk 总字节超限时，Worker 阻塞等待释放。
- 已实现聚合分片锁（sharded pending map）和“同文件续块放行”策略，避免低内存上限下的聚合自锁。
- 已实现 Writer 顺序写保证：Worker 可乱序完成，但归档写入按文件索引顺序输出。
- 当前仍为“文件级压缩输出”：同一文件的 chunk 先聚合，再生成单个 `WriteTask` 进入写阶段。
- 已落地取消路径：`pzip_cancel` 可中断运行中任务，`pzip_run` 返回 `PZIP_E_CANCELED`。
- 已落地故障注入测试能力：`ZipWriter` 支持测试专用环境变量
  `PZIP_TEST_FAIL_WRITE_AFTER_BYTES`（默认关闭，仅用于测试写入中途失败路径）。
- 已落地的关键回归用例（CTest）：
  - `pzip_parallel_pipeline`
  - `pzip_cancel_pipeline`
  - `pzip_codec_failure`
  - `pzip_archive_open_failure`
  - `pzip_backpressure_progress`
  - `pzip_writer_mid_failure`

## 5. 跨平台要求

### 5.1 平台抽象

必须引入 `platform/` 抽象层，封装：

- 文件遍历
- 路径处理（分隔符、大小写敏感差异）
- 内存映射（可选）
- 时间戳与权限元数据
- 线程命名与 CPU 信息

### 5.2 编译器矩阵

- Windows: MSVC 2022（`/std:c++17`）
- macOS: AppleClang 15+
- Linux: GCC 11+、Clang 15+

### 5.3 构建系统

- CMake >= 3.20
- 产物：
  - `libpzip.a` / `libpzip.so` / `libpzip.dylib`
  - `pzip.dll + pzip.lib`
- 安装导出：`install(TARGETS ...)` + `install(FILES include/pzip.h ...)`
- 提供 `PZIP_BUILD_SHARED`、`PZIP_BUILD_STATIC`、`PZIP_ENABLE_TESTS` 选项。

## 6. 错误处理规范

- 统一负值错误码，0 表示成功。
- 每个错误码必须可映射到稳定字符串。
- 对外禁止泄漏内部异常文本和路径敏感信息。
- 日志分级：`ERROR/WARN/INFO/DEBUG`，默认 `WARN`。

## 7. 代码规范（Google 风格落地）

- C++ 遵循 Google C++ Style Guide。
- C 接口遵循 Google C 风格：
  - 类型名后缀 `_t` 仅用于 C API 暴露类型。
  - 函数命名 `pzip_verb_object`。
  - 宏全大写，前缀 `PZIP_`。
- 头文件自包含；禁止循环依赖。
- 对外头文件禁止包含 C++ 标准库头。
- 禁止在公共 API 边界抛异常。

## 8. 测试与质量门禁

### 8.1 测试分层

- 单元测试：任务队列、缓冲池、路径规范化、错误码。
- 集成测试：目录压缩、空目录、长路径、中文文件名。
- 兼容性测试：不同外部 codec、不同线程数、不同 chunk 大小。
- 压力测试：百万小文件、超大文件（>4GB）、低内存场景。

### 8.2 必须达成指标

- 覆盖率：核心模块行覆盖率 >= 80%。
- 稳定性：主干连续 100 次回归无随机失败。
- 性能：在 8 核机器上，相对单线程性能提升 >= 3x（基准集）。

### 8.3 Sanitizer/静态检查

- Linux/macOS: ASan + UBSan + TSAN（夜间任务）。
- 全平台：clang-tidy、cppcheck（可选）、格式检查。

## 9. 安全与鲁棒性补充要求（建议纳入强制项）

以下是建议新增并强制执行的规范，解决遗留库常见问题：

1. 路径穿越防护：拒绝 `..`、绝对路径写入到压缩条目中。
2. ZIP 炸弹防护：限制压缩比上限与单文件解压上限元数据。
3. 资源配额：限制最大文件数、最大总输入字节数。
4. 明确线程安全边界：同一 `pzip_ctx_t` 不可并发调用写 API。
5. 可观察性：提供进度回调和统计回调（已处理文件数、吞吐）。
6. 可复现构建：锁定编译选项，发布产物可追溯 commit。
7. 许可证合规：外部 codec 的 license 清单与 NOTICE 自动生成。
8. Fuzz 测试：对 ZIP 头、文件名解析、边界长度做持续 fuzzing。
9. 大端/小端一致性：统一序列化逻辑，禁止依赖宿主字节序。
10. 版本治理：语义化版本（SemVer）与 ABI 检查（abi-compliance-checker）。

## 10. 目录建议

```text
include/
  pzip.h                 # 唯一对外头文件
src/
  api/                   # C API 实现（参数检查、错误码）
  core/                  # 调度、流水线、任务模型
  io/                    # 文件与路径处理
  codec/                 # 外部算法适配
  platform/              # 平台抽象层
  zip/                   # zip 格式读写
tests/
  unit/
  integration/
  stress/
cmake/
  PzipOptions.cmake
  PzipWarnings.cmake
```

## 11. 迁移计划（建议）

1. M1: 冻结旧接口，建立新 `pzip.h` + opaque handle 骨架。
2. M2: 接入线程池与 chunk 流水线，先支持单算法适配器。
3. M3: 完成外部 codec vtable，支持至少 2 种算法实现。
4. M4: 完成跨平台 CI 与全量测试门禁。
5. M5: 性能调优与 ABI 冻结，发布 `v1.0.0`。

## 12. 验收标准

- 用户只需包含 `pzip.h` 即可调用库功能。
- 不依赖 C++ ABI 即可从 C/Go/Rust/Python FFI 调用。
- Windows/macOS/Linux 三平台 CI 全绿。
- 并行压缩在标准基准上达到目标加速比。
- 外部算法可独立替换，无需修改核心调度层。

## 13. 第三方依赖确认机制

为控制供应链风险与跨平台一致性，三方依赖引入必须执行“先确认、后实现”流程。

### 13.1 当前已确认依赖

- `third_party/filesystem`（`gulrak/filesystem`）
  - 用途：C++11 下提供跨平台文件系统能力。
  - 暴露策略：仅内部实现使用，对外 `pzip.h` 不暴露。
- `third_party/zlib`
  - 用途：默认压缩/解压算法实现（deflate/inflate）。
  - 暴露策略：仅通过 C API 行为体现，不暴露 zlib 头文件到公共接口。

### 13.2 新增依赖准入规则

- 新增依赖前必须先确认：
  - 许可证类型与兼容性（MIT/BSD/Zlib 等）
  - 支持平台（Windows/macOS/Linux）
  - 构建方式（源码内置或系统包）
  - 版本锁定策略（tag/commit）
  - 对外 ABI 与头文件暴露影响
- 若未完成确认，不得合入主分支。

### 13.3 CMake 依赖策略

- 默认使用仓库内依赖（`third_party`）保证可重复构建。
- 对可选依赖提供开关（如 `PZIP_USE_BUNDLED_ZLIB`）以支持系统库替换。

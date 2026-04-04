# 并行压缩库工程规范清单（Checklist）

> 本文用于项目执行与评审打勾，默认关键字含义：
>
> - `MUST`: 必须满足，否则不可发布
> - `SHOULD`: 推荐满足，可在版本说明中记录例外
> - `MAY`: 可选增强项

## 1. API 与兼容性

- [ ] `MUST` 对外只暴露一个头文件：`include/pzip.h`
- [ ] `MUST` 对外 API 全部为 C 接口（`extern "C"`）
- [ ] `MUST` 对外不暴露 C++ 类型、STL 容器、异常
- [ ] `MUST` 使用 opaque handle（`pzip_ctx_t`）
- [ ] `MUST` 所有公开结构体含 `abi_version`
- [ ] `MUST` 公开结构体保留 `reserve[]` 扩展位
- [ ] `MUST` 同主版本 ABI 兼容
- [ ] `SHOULD` 提供 `pzip_get_abi_version()`

## 2. 并行与性能

- [ ] `MUST` 支持多文件并行压缩
- [ ] `MUST` 支持可配置线程数（0=auto）
- [ ] `MUST` 写阶段保证 ZIP 结构顺序正确
- [ ] `MUST` 支持取消（`pzip_cancel`）
- [ ] `MUST` 设置默认内存上限，避免无限膨胀
- [ ] `SHOULD` 支持 chunk 级并行
- [ ] `SHOULD` 提供性能基准工具与基准数据集

## 3. 外部算法接入

- [ ] `MUST` 默认提供 zlib 压缩与解压算法
- [ ] `MUST` 通过 vtable/函数表注入压缩算法
- [ ] `MUST` 支持算法上下文 create/destroy
- [ ] `MUST` 支持 `bound()` 预估输出上限
- [ ] `MUST` 算法错误统一映射到 `pzip_status_t`
- [ ] `SHOULD` 内置至少一个示例适配器（如 deflate）
- [ ] `SHOULD` 提供算法接入示例文档

## 4. 跨平台与构建

- [ ] `MUST` 支持 Linux / macOS / Windows
- [ ] `MUST` CMake >= 3.20
- [ ] `MUST` 支持静态库和动态库构建
- [ ] `MUST` 控制符号可见性（默认隐藏，仅导出 API）
- [ ] `MUST` 通过 CI 覆盖三平台编译和测试
- [ ] `SHOULD` 提供包管理配置（vcpkg/homebrew/conan 之一）

## 5. 质量与测试

- [ ] `MUST` 核心模块单元测试覆盖率 >= 80%
- [ ] `MUST` 集成测试覆盖：空目录、长路径、超大文件、非 ASCII 文件名
- [ ] `MUST` 并发一致性测试（不同线程数结果一致）
- [ ] `MUST` 回归测试稳定（连续多轮无随机失败）
- [ ] `SHOULD` 启用 ASan/UBSan
- [ ] `SHOULD` 夜间任务启用 TSAN
- [ ] `SHOULD` 引入 fuzz 测试入口

## 6. 安全与合规

- [ ] `MUST` 防路径穿越（拒绝 `..` 和绝对路径条目）
- [ ] `MUST` 限制最大文件数/总输入大小
- [ ] `MUST` 对异常输入返回明确错误码
- [ ] `MUST` 不在错误文本泄漏敏感路径/密钥
- [ ] `MUST` 建立第三方依赖 license 清单
- [ ] `SHOULD` 发布前执行依赖漏洞扫描

## 7. 代码规范与流程

- [ ] `MUST` 遵循 Google C/C++ Style
- [ ] `MUST` 成员函数使用小驼峰命名
- [ ] `MUST` 成员变量使用 `m_` + 小驼峰命名
- [ ] `MUST` 静态成员函数/全局函数使用大驼峰命名
- [ ] `MUST` 开启警告并将关键警告视为错误
- [ ] `MUST` 提交前通过格式化与静态检查
- [ ] `MUST` 所有 PR 需通过 CI 才可合并
- [ ] `SHOULD` PR 模板包含性能影响与兼容性影响说明
- [ ] `SHOULD` 关键模块变更必须附带 benchmark 对比

## 8. 发布管理

- [ ] `MUST` 使用 SemVer
- [ ] `MUST` 维护 `CHANGELOG.md`
- [ ] `MUST` 发布包包含头文件、库文件、许可证
- [ ] `MUST` 提供最小可运行示例（C）
- [ ] `SHOULD` 维护 LTS 分支策略

# libpzip 性能分析指引

本文档整理了 `libpzip` 当前用于定位压缩性能瓶颈的常用命令、推荐执行顺序，以及如何根据 `perf` 输出判断瓶颈主要落在压缩、同步还是 I/O。

## 1. 目标

在评估如下问题时使用本文档：

- `BlockingQueue` 是否是主要瓶颈。
- 是否值得替换为 `moodycamel::ConcurrentQueue`。
- 当前耗时主要在压缩、I/O 还是线程同步。
- 优化优先级应放在 codec、CRC、内存搬运还是队列实现。

## 2. 测试前提

为避免噪声，建议满足以下条件：

- 使用 `Release` 构建。
- 在同一台机器上重复测试。
- 尽量关闭其它高负载任务。
- 尽量选择耗时至少 `3` 到 `10` 秒的数据集，避免超短任务导致采样抖动。
- 若需要比较纯 CPU 瓶颈，可把输入放在内存盘；若需要评估真实体验，则使用真实磁盘。

## 3. 推荐命令顺序

建议按以下顺序执行。

### 3.1 总体性能概览

先看整体 CPU 利用率、上下文切换、IPC、缓存失效率。

```bash
sudo perf stat -d -- ./pzip-zip music.zip /mnt/disk/Music/ -r
```

若要针对当前构建目录做快速试验，可使用：

```bash
sudo perf stat -d -- ./pzip-zip build.zip ./build -r
```

关注这些指标：

- `CPUs utilized`: 并行度是否有效。
- `context-switches`: 是否存在较明显的锁竞争或频繁阻塞。
- `instructions per cycle`: CPU 执行效率。
- `stalled-cycles-frontend/backend`: 前后端是否存在明显停顿。
- `L1-dcache-load-misses`: 是否存在较重的缓存局部性问题。

### 3.2 函数热点分析

当需要知道“时间到底花在什么函数上”时，执行：

```bash
sudo perf record -F 999 -g --call-graph dwarf -- ./pzip-zip build.zip ./build -r
sudo perf report --stdio | head -120
```

如果只想筛出关键符号：

```bash
sudo perf report --stdio | grep -E "BlockingQueue|futex|pthread_cond|longest_match|deflate_slow|Crc32|writeEntry|writev"
```

常见判断方式：

- `longest_match`、`deflate_slow` 很高：压缩算法本身是主瓶颈。
- `BlockingQueue`、`pthread_cond_wait`、`futex` 很高：队列或同步等待开销偏大。
- `writev`、`vfs_writev`、`ext4_*` 很高：写磁盘路径更重。
- `read`、`page_fault`、文件系统函数很高：读路径或页缓存行为值得关注。

### 3.3 等待与同步开销分析

当怀疑线程同步、条件变量或 futex 导致吞吐下降时，执行：

```bash
sudo perf record -e sched:sched_switch,futex:* -g -- ./pzip-zip build.zip ./build -r
sudo perf report --stdio | head -120
```

重点关注：

- `__x64_sys_futex`
- `do_futex`
- `futex_wait`
- `pthread_cond_wait`
- `pthread_cond_signal`

如果这些符号只占很小比例，通常说明同步不是端到端主瓶颈。

## 4. 输出解读

### 4.1 如何判断队列是否值得优化

如果满足下面任一情况，可以继续深入评估队列实现：

- `BlockingQueue`、`futex`、`pthread_cond_wait` 合计占比较高。
- `context-switches` 明显偏高。
- `CPUs utilized` 低于预期，且 worker 经常阻塞而不是计算。
- A/B 测试中替换队列后总耗时和 P95 都有稳定改善。

反之，如果热点主要集中在压缩函数，如：

- `longest_match`
- `deflate_slow`
- `compress_block`

则优先级应放在 codec 和压缩策略，而不是队列替换。

### 4.2 如何看待 `Children` 和 `Self`

`perf report` 中：

- `Self`: 当前函数本身消耗的采样占比。
- `Children`: 当前函数及其调用链下方累计占比。

解读时不要把同一调用链展开后的多行结果重复累加，否则会高估某类开销。

### 4.3 `lost chunks` 如何处理

若输出包含：

```text
Processed XXXX events and lost N chunks!
Check IO/CPU overload!
```

说明采样过程中有少量样本数据块丢失。少量丢失通常不会改变热点的大方向判断，但如果丢失很多，建议：

- 使用更长时间的数据集。
- 尽量减少系统其他负载。
- 降低采样频率，例如从 `-F 999` 调低。

## 5. 针对当前项目的经验结论

结合当前 `libpzip` 的一次实测结果：

- `longest_match` 约 `53%`
- `deflate_slow` 约 `24%`
- `Crc32` 约 `4%` 到 `5%`
- `futex` 相关约 `1%` 左右
- `pthread_cond_wait`、`pthread_cond_signal` 均在低个位数甚至更低

这类画像通常表示：

- 主要瓶颈在 `zlib deflate` 的压缩路径。
- `BlockingQueue` 不是主要瓶颈。
- 直接替换为 `moodycamel::ConcurrentQueue` 大概率不是高回报优化项。

## 6. 建议优化顺序

若热点与上述画像接近，建议按如下顺序优化：

1. 先检查 `zlib` 压缩级别是否过高。
2. 评估是否切换为更偏吞吐的 codec，例如 `zstd`。
3. 评估 `Crc32` 和内存搬运是否有重复扫描或拷贝。
4. 最后再考虑替换队列实现。

## 7. A/B 对照测试建议

如果确实要比较 `BlockingQueue` 与 `moodycamel::ConcurrentQueue`，建议固定以下变量：

- 相同输入数据集。
- 相同线程数。
- 相同 chunk 大小。
- 相同 codec 和压缩级别。
- 相同构建参数。

建议每组至少跑 `10` 次，前 `2` 次作为 warmup，并记录：

- 总耗时中位数。
- P95 耗时。
- `perf stat` 指标。
- `perf report` 关键热点占比。

如果只看到 `futex` 降了一点，但总耗时变化很小，则不建议为此引入新的并发队列依赖和适配复杂度。

## 8. 常用命令速查

```bash
# 总览 CPU / IPC / 上下文切换 / cache miss
sudo perf stat -d -- ./pzip-zip music.zip /mnt/disk/Music/ -r

# 录制热点调用栈
sudo perf record -F 999 -g --call-graph dwarf -- ./pzip-zip build.zip ./build -r

# 查看热点报告
sudo perf report --stdio | head -120

# 仅筛关键热点
sudo perf report --stdio | grep -E "BlockingQueue|futex|pthread_cond|longest_match|deflate_slow|Crc32|writeEntry|writev"

# 专门查看调度与 futex 等待
sudo perf record -e sched:sched_switch,futex:* -g -- ./pzip-zip build.zip ./build -r
sudo perf report --stdio | head -120
```

## 9. 后续扩展

若需要更精确地区分 `read`、`compress`、`encrypt`、`write` 各阶段耗时，可在 `src/core/pzip_engine.cpp` 中增加分阶段埋点，输出：

- reader 总耗时
- worker 压缩总耗时
- queue wait 总耗时
- writer 总耗时
- encryption 总耗时

这类埋点适合做项目内长期回归对比，`perf` 更适合做外部热点定位。
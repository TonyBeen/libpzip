# libpzip

libpzip 是一个以 C API 对外暴露的 ZIP 打包库，内部使用并行流水线执行读、压缩和写入。

当前内置的默认 codec：

- zlib
- zstd
- lz4

其中：

- zlib 使用标准 ZIP method 8
- zstd 使用 ZIP method 93
- lz4 使用 pzip 自定义 method，仅保证 libpzip 自身可读写，不承诺与通用 ZIP 工具互操作

## CLI 示例

构建后会生成两个示例程序：

- pzip-zip
- pzip-unzip

压缩示例：

```bash
./build/pzip-zip --codec zlib -l 6 out.zip ./input -r
./build/pzip-zip --codec zstd out.zip ./input -r
./build/pzip-zip --codec lz4 out.zip ./input -r
```

解压示例：

```bash
./build/pzip-unzip --codec zlib out.zip ./output
./build/pzip-unzip --codec zstd out.zip ./output
./build/pzip-unzip --codec lz4 out.zip ./output
```

说明：

- `-l` 仅对 `--codec zlib` 生效
- 使用 `lz4` 时，压缩和解压都应通过 `libpzip` 完成

## C API 示例

仓库内提供了几个最小示例：

- `examples/minimal_c.c`
- `examples/zstd_codec_c.c`
- `examples/lz4_codec_c.c`
- `examples/encryption_reserved_c.c`

默认 codec 工厂：

- `pzip_make_default_zlib_codec`
- `pzip_make_default_zstd_codec`
- `pzip_make_default_lz4_codec`

## 性能分析

性能 profiling 命令和结果解读见：

- `docs/perf_profiling_guide.md`
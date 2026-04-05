#ifndef PZIP_H_
#define PZIP_H_

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

#define PZIP_ABI_VERSION 1u

typedef struct pzip_ctx pzip_ctx_t;

typedef enum pzip_status {
    PZIP_OK                 = 0,
    PZIP_E_INVALID_ARG      = -22,
    PZIP_E_NO_MEMORY        = -12,
    PZIP_E_IO               = -5,
    PZIP_E_NOT_FOUND        = -2,
    PZIP_E_BUSY             = -16,
    PZIP_E_CANCELED         = -125,
    PZIP_E_INTERNAL         = -10000,
    PZIP_E_CODEC            = -10001,
    PZIP_E_NOT_SUPPORTED    = -10002
} pzip_status_t;

typedef enum pzip_codec_flags {
    PZIP_CODEC_FLAG_NONE = 0,
    PZIP_CODEC_FLAG_ZIP_COMPATIBLE = 1u << 0
} pzip_codec_flags_t;

typedef enum pzip_encryption_flags {
    PZIP_ENCRYPTION_FLAG_NONE = 0,
    PZIP_ENCRYPTION_FLAG_STREAM = 1u << 0
} pzip_encryption_flags_t;

typedef struct pzip_codec_vtable {
    uint32_t abi_version;
    uint16_t zip_method;
    uint16_t reserved0;
    uint32_t flags;
    void* (*create)(void* user);
    void (*destroy)(void* codec_ctx);
    pzip_status_t (*compress)(void* codec_ctx, const uint8_t* src, size_t src_size, uint8_t* dst,
                              size_t dst_cap, size_t* dst_size);
    pzip_status_t (*decompress)(void* codec_ctx, const uint8_t* src, size_t src_size, uint8_t* dst,
                                size_t dst_cap, size_t* dst_size);
    size_t (*bound)(void* codec_ctx, size_t src_size);
    const char* (*name)(void* codec_ctx);
} pzip_codec_vtable_t;

typedef struct pzip_encryption_vtable {
    uint32_t abi_version;
    uint32_t algorithm_id;
    uint32_t flags;
    uint32_t reserved0;
    void* (*create)(void* user);
    void (*destroy)(void* encryption_ctx);
    pzip_status_t (*encrypt)(void* encryption_ctx, const uint8_t* src, size_t src_size,
                             uint8_t* dst, size_t dst_cap, size_t* dst_size);
    pzip_status_t (*decrypt)(void* encryption_ctx, const uint8_t* src, size_t src_size,
                             uint8_t* dst, size_t dst_cap, size_t* dst_size);
    size_t (*bound)(void* encryption_ctx, size_t src_size);
    const char* (*name)(void* encryption_ctx);
} pzip_encryption_vtable_t;

typedef struct pzip_encryption_config {
    uint32_t abi_version;
    uint32_t algorithm_id;
    uint32_t key_id;
    uint32_t nonce_size;
    uint32_t aad_size;
    uint32_t reserve[8];
} pzip_encryption_config_t;

typedef struct pzip_options {
    uint32_t abi_version;           // 结构体 ABI 兼容版本
    uint32_t thread_count;          // 线程数，0 表示自动选择
    uint32_t chunk_size_kb;         // 分块大小，0 表示库默认值
    uint32_t enable_solid_mode;     // 预留的 solid 模式开关
    uint32_t max_file_count;        // 最大输入文件数
    uint64_t max_total_input_bytes; // 输入总大小上限
    uint32_t reserve[10];
} pzip_options_t;

PZIP_API pzip_status_t pzip_create(const pzip_options_t* options, pzip_ctx_t** out_ctx);

PZIP_API pzip_status_t pzip_destroy(pzip_ctx_t* ctx);

PZIP_API pzip_status_t pzip_set_codec(pzip_ctx_t* ctx, const pzip_codec_vtable_t* codec,
                                      void* codec_user);

PZIP_API pzip_status_t pzip_make_default_zlib_codec(pzip_codec_vtable_t* out_codec);

PZIP_API pzip_status_t pzip_make_default_zstd_codec(pzip_codec_vtable_t* out_codec);

PZIP_API pzip_status_t pzip_make_default_lz4_codec(pzip_codec_vtable_t* out_codec);

PZIP_API pzip_status_t pzip_set_encryption(pzip_ctx_t* ctx,
                                           const pzip_encryption_vtable_t* encryption,
                                           void* encryption_user);

PZIP_API pzip_status_t pzip_set_encryption_enabled(pzip_ctx_t* ctx, uint32_t enabled);

PZIP_API pzip_status_t pzip_set_encryption_config(pzip_ctx_t* ctx,
                                                  const pzip_encryption_config_t* config);

PZIP_API pzip_status_t pzip_get_encryption_config(const pzip_ctx_t* ctx,
                                                  pzip_encryption_config_t* out_config);

PZIP_API pzip_status_t pzip_open_archive(pzip_ctx_t* ctx, const char* archive_path);

PZIP_API pzip_status_t pzip_add_path(pzip_ctx_t* ctx, const char* src_path,
                                     const char* entry_prefix);

PZIP_API pzip_status_t pzip_run(pzip_ctx_t* ctx);

PZIP_API pzip_status_t pzip_extract_archive(pzip_ctx_t* ctx, const char* archive_path,
                                            const char* output_dir);

PZIP_API pzip_status_t pzip_cancel(pzip_ctx_t* ctx);

PZIP_API pzip_status_t pzip_close_archive(pzip_ctx_t* ctx);

PZIP_API pzip_status_t pzip_last_error(const pzip_ctx_t* ctx, int32_t* code, const char** message);

PZIP_API uint32_t pzip_get_abi_version(void);

PZIP_API const char* pzip_version_string(void);

#ifdef __cplusplus
}
#endif

#endif  // PZIP_H_

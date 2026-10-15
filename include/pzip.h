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

// Standard ZIP password crypto mode.
typedef enum pzip_crypto_mode {
    PZIP_CRYPTO_NONE = 0,       // No password encryption
    PZIP_CRYPTO_ZIPCRYPTO = 1,  // Traditional PKWARE ZipCrypto (widest compatibility)
    PZIP_CRYPTO_AES256 = 2,     // WinZip AES-256 (industry standard strong encryption, default)
    PZIP_CRYPTO_AES128 = 3      // WinZip AES-128
} pzip_crypto_mode_t;

/**
 * @brief 归档打开工作模式枚举。
 */
typedef enum pzip_open_mode {
    /** @brief 默认模式：创建新归档；若归档已存在则全量截断并覆盖写入。 */
    PZIP_MODE_CREATE    = 0,
    /** @brief 追加模式：在现有归档尾部追加新文件；若归档不存在则自动创建。 */
    PZIP_MODE_APPEND    = 1,
    /** @brief 编辑模式：支持对已有归档进行文件增、删、改（通过安全流拷贝重打包回收物理空间）。 */
    PZIP_MODE_MODIFY    = 2
} pzip_open_mode_t;

/**
 * @brief ZIP 归档内部条目的元数据信息。
 */
typedef struct pzip_entry_info {
    const char* entry_name;         /**< @brief 条目的相对路径名称 (UTF-8 编码) */
    uint64_t    uncompressed_size;  /**< @brief 条目的原始未压缩字节大小 */
    uint64_t    compressed_size;    /**< @brief 条目在包内的物理压缩字节大小 */
    uint32_t    crc32;              /**< @brief 条目原始数据的 CRC-32 校验码 */
    uint16_t    method;             /**< @brief 压缩算法 (0=Store, 8=Deflate, 99=WinZip AES 等) */
    uint16_t    flags;              /**< @brief 通用位标志 (General Purpose Bit Flags) */
    uint32_t    dos_time;           /**< @brief MS-DOS 格式最后修改时间 */
    uint32_t    dos_date;           /**< @brief MS-DOS 格式最后修改日期 */
    int32_t     is_encrypted;       /**< @brief 该条目是否受密码保护加密 (1: 是, 0: 否) */
    int32_t     is_directory;       /**< @brief 该条目是否为目录节点 (1: 是, 0: 否) */
} pzip_entry_info_t;

/**
 * @brief 归档条目枚举遍历的回调函数原型。
 * 
 * @param[in] entry      当前条目的只读元数据指针。指针在回调执行期间有效。
 * @param[in] user_data  用户注册的上下文透传指针。
 * 
 * @return int32_t       返回 0 表示继续枚举后续条目；返回非 0 表示立即提前终止枚举。
 */
typedef int32_t (*pzip_list_callback_t)(const pzip_entry_info_t* entry, void* user_data);

// Codec vtable interface.
// In multithreaded pipelines, if `create` is provided, each worker thread calls `create(user)`
// to instantiate a thread-local codec context, and `destroy(codec_ctx)` on thread exit.
// If `create` is NULL (stateless codec), `codec_ctx` passed to `compress`/`decompress`/`bound`
// will be NULL, and the callback implementations must be reentrant and thread-safe.
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

// Progress callback signature.
// `bytes_processed`: Number of uncompressed bytes processed/written so far.
// `total_bytes`: Total number of uncompressed bytes across all input files.
// `user_data`: User pointer provided during registration.
typedef void (*pzip_progress_callback_t)(uint64_t bytes_processed, uint64_t total_bytes, void* user_data);

// Engine configuration options.
// Note: Callers MUST zero-initialize this struct (e.g. via memset) before populating fields
// to ensure forward compatibility when the struct expands.
typedef struct pzip_options {
    uint32_t abi_version;           // Struct ABI compatibility version.
    uint32_t thread_count;          // Worker thread count, 0 means auto-select.
    uint32_t chunk_size_kb;         // Chunk size in KiB, 0 means library default.
    uint32_t enable_solid_mode;     // Reserved solid-mode switch.
    uint32_t max_file_count;        // Maximum number of input files.
    uint64_t max_total_input_bytes; // Upper limit for total input bytes.
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

// Registers a progress callback for compression and extraction operations.
PZIP_API pzip_status_t pzip_set_progress_callback(pzip_ctx_t* ctx,
                                                  pzip_progress_callback_t callback,
                                                  void* user_data);

// Sets standard password for ZIP archive creation and extraction.
// Setting a non-empty password automatically enables password protection
// using PZIP_CRYPTO_AES256 by default (unless another mode was set).
// Passing NULL or "" clears the password and disables password protection.
PZIP_API pzip_status_t pzip_set_password(pzip_ctx_t* ctx, const char* password);

// Selects the standard ZIP encryption algorithm (AES-256 or ZipCrypto).
PZIP_API pzip_status_t pzip_set_crypto_mode(pzip_ctx_t* ctx, pzip_crypto_mode_t mode);

/**
 * @brief 以指定模式打开或创建 ZIP 归档。
 * 
 * @param[in,out] ctx          PZIP 上下文句柄。
 * @param[in]     archive_path ZIP 归档文件的物理磁盘路径 (UTF-8 编码)。
 * @param[in]     mode         打开工作模式 (PZIP_MODE_CREATE / PZIP_MODE_APPEND / PZIP_MODE_MODIFY)。
 * 
 * @return pzip_status_t       PZIP_OK 成功；参数非法返回 PZIP_E_INVALID_ARG；打开失败返回 PZIP_E_IO。
 */
PZIP_API pzip_status_t pzip_open_archive_mode(pzip_ctx_t* ctx, 
                                              const char* archive_path, 
                                              pzip_open_mode_t mode);

/**
 * @brief 标记从已有 ZIP 归档中删除指定条目。
 * 
 * @note 实际物理删除与空间回收将在调用 pzip_run() 时以事务原子方式执行。
 * 
 * @param[in,out] ctx         PZIP 上下文句柄。
 * @param[in]     entry_name  待删除的条目路径名称 (UTF-8 编码)。
 * @param[in]     recursive   递归开关：
 *                            - 0: 精确匹配文件，仅删除与 entry_name 全等匹配的单个文件条目；
 *                            - 非0: 目录匹配，递归删除该目录自身以及其下的所有子文件和子目录。
 * 
 * @return pzip_status_t      PZIP_OK 成功标记；参数非法返回 PZIP_E_INVALID_ARG。
 */
PZIP_API pzip_status_t pzip_remove_entry(pzip_ctx_t* ctx, 
                                         const char* entry_name,
                                         int32_t recursive);

/**
 * @brief 在已有 ZIP 归档内重命名或移动指定条目。
 * 
 * @note 该操作在重打包时直接修改索引与局部头，零解压、零重压缩，耗时毫秒级。
 * 
 * @param[in,out] ctx            PZIP 上下文句柄。
 * @param[in]     old_entry_name 原条目路径名称 (UTF-8 编码)。
 * @param[in]     new_entry_name 新条目目标路径名称 (UTF-8 编码)。
 * 
 * @return pzip_status_t         PZIP_OK 成功标记；参数非法返回 PZIP_E_INVALID_ARG。
 */
PZIP_API pzip_status_t pzip_rename_entry(pzip_ctx_t* ctx,
                                         const char* old_entry_name,
                                         const char* new_entry_name);

/**
 * @brief 探测指定条目是否存在于已打开的归档中。
 * 
 * @param[in]  ctx         PZIP 上下文句柄。
 * @param[in]  entry_name  待检索的条目路径 (UTF-8 编码)。
 * @param[out] out_exists  输出结果：1 表示存在，0 表示不存在。
 * 
 * @return pzip_status_t   PZIP_OK 成功查询；未打开归档返回 PZIP_E_BUSY 或 PZIP_E_INVALID_ARG。
 */
PZIP_API pzip_status_t pzip_has_entry(const pzip_ctx_t* ctx, 
                                      const char* entry_name, 
                                      int32_t* out_exists);

/**
 * @brief 获取已打开归档中指定条目的详细元数据信息。
 * 
 * @param[in]  ctx         PZIP 上下文句柄。
 * @param[in]  entry_name  待查询的条目路径 (UTF-8 编码)。
 * @param[out] out_info    用于接收条目元数据的结构体指针。
 * 
 * @return pzip_status_t   PZIP_OK 成功；未找到该条目返回 PZIP_E_NOT_FOUND。
 */
PZIP_API pzip_status_t pzip_stat_entry(const pzip_ctx_t* ctx, 
                                       const char* entry_name, 
                                       pzip_entry_info_t* out_info);

/**
 * @brief 获取当前归档中已存在的条目总数量。
 * 
 * @param[in]  ctx        PZIP 上下文句柄。
 * @param[out] out_count  输出条目数量。
 * 
 * @return pzip_status_t  PZIP_OK 成功；未打开归档返回 PZIP_E_INVALID_ARG。
 */
PZIP_API pzip_status_t pzip_get_entry_count(const pzip_ctx_t* ctx, 
                                            uint32_t* out_count);

/**
 * @brief 枚举已打开归档中的所有条目。
 * 
 * @param[in] ctx        PZIP 上下文句柄。
 * @param[in] callback   遍历每项条目时调用的回调函数指针。
 * @param[in] user_data  用户透传指针。
 * 
 * @return pzip_status_t PZIP_OK 成功完成遍历；回调中止返回 PZIP_OK；I/O 错误返回 PZIP_E_IO。
 */
PZIP_API pzip_status_t pzip_list_entries(const pzip_ctx_t* ctx, 
                                         pzip_list_callback_t callback, 
                                         void* user_data);

/**
 * @brief 静态快速只读枚举指定磁盘 ZIP 文件的条目列表（轻量级，无需创建完整引擎实例）。
 * 
 * @param[in] archive_path 目标 ZIP 文件的物理磁盘路径。
 * @param[in] callback     遍历每项条目时的回调函数。
 * @param[in] user_data    用户透传指针。
 * 
 * @return pzip_status_t   PZIP_OK 成功；解析失败或打开错误返回相应错误码。
 */
PZIP_API pzip_status_t pzip_list_archive(const char* archive_path, 
                                         pzip_list_callback_t callback, 
                                         void* user_data);

/**
 * @brief 从 ZIP 归档中按需精准提取单个条目至目标磁盘文件。
 * 
 * @param[in,out] ctx          PZIP 上下文句柄 (若条目加密，需已预先配置好密码)。
 * @param[in]     archive_path ZIP 归档路径。
 * @param[in]     entry_name   待解压的条目路径名称。
 * @param[in]     output_file  解压输出的目标物理磁盘文件路径。
 * 
 * @return pzip_status_t       PZIP_OK 成功提取；密码错误返回 PZIP_E_CODEC；条目不存在返回 PZIP_E_NOT_FOUND。
 */
PZIP_API pzip_status_t pzip_extract_entry(pzip_ctx_t* ctx,
                                          const char* archive_path,
                                          const char* entry_name,
                                          const char* output_file);

/**
 * @brief 清空当前上下文中已暂存的所有待添加路径及待删除/重命名操作。
 * 
 * @param[in,out] ctx        PZIP 上下文句柄。
 * 
 * @return pzip_status_t     PZIP_OK 成功重置。
 */
PZIP_API pzip_status_t pzip_clear_pending(pzip_ctx_t* ctx);

PZIP_API pzip_status_t pzip_close_archive(pzip_ctx_t* ctx);


// Retrieves the last error code and descriptive message string.
// Note: The returned `*message` pointer points to an internal buffer and remains valid
// until the next libpzip API call on the same context.
PZIP_API pzip_status_t pzip_last_error(const pzip_ctx_t* ctx, int32_t* code, const char** message);

PZIP_API uint32_t pzip_get_abi_version(void);

PZIP_API const char* pzip_version_string(void);

#ifdef __cplusplus
}
#endif

#endif  // PZIP_H_

#include <new>

#include "codec/lz4_codec.h"
#include "codec/zlib_codec.h"
#include "codec/zstd_codec.h"
#include "core/pzip_engine.h"
#include "pzip.h"
#include "zip/zip_reader.h"

namespace {
static const char* kVersion = "1.0.0";
}

struct pzip_ctx {
    pzip::PzipEngine m_engine;
};

extern "C" {

PZIP_API pzip_status_t pzip_create(const pzip_options_t* options, pzip_ctx_t** out_ctx) {
    if (out_ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    *out_ctx = NULL;

    pzip_ctx_t* ctx = new (std::nothrow) pzip_ctx_t();
    if (ctx == NULL) {
        return PZIP_E_NO_MEMORY;
    }

    const pzip_status_t status = ctx->m_engine.configure(options);
    if (status != PZIP_OK) {
        delete ctx;
        return status;
    }

    *out_ctx = ctx;
    return PZIP_OK;
}

PZIP_API pzip_status_t pzip_destroy(pzip_ctx_t* ctx) {
    if (ctx == NULL) {
        return PZIP_OK;
    }
    delete ctx;
    return PZIP_OK;
}

PZIP_API pzip_status_t pzip_set_codec(pzip_ctx_t* ctx, const pzip_codec_vtable_t* codec,
                                      void* codec_user) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.setCodec(codec, codec_user);
}

PZIP_API pzip_status_t pzip_make_default_zlib_codec(pzip_codec_vtable_t* out_codec) {
    if (out_codec == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    *out_codec = pzip::codec::CreateZlibCodecVtable();
    return PZIP_OK;
}

PZIP_API pzip_status_t pzip_make_default_zstd_codec(pzip_codec_vtable_t* out_codec) {
    if (out_codec == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    *out_codec = pzip::codec::CreateZstdCodecVtable();
    return PZIP_OK;
}

PZIP_API pzip_status_t pzip_make_default_lz4_codec(pzip_codec_vtable_t* out_codec) {
    if (out_codec == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    *out_codec = pzip::codec::CreateLz4CodecVtable();
    return PZIP_OK;
}

PZIP_API pzip_status_t pzip_set_encryption(pzip_ctx_t* ctx,
                                           const pzip_encryption_vtable_t* encryption,
                                           void* encryption_user) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.setEncryption(encryption, encryption_user);
}

PZIP_API pzip_status_t pzip_set_encryption_enabled(pzip_ctx_t* ctx, uint32_t enabled) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.setEncryptionEnabled(enabled);
}

PZIP_API pzip_status_t pzip_set_encryption_config(pzip_ctx_t* ctx,
                                                  const pzip_encryption_config_t* config) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.setEncryptionConfig(config);
}

PZIP_API pzip_status_t pzip_get_encryption_config(const pzip_ctx_t* ctx,
                                                  pzip_encryption_config_t* out_config) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.getEncryptionConfig(out_config);
}

PZIP_API pzip_status_t pzip_open_archive(pzip_ctx_t* ctx, const char* archive_path) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.openArchive(archive_path);
}

PZIP_API pzip_status_t pzip_add_path(pzip_ctx_t* ctx, const char* src_path,
                                     const char* entry_prefix) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.addPath(src_path, entry_prefix);
}

PZIP_API pzip_status_t pzip_run(pzip_ctx_t* ctx) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.run();
}

PZIP_API pzip_status_t pzip_extract_archive(pzip_ctx_t* ctx, const char* archive_path,
                                            const char* output_dir) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.extractArchive(archive_path, output_dir);
}

PZIP_API pzip_status_t pzip_cancel(pzip_ctx_t* ctx) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.cancel();
}

PZIP_API pzip_status_t pzip_set_progress_callback(pzip_ctx_t* ctx,
                                                  pzip_progress_callback_t callback,
                                                  void* user_data) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.setProgressCallback(callback, user_data);
}

PZIP_API pzip_status_t pzip_set_password(pzip_ctx_t* ctx, const char* password) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.setPassword(password);
}

PZIP_API pzip_status_t pzip_set_crypto_mode(pzip_ctx_t* ctx, pzip_crypto_mode_t mode) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.setCryptoMode(mode);
}

PZIP_API pzip_status_t pzip_close_archive(pzip_ctx_t* ctx) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.closeArchive();
}

PZIP_API pzip_status_t pzip_open_archive_mode(pzip_ctx_t* ctx, const char* archive_path,
                                              pzip_open_mode_t mode) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.openArchiveMode(archive_path, mode);
}

PZIP_API pzip_status_t pzip_remove_entry(pzip_ctx_t* ctx, const char* entry_name,
                                         int32_t recursive) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.removeEntry(entry_name, recursive);
}

PZIP_API pzip_status_t pzip_rename_entry(pzip_ctx_t* ctx, const char* old_entry_name,
                                         const char* new_entry_name) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.renameEntry(old_entry_name, new_entry_name);
}

PZIP_API pzip_status_t pzip_has_entry(const pzip_ctx_t* ctx, const char* entry_name,
                                      int32_t* out_exists) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.hasEntry(entry_name, out_exists);
}

PZIP_API pzip_status_t pzip_stat_entry(const pzip_ctx_t* ctx, const char* entry_name,
                                       pzip_entry_info_t* out_info) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.statEntry(entry_name, out_info);
}

PZIP_API pzip_status_t pzip_get_entry_count(const pzip_ctx_t* ctx, uint32_t* out_count) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.getEntryCount(out_count);
}

PZIP_API pzip_status_t pzip_list_entries(const pzip_ctx_t* ctx, pzip_list_callback_t callback,
                                         void* user_data) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.listEntries(callback, user_data);
}

PZIP_API pzip_status_t pzip_list_archive(const char* archive_path, pzip_list_callback_t callback,
                                         void* user_data) {
    if (archive_path == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return pzip::ZipReader::listArchive(archive_path, callback, user_data);
}

PZIP_API pzip_status_t pzip_extract_entry(pzip_ctx_t* ctx, const char* archive_path,
                                          const char* entry_name, const char* output_file) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.extractEntry(archive_path, entry_name, output_file);
}

PZIP_API pzip_status_t pzip_clear_pending(pzip_ctx_t* ctx) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.clearPending();
}

PZIP_API pzip_status_t pzip_last_error(const pzip_ctx_t* ctx, int32_t* code, const char** message) {
    if (ctx == NULL || code == NULL || message == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    *code = ctx->m_engine.lastErrorCode();
    *message = ctx->m_engine.lastErrorMessage();
    return PZIP_OK;
}

PZIP_API uint32_t pzip_get_abi_version(void) { return PZIP_ABI_VERSION; }

PZIP_API const char* pzip_version_string(void) { return kVersion; }

}  // extern "C"

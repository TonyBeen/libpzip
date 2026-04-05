#include <new>

#include "codec/lz4_codec.h"
#include "codec/zlib_codec.h"
#include "codec/zstd_codec.h"
#include "core/pzip_engine.h"
#include "pzip.h"

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

PZIP_API pzip_status_t pzip_close_archive(pzip_ctx_t* ctx) {
    if (ctx == NULL) {
        return PZIP_E_INVALID_ARG;
    }
    return ctx->m_engine.closeArchive();
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

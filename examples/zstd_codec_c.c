#include "pzip.h"

int main(void) {
    pzip_ctx_t* ctx = NULL;
    pzip_options_t opt;
    pzip_codec_vtable_t codec;
    int i;

    for (i = 0; i < 10; ++i) {
        opt.reserve[i] = 0;
    }
    opt.abi_version = PZIP_ABI_VERSION;
    opt.thread_count = 2;
    opt.chunk_size_kb = 256;
    opt.enable_solid_mode = 0;
    opt.max_file_count = 1000;
    opt.max_total_input_bytes = 1ULL << 30;

    if (pzip_create(&opt, &ctx) != PZIP_OK) {
        return 1;
    }
    if (pzip_make_default_zstd_codec(&codec) != PZIP_OK) {
        return 2;
    }
    if (pzip_set_codec(ctx, &codec, NULL) != PZIP_OK) {
        return 3;
    }
    if (pzip_open_archive(ctx, "/tmp/pzip_zstd/out.zip") != PZIP_OK) {
        return 4;
    }
    if (pzip_add_path(ctx, "/tmp/pzip_zlib/in", "in") != PZIP_OK) {
        return 5;
    }
    if (pzip_run(ctx) != PZIP_OK) {
        return 6;
    }

    pzip_destroy(ctx);
    return 0;
}

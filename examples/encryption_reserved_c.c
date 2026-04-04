#include <stdio.h>

#include "pzip.h"

int main(void) {
    pzip_ctx_t* ctx = NULL;
    pzip_options_t opt;
    pzip_encryption_config_t enc;
    int i;

    for (i = 0; i < 10; ++i) {
        opt.reserve[i] = 0;
    }
    opt.abi_version = PZIP_ABI_VERSION;
    opt.thread_count = 0;
    opt.chunk_size_kb = 256;
    opt.enable_solid_mode = 0;
    opt.max_file_count = 1000;
    opt.max_total_input_bytes = 1ULL << 30;

    if (pzip_create(&opt, &ctx) != PZIP_OK) {
        return 1;
    }

    for (i = 0; i < 8; ++i) {
        enc.reserve[i] = 0;
    }
    enc.abi_version = PZIP_ABI_VERSION;
    enc.algorithm_id = 1;
    enc.key_id = 7;
    enc.nonce_size = 12;
    enc.aad_size = 16;

    if (pzip_set_encryption_config(ctx, &enc) != PZIP_OK) {
        return 2;
    }

    enc.algorithm_id = 0;
    if (pzip_get_encryption_config(ctx, &enc) != PZIP_OK) {
        return 3;
    }

    printf("enc_cfg algorithm=%u key_id=%u nonce=%u aad=%u\n", enc.algorithm_id, enc.key_id,
           enc.nonce_size, enc.aad_size);

    pzip_destroy(ctx);
    return 0;
}

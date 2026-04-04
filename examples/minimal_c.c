#include <stdio.h>

#include "pzip.h"

int main(void) {
    pzip_ctx_t* ctx = NULL;
    pzip_options_t opt;
    int32_t code = 0;
    const char* message = NULL;

    for (size_t i = 0; i < sizeof(opt.reserve) / sizeof(opt.reserve[0]); ++i) {
        opt.reserve[i] = 0;
    }
    opt.abi_version = PZIP_ABI_VERSION;
    opt.thread_count = 0;
    opt.chunk_size_kb = 256;
    opt.enable_solid_mode = 0;
    opt.max_file_count = 10000;
    opt.max_total_input_bytes = 1ULL << 34;

    if (pzip_create(&opt, &ctx) != PZIP_OK) {
        return 1;
    }

    pzip_last_error(ctx, &code, &message);
    printf("pzip version=%s abi=%u last=%d msg=%s\n", pzip_version_string(), pzip_get_abi_version(),
           code, message);

    pzip_destroy(ctx);
    return 0;
}

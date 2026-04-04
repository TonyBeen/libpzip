#ifndef PZIP_CODEC_ZSTD_CODEC_H_
#define PZIP_CODEC_ZSTD_CODEC_H_

#include "pzip.h"

namespace pzip {
namespace codec {

pzip_codec_vtable_t CreateZstdCodecVtable();

}  // namespace codec
}  // namespace pzip

#endif  // PZIP_CODEC_ZSTD_CODEC_H_

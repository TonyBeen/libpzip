#ifndef PZIP_CODEC_LZ4_CODEC_H_
#define PZIP_CODEC_LZ4_CODEC_H_

#include "pzip.h"

namespace pzip {
namespace codec {

pzip_codec_vtable_t CreateLz4CodecVtable();

}  // namespace codec
}  // namespace pzip

#endif  // PZIP_CODEC_LZ4_CODEC_H_
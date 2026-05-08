
// WASM entry points for decoding zlib-compressed xdb value blobs.
//
// Input for all decoders: the raw BLOB from an xdb.sqlite row, which holds
// zlib-compressed data with an 8-byte header
// [compressed_size:4][uncompressed_size:4]. Output is UTF-8, NUL-terminated, in
// a static buffer valid until the next call to either decoder.
//
// Note: malformed input will abort() via Assert if an unknown tag is
// encountered during conversion.

#include <emscripten.h>
#include <stdlib.h>
#include <util/buffer.h>
#include <util/hashcons.h>
#include <util/stream.h>
#include <imlang/serial.h>
#include <imlang/block.h>
#include <imlang/storage.h>
#include <imlang/type.h>

NAMESPACE_XGILL_USING

static Buffer g_out_buf;
static Buffer g_uncompressed;

// Decompress data[0..len) into g_uncompressed, reset g_out_buf, and return
// the uncompressed length.
static size_t Decompress(const uint8_t *data, size_t len)
{
  g_out_buf.Reset();
  g_uncompressed.Reset();

  Buffer compressed(data, len);
  compressed.pos = compressed.base + len;
  UncompressBufferInUse(&compressed, &g_uncompressed);
  return g_uncompressed.pos - g_uncompressed.base;
}

// NUL-terminate g_out_buf and return a pointer into it.
static const char *NulTerminate()
{
  uint8_t zero = 0;
  g_out_buf.Append(&zero, 1);
  return (const char*)g_out_buf.base;
}

static HashObject *ReadSingleValue(Buffer *buf)
{
  switch (PeekOpenTag(buf)) {
  case TAG_BlockCFG:     return BlockCFG::Read(buf);
  case TAG_CompositeCSU: return CompositeCSU::Read(buf);
  default:               return nullptr;
  }
}

extern "C" {

EMSCRIPTEN_KEEPALIVE
uint8_t *xdb_alloc(size_t n) { return (uint8_t*)malloc(n); }

EMSCRIPTEN_KEEPALIVE
void xdb_free(uint8_t *p) { free(p); }

// Convert compressed xdb blob to a JSON string.
EMSCRIPTEN_KEEPALIVE
const char *xdb_to_json(const uint8_t *data, size_t len)
{
  size_t ulen = Decompress(data, len);
  Buffer parse_data(g_uncompressed.base, ulen);

  BufferOutStream sink(&g_out_buf);
  PrintJSONBuffer(sink, &parse_data);

  return NulTerminate();
}

// Convert compressed xdb blob to the default xdbfind text format (the
// multi-line human-readable output of `xdbfind <table> <key>` with no flags).
// Auto-detects the top-level type by tag (TAG_BlockCFG / TAG_CompositeCSU),
// mirroring main/xdbfind.cpp.
EMSCRIPTEN_KEEPALIVE
const char *xdb_to_text(const uint8_t *data, size_t len)
{
  size_t ulen = Decompress(data, len);
  Buffer read_buf(g_uncompressed.base, ulen);

  BufferOutStream sink(&g_out_buf);
  while (read_buf.pos != read_buf.base + ulen) {
    HashObject *v = ReadSingleValue(&read_buf);
    if (!v) break;
    sink << v << endl;
  }

  return NulTerminate();
}

// Free all interned data accumulated by previous xdb_to_json / xdb_to_text
// calls. Safe to call between decodes — no HashObject* survives across calls,
// so there are no roots to preserve. Returns the number of HashObjects freed.
//
// The three HashCaches must be cleared before Sweep(): they hold raw pointers
// to interned BlockId*/String* keys, and sweeping first would leave them
// dangling.
EMSCRIPTEN_KEEPALIVE
size_t xdb_flush_interned()
{
  BlockCFGCache.Clear();
  InitializerCache.Clear();
  CompositeCSUCache.Clear();
  ClearMarkBits();
  return Sweep();
}

} // extern "C"

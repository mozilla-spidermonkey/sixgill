
// Stub implementations of symbols declared in imlang/storage.h that are
// defined in imlang/storage.cpp and imlang/interface.cpp.  Those files
// cannot be compiled into the wasm build because they pull in Transaction,
// sqlite3, signal handlers, and other host-only machinery.  None of these
// stubs are on any Print/Read path used by xdb_to_text; they exist solely
// to satisfy the linker.

#include <imlang/storage.h>

NAMESPACE_XGILL_BEGIN

// Null ExternalLookup: if a cache miss ever fires (it shouldn't during
// plain Print), insert NULL so callers gracefully get nullptr.
struct NullLookup_BlockCFG : public Cache_BlockCFG::ExternalLookup {
  void LookupInsert(Cache_BlockCFG *cache, BlockId *id) override {
    cache->Insert(id, nullptr);
  }
};

struct NullLookup_Initializer : public Cache_Initializer::ExternalLookup {
  void LookupInsert(Cache_Initializer *cache, String *key) override {
    cache->Insert(key, nullptr);
  }
};

struct NullLookup_CompositeCSU : public Cache_CompositeCSU::ExternalLookup {
  void LookupInsert(Cache_CompositeCSU *cache, String *key) override {
    cache->Insert(key, nullptr);
  }
};

static NullLookup_BlockCFG    s_lookup_cfg;
static NullLookup_Initializer s_lookup_init;
static NullLookup_CompositeCSU s_lookup_csu;

Cache_BlockCFG    BlockCFGCache(&s_lookup_cfg,  50000);
Cache_Initializer InitializerCache(&s_lookup_init, 25000);
Cache_CompositeCSU CompositeCSUCache(&s_lookup_csu, 50000);

BlockCFG *GetBlockCFG(BlockId *) { return nullptr; }
void ClearBlockCaches() {}
void BlockCFGCacheAddListWithRefs(const Vector<BlockCFG*> &) {}
void BlockCFGUncompress(Transaction *, size_t, Vector<BlockCFG*> *) {}

NAMESPACE_XGILL_END

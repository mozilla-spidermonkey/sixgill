// ES module wrapper around the emscripten-generated WASM module.
//
// The compiled module is loaded relative to `baseUrl`, which defaults to this
// wrapper's own location -- so production loads ./xdb-json-wasm.mjs from beside
// this file. A caller whose module lives elsewhere (e.g. the node build used by
// the test suite, in wasm/test/) passes its own import.meta.url, so the
// same-named xdb-json-wasm.mjs sitting beside *it* is loaded instead. This lets
// one wrapper serve both builds without duplication. ES module specifiers
// resolve relative to the module containing the import, which is why the module
// is loaded via a dynamic import() against baseUrl rather than a static import.
//
// Usage:
//   import init from "./xdb-decode.js";
//   const xdb = await init();                   // module beside this wrapper
//   const xdb = await init(import.meta.url);    // module beside the caller
//   const json = xdb.xdbToJson(blob);           // blob is a Uint8Array
//   const text = xdb.xdbToText(blob);           // same input, xdbfind text format

const modulePromises = new Map();

export default async function init(baseUrl = import.meta.url) {
  const url = new URL("./xdb-json-wasm.mjs", baseUrl).href;
  let modulePromise = modulePromises.get(url);
  if (!modulePromise) {
    modulePromise = import(url).then(({ default: createModule }) => createModule());
    modulePromises.set(url, modulePromise);
  }
  const m = await modulePromise;

  function callDecoder(fn, bytes) {
    const ptr = m._xdb_alloc(bytes.length);
    m.HEAPU8.set(bytes, ptr);
    const outPtr = fn(ptr, bytes.length);
    const out = m.UTF8ToString(outPtr);
    m._xdb_free(ptr);
    return out;
  }

  return {
    // Convert a Uint8Array containing a zlib-compressed xdb value blob
    // into a JSON string.
    xdbToJson: (bytes) => callDecoder(m._xdb_to_json, bytes),

    // Convert a Uint8Array containing a zlib-compressed xdb value blob
    // into the default xdbfind text format (multi-line, human-readable).
    xdbToText: (bytes) => callDecoder(m._xdb_to_text, bytes),

    // Free all interned data accumulated by prior xdbToJson / xdbToText calls.
    // Call periodically to bound memory growth. Returns the count of objects freed.
    flushInternedData: () => m._xdb_flush_interned(),
  };
}

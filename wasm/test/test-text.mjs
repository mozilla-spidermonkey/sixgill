// Test: compare xdb_to_text wasm output against bin/xdbfind for body and comp.
//
// Usage: node --experimental-sqlite wasm/test/test-text.mjs [xdb.sqlite]

import { DatabaseSync } from "node:sqlite";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const dbPath = process.argv[2] ?? "xdb.sqlite";

// Load the node build of the wasm module via the shared wrapper. Passing our
// own import.meta.url makes it resolve xdb-json-wasm.mjs beside this test (the
// node build in wasm/test/) rather than the shell build beside the wrapper.
const { default: init } = await import("../xdb-decode.js");
const xdb = await init(import.meta.url);

let ok = 0;
let fail = 0;

function test(table, key) {
  const db = new DatabaseSync(dbPath, { readOnly: true });
  const row = db.prepare(`SELECT data FROM "${table}" WHERE name = ?`).get(key);
  db.close();
  if (!row) {
    fail++;
    console.log(`FAIL ${table}:${key} (not found)`);
    return;
  }

  const wasmOut = xdb.xdbToText(new Uint8Array(row.data));
  const nativeOut = execFileSync(
    `${root}/bin/xdbfind`,
    [`--db=${dbPath}`, table, key],
    { encoding: "utf8" },
  );

  if (wasmOut === nativeOut) {
    ok++;
    return;
  }

  fail++;
  console.log(`FAIL ${table}: ${key}`);
  // Show first differing line for diagnosis.
  const wa = wasmOut.split("\n");
  const na = nativeOut.split("\n");
  for (let i = 0; i < Math.max(wa.length, na.length); i++) {
    if (wa[i] !== na[i]) {
      console.log(`  wasm[${i}]: ${JSON.stringify(wa[i])}`);
      console.log(`  ref [${i}]: ${JSON.stringify(na[i])}`);
      break;
    }
  }
}

const db = new DatabaseSync(dbPath, { readOnly: true });
const bodyKeys = db
  .prepare("SELECT name FROM body LIMIT 20")
  .all()
  .map((r) => r.name);
const compKeys = db
  .prepare("SELECT name FROM comp LIMIT 20")
  .all()
  .map((r) => r.name);
db.close();

for (const k of bodyKeys) test("body", k);
for (const k of compKeys) test("comp", k);

console.log(`\nResults: ${ok} passed, ${fail} failed`);
if (fail > 0) process.exit(1);

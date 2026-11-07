// Checks the generated site/catalog.js: every component page's options tables,
// keyboard list and related links are well formed, then prints how many pages
// have each.
//   node web/tools/check_catalog.mjs [web/site/catalog.js]
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const file = process.argv[2] || join(dirname(fileURLToPath(import.meta.url)), '..', 'site', 'catalog.js');
const window = {};
new Function('window', readFileSync(file, 'utf8'))(window);
const C = window.CUI_CATALOG;
const errors = [];
const bad = (id, msg) => errors.push(`${id}: ${msg}`);
const ident = /^[A-Za-z_]\w*$/;

const items = Object.values(C.items);
const inCats = new Set(C.categories.flatMap((c) => c.items));
let nOpt = 0, nTables = 0, nFields = 0, nDefaults = 0, nKey = 0, nKeyLines = 0, nRel = 0;
const noOptions = [], noKeyboard = [];

for (const it of items) {
  const id = it.id;
  if (!inCats.has(id)) bad(id, 'not in any category');
  for (const k of ['title', 'summary', 'html']) if (typeof it[k] !== 'string') bad(id, `${k} is not a string`);
  if (!Array.isArray(it.scenes) || !it.scenes.every((s) => C.scenes[s])) bad(id, 'a scene that does not exist');
  if (!it.doc || typeof it.doc.file !== 'string' || !Number.isInteger(it.doc.line) || it.doc.line < 1) bad(id, 'doc file/line missing');

  // options: [{fn: [names], struct, line, fields: [{name, type, default, doc}]}]
  if (!Array.isArray(it.options)) bad(id, 'options is not an array');
  else {
    const structs = new Set();
    for (const o of it.options) {
      if (!Array.isArray(o.fn) || !o.fn.length || !o.fn.every((f) => /^cui_\w+$/.test(f))) bad(id, `options fn ${JSON.stringify(o.fn)}`);
      if (!/^cui_\w+$/.test(o.struct || '')) bad(id, `options struct ${o.struct}`);
      if (structs.has(o.struct)) bad(id, `${o.struct} listed twice`);
      structs.add(o.struct);
      if (!Number.isInteger(o.line) || o.line < 1) bad(id, `${o.struct} has no header line`);
      if (!Array.isArray(o.fields) || !o.fields.length) { bad(id, `${o.struct} has no fields`); continue; }
      const names = new Set();
      for (const f of o.fields) {
        if (!ident.test(f.name || '') || f.name === '_') bad(id, `${o.struct}: field name ${JSON.stringify(f.name)}`);
        if (names.has(f.name)) bad(id, `${o.struct}: ${f.name} twice`);
        names.add(f.name);
        if (typeof f.type !== 'string' || !f.type.trim() || /[;{}]/.test(f.type)) bad(id, `${o.struct}.${f.name}: type ${JSON.stringify(f.type)}`);
        if (typeof f.default !== 'string') bad(id, `${o.struct}.${f.name}: default is not a string`);
        if (typeof f.doc !== 'string' || /\/\*|\*\//.test(f.doc)) bad(id, `${o.struct}.${f.name}: doc ${JSON.stringify(f.doc)}`);
        if (f.default) nDefaults++;
        nFields++;
      }
    }
    if (it.options.length) { nOpt++; nTables += it.options.length; } else noOptions.push(id);
  }

  // keyboard: html strings, each naming at least one key, tags balanced
  if (!Array.isArray(it.keyboard)) bad(id, 'keyboard is not an array');
  else {
    for (const k of it.keyboard) {
      if (typeof k !== 'string' || !k.trim()) { bad(id, 'empty keyboard line'); continue; }
      const open = (k.match(/<kbd>/g) || []).length, close = (k.match(/<\/kbd>/g) || []).length;
      if (open !== close) bad(id, `unbalanced <kbd> in ${JSON.stringify(k)}`);
      if (/<kbd>[^<]*<kbd>/.test(k)) bad(id, `nested <kbd> in ${JSON.stringify(k)}`);
      if (!open && !/arrow/i.test(k)) bad(id, `keyboard line without a key: ${JSON.stringify(k)}`);
      if (/<code>[^<]*<kbd>/.test(k)) bad(id, `<kbd> inside code: ${JSON.stringify(k)}`);
    }
    if (it.keyboard.length) { nKey++; nKeyLines += it.keyboard.length; } else noKeyboard.push(id);
  }

  // related: up to four other existing pages, no repeats
  if (!Array.isArray(it.related)) bad(id, 'related is not an array');
  else {
    if (it.related.length > 4) bad(id, 'more than 4 related');
    if (new Set(it.related).size !== it.related.length) bad(id, 'related repeats');
    for (const r of it.related) {
      if (!C.items[r]) bad(id, `related ${r} does not exist`);
      if (r === id) bad(id, 'related to itself');
    }
    if (it.related.length) nRel++;
  }
}

console.log(`items: ${items.length}`);
console.log(`options: ${nOpt} items with a table (${nTables} tables, ${nFields} rows, ${nDefaults} with a default)`);
console.log(`keyboard: ${nKey} items with a list (${nKeyLines} lines)`);
console.log(`related: ${nRel} items with related links`);
console.log(`no options table (${noOptions.length}): ${noOptions.join(', ') || 'none'}`);
if (errors.length) {
  console.log(`\n${errors.length} problem(s):`);
  for (const e of errors) console.log('  ' + e);
  process.exit(1);
}
console.log('ok: every item is well formed');

const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const sketch = fs.readFileSync('ph_titrator/ph_titrator.ino', 'utf8') +
  fs.readFileSync('ph_titrator/web_ui_page.inc', 'utf8');
const match = sketch.match(/function retainSavedRecords\(records\)([\s\S]*?)<\/script>/);
assert.ok(match, 'embedded retention helper must be present');

const context = {};
vm.createContext(context);
vm.runInContext(`function retainSavedRecords(records)${match[1].replace(/"\);\s*page \+= F\("$/, '')}`, context);

const records = Array.from({ length: 52 }, (_, index) => ({
  id: `run-${index}`,
  savedAt: `2026-07-${String(index + 1).padStart(2, '0')}T00:00:00.000Z`,
}));
const retained = context.retainSavedRecords(records);
assert.equal(retained.length, 50);
assert.equal(retained[0].id, 'run-51');
assert.equal(retained.at(-1).id, 'run-2');
assert.equal(records.length, 52, 'retention must not mutate the input list');

console.log('IndexedDB record retention tests passed');

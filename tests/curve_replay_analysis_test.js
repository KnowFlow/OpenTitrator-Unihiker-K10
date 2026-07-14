const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const sketch = fs.readFileSync('ph_titrator/ph_titrator.ino', 'utf8');
const match = sketch.match(/function analyzeReplay\(points\)([\s\S]*?)function replayText/);
assert.ok(match, 'embedded replay analyzer must be present');

const context = { num: (value) => Number(value || 0), isFinite, Math };
vm.createContext(context);
const analyzerSource = match[1].replace(/"\);\s*page \+= F\("$/, '');
vm.runInContext(`function analyzeReplay(points)${analyzerSource}`, context);

const points = [
  { endpoint: 'pH', used_g: 0.0, ph: 2.0 },
  { endpoint: 'pH', used_g: 0.1, ph: 2.1 },
  { endpoint: 'pH', used_g: 0.1, ph: 2.2 },
  { endpoint: 'pH', used_g: 0.2, ph: 3.0 },
  { endpoint: 'pH', used_g: 0.3, ph: 6.0 },
  { endpoint: 'pH', used_g: 0.4, ph: 6.2 },
  { endpoint: 'pH', used_g: 0.5, ph: 6.3 },
  { endpoint: 'pH', used_g: 0.6, ph: 6.35 },
];
const result = context.analyzeReplay(points);
assert.equal(result.quality, 'high');
assert.equal(result.slopes.length, 5);
assert.equal(result.candidate.used_g, 0.2);
assert.ok(result.candidate.slope > 18);

const mixed = context.analyzeReplay([
  { endpoint: 'pH', used_g: 0.0, ph: 2.0 },
  { endpoint: 'mV', used_g: 0.1, mv: 100 },
  { endpoint: 'pH', used_g: 0.2, ph: 3.0 },
]);
assert.equal(mixed.quality, 'insufficient');
assert.match(mixed.reason, /Mixed/);

console.log('Curve replay analysis tests passed');

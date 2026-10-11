// Compare gqlc and graphql-js validation for one query: node tests/oracle/one.mjs 'query { ... }' [schema.graphqls]
import { buildSchema, validate, parse, specifiedRules } from 'graphql';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '../..');
const sdlPath = process.argv[3] || path.join(HERE, 'harness.graphqls');
const q = process.argv[2];
const gqlb = path.join(ROOT, 'build/one.gqlb');
execFileSync(path.join(ROOT, 'build/gqlc'), ['compile', '-q', '-o', gqlb, '-r', gqlb + '.lua', sdlPath]);
fs.writeFileSync(path.join(ROOT, 'build/one.graphql'), q);
let ours;
try { ours = execFileSync(path.join(ROOT, 'build/gqlc'), ['check', gqlb, path.join(ROOT, 'build/one.graphql')], { encoding: 'utf8' }); }
catch (e) { ours = e.stdout; }
const rest = ours.slice(ours.indexOf(': ') + 2).trim();
const mine = rest === 'ok' ? [] : JSON.parse(rest);
const RULES = specifiedRules.filter((r) => !/DeferStream|StreamDirective/.test(r.name));
let theirs;
try { theirs = validate(buildSchema(fs.readFileSync(sdlPath, 'utf8')), parse(q), RULES, { hideSuggestions: true }).map((e) => ({ message: e.message, locations: e.locations })); }
catch (e) { theirs = [{ message: e.message, locations: e.locations }]; }
const fmt = (e) => `${e.message} ${JSON.stringify((e.locations || []).map((l) => [l.line, l.column]))}`;
console.log('gqlgw:');
for (const e of mine) console.log('  ' + fmt(e));
console.log('graphql-js:');
for (const e of theirs) console.log('  ' + fmt(e));

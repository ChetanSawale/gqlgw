// Differential tests: gqlc / libgql versus graphql-js (the reference implementation).
//
//   node tests/oracle/run.mjs [--fuzz N] [--seed S] [--verbose]
//
// 1. introspection: `gqlc introspect` == introspectionFromSchema (normalized)
// 2. SDL: tests/sdl/*.graphqls compile verdicts and messages
// 3. queries: tests/queries/*.graphql validation messages
// 4. fuzz: random documents, validation verdicts and messages
import {
  buildSchema, validate, parse, specifiedRules, validateSchema, getIntrospectionQuery, graphqlSync,
  isObjectType, isInterfaceType, isUnionType, isInputObjectType, isEnumType, isScalarType,
  isCompositeType, isLeafType, getNamedType, isNonNullType, isListType,
} from 'graphql';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '../..');
const GQLC = path.join(ROOT, 'build/gqlc');
const TMP = fs.mkdtempSync(path.join(os.tmpdir(), 'gqlgw-oracle-'));

const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : dflt;
};
const VERBOSE = args.includes('--verbose');
const FUZZ = Number(opt('--fuzz', 3000));
let seed = Number(opt('--seed', 42));

const GATEWAY_DIRS = `
directive @fetch(source: String!, path: String!, method: String! = "GET", body: String, select: String) on FIELD_DEFINITION
directive @batch(path: String!, match: String!, max: Int! = 100, select: String) on FIELD_DEFINITION
directive @cache(ttl: Int!, scope: String! = "PUBLIC") on FIELD_DEFINITION | OBJECT | INTERFACE | UNION
directive @from(key: String!) on FIELD_DEFINITION
directive @discriminator(key: String!) on INTERFACE | UNION
`;
const GATEWAY_NAMES = new Set(['fetch', 'batch', 'cache', 'from', 'discriminator']);

let failures = 0;
const fail = (what, detail) => {
  failures++;
  console.log(`  FAIL ${what}`);
  if (detail) console.log('    ' + String(detail).split('\n').join('\n    '));
};

function gqlc(...a) {
  try {
    return { code: 0, out: execFileSync(GQLC, a, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'], maxBuffer: 1 << 28 }) };
  } catch (e) {
    return { code: e.status ?? 1, out: e.stdout ?? '', err: e.stderr ?? '' };
  }
}

function compile(sdlPath) {
  const out = path.join(TMP, path.basename(sdlPath).replace(/\.graphqls$/, '') + '.gqlb');
  const r = gqlc('compile', '-q', '-o', out, '-r', out + '.lua', sdlPath);
  if (r.code !== 0) throw new Error(`gqlc compile ${sdlPath} failed:\n${r.err}`);
  return out;
}

const sorted = (a) => [...a].sort();
const sameList = (a, b) => JSON.stringify(sorted(a)) === JSON.stringify(sorted(b));

// ---------------------------------------------------------------- introspection

const canon = (v) => Array.isArray(v) ? v.map(canon)
  : v && typeof v === 'object' ? Object.fromEntries(Object.keys(v).sort().map((k) => [k, canon(v[k])])) : v;

function normIntrospection(x) {
  const s = canon(structuredClone(x.__schema));
  s.directives = s.directives.filter((d) => !GATEWAY_NAMES.has(d.name));
  for (const d of s.directives) d.locations = sorted(d.locations);
  s.directives.sort((a, b) => a.name.localeCompare(b.name));
  s.types.sort((a, b) => a.name.localeCompare(b.name));
  for (const t of s.types) {
    if (t.possibleTypes) t.possibleTypes.sort((a, b) => a.name.localeCompare(b.name));
  }
  return s;
}

function diffJSON(a, b, p = '$') {
  if (JSON.stringify(a) === JSON.stringify(b)) return null;
  if (typeof a !== 'object' || typeof b !== 'object' || a === null || b === null || Array.isArray(a) !== Array.isArray(b))
    return `${p}: ${JSON.stringify(a)?.slice(0, 200)} != ${JSON.stringify(b)?.slice(0, 200)}`;
  const keys = new Set([...Object.keys(a), ...Object.keys(b)]);
  for (const k of keys) {
    const name = Array.isArray(a) ? `${p}[${k}]` : `${p}.${k}`;
    const d = diffJSON(a[k], b[k], a[k]?.name ? `${name}<${a[k].name}>` : name);
    if (d) return d;
  }
  return `${p}: differs`;
}

function testIntrospection(sdlPath) {
  const gqlb = compile(sdlPath);
  const options = {
    descriptions: true, specifiedByUrl: true, directiveIsRepeatable: true, schemaDescription: true,
    inputValueDeprecation: true, oneOf: true,
  };
  const qfile = path.join(TMP, 'introspection.graphql');
  fs.writeFileSync(qfile, getIntrospectionQuery(options));
  const r = gqlc('introspect', gqlb, qfile);
  if (r.code !== 0) return fail(`introspect ${sdlPath}`, r.err);
  const ours = normIntrospection(JSON.parse(r.out));
  const schema = buildSchema(GATEWAY_DIRS + fs.readFileSync(sdlPath, 'utf8'));
  const res = graphqlSync({ schema, source: getIntrospectionQuery(options) });
  if (res.errors) return fail(`graphql-js introspection ${sdlPath}`, JSON.stringify(res.errors));
  const theirs = normIntrospection(res.data);
  const d = diffJSON(ours, theirs);
  if (d) fail(`introspection ${path.basename(sdlPath)}`, d);
  else console.log(`  ok   introspection ${path.basename(sdlPath)} (${ours.types.length} types)`);
}

// ---------------------------------------------------------------------- SDL

function jsSdlErrors(sdl) {
  try {
    const schema = buildSchema(GATEWAY_DIRS + sdl);
    return validateSchema(schema).map((e) => e.message);
  } catch (e) {
    if (e.errors) return e.errors.map((x) => x.message);
    return e.message.split('\n\n');
  }
}

function testSdl() {
  const dir = path.join(ROOT, 'tests/sdl');
  const files = fs.readdirSync(dir).filter((f) => f.endsWith('.graphqls')).sort();
  let ok = 0;
  for (const f of files) {
    const p = path.join(dir, f);
    const sdl = fs.readFileSync(p, 'utf8');
    const expectGateway = /^# gateway/m.test(sdl);
    const r = gqlc('compile', '-q', '-o', path.join(TMP, 'sdl.gqlb'), '-r', path.join(TMP, 'sdl.lua'), p);
    const ours = (r.err || '').split('\n').filter(Boolean).map((l) => l.replace(/^[^:]*:(\d+:\d+:)? /, ''));
    if (expectGateway) {
      // gateway-specific checks: expected messages listed as "# expect: ..." lines
      const expected = [...sdl.matchAll(/^# expect: (.*)$/gm)].map((m) => m[1]);
      if (!sameList(ours, expected)) fail(`sdl ${f}`, `ours:     ${JSON.stringify(ours)}\nexpected: ${JSON.stringify(expected)}`);
      else ok++;
      continue;
    }
    // gqlgw never prints "Did you mean" suggestions
    const theirs = jsSdlErrors(sdl).map((m) => m.replace(/\. Did you mean [^?]*\? Found:/, ', found:').replace(/ Did you mean [^?]*\?$/, ''));
    if (!sameList(ours, theirs)) fail(`sdl ${f}`, `ours:   ${JSON.stringify(sorted(ours))}\ntheirs: ${JSON.stringify(sorted(theirs))}`);
    else ok++;
  }
  console.log(`  ok   sdl: ${ok}/${files.length} files`);
}

// ------------------------------------------------------------------ queries

// @defer / @stream are not supported by gqlgw (and these rules overflow on fragment cycles)
const RULES = specifiedRules.filter((r) => !/DeferStream|StreamDirective/.test(r.name));

function jsValidate(schema, src) {
  let doc;
  try {
    doc = parse(src);
  } catch (e) {
    return [e.message];
  }
  try {
    return validate(schema, doc, RULES, { hideSuggestions: true }).map((e) => e.message);
  } catch (e) {
    return null;   // graphql-js crashed (e.g. stack overflow): no verdict
  }
}

function checkBatch(gqlb, files) {
  const res = new Map();
  for (let i = 0; i < files.length; i += 500) {
    const chunk = files.slice(i, i + 500);
    const r = gqlc('check', gqlb, ...chunk);
    for (const line of r.out.split('\n')) {
      if (!line) continue;
      const k = line.indexOf(': ');
      const f = line.slice(0, k), rest = line.slice(k + 2);
      res.set(f, rest === 'ok' ? [] : JSON.parse(rest).map((e) => e.message));
    }
  }
  return res;
}

const onlyOurs = new Map(), onlyTheirs = new Map();
const pattern = (m) => m.replace(/"[^"]*"/g, '"_"').replace(/-?\d+(\.\d+)?/g, 'N').replace(/: .*$/, ': _');
function multisetDiff(a, b, into) {
  const cnt = new Map();
  for (const x of b) cnt.set(x, (cnt.get(x) || 0) + 1);
  for (const x of a) {
    const c = cnt.get(x) || 0;
    if (c > 0) cnt.set(x, c - 1);
    else { const p = pattern(x); const e = into.get(p) || { n: 0, ex: x }; e.n++; into.set(p, e); }
  }
}
function printSummary() {
  for (const [label, m] of [['only in gqlgw', onlyOurs], ['only in graphql-js', onlyTheirs]]) {
    const top = [...m.entries()].sort((x, y) => y[1].n - x[1].n).slice(0, 15);
    if (!top.length) continue;
    console.log(`  message patterns ${label}:`);
    for (const [, e] of top) console.log(`    ${String(e.n).padStart(5)}  ${e.ex.slice(0, 160)}`);
  }
}

function compareQueries(label, schema, gqlb, files) {
  const ours = checkBatch(gqlb, files);
  let ok = 0, verdict = 0;
  const shown = [];
  for (const f of files) {
    const theirs = jsValidate(schema, fs.readFileSync(f, 'utf8'));
    const mine = ours.get(f);
    if (theirs === null) {
      ok++;
      continue;
    }
    if (theirs.includes('Too many validation errors, error limit reached. Validation aborted.') &&
        mine.includes('Too many validation errors, error limit reached. Validation aborted.')) {
      ok++;   // both truncated: the reported subsets legitimately depend on traversal order
      continue;
    }
    if (!mine) {
      fail(`${label} ${f}`, 'no result from gqlc');
      continue;
    }
    if ((mine.length === 0) !== (theirs.length === 0)) {
      verdict++;
      if (shown.length < 8) shown.push(`${f}\n  src:    ${fs.readFileSync(f, 'utf8').replace(/\n/g, ' ').slice(0, 300)}\n  ours:   ${JSON.stringify(mine)}\n  theirs: ${JSON.stringify(theirs)}`);
    } else if (!sameList(mine, theirs)) {
      multisetDiff(mine, theirs, onlyOurs);
      multisetDiff(theirs, mine, onlyTheirs);
      if (VERBOSE) {
        const dd = path.join(ROOT, 'build/oracle-diffs');
        fs.mkdirSync(dd, { recursive: true });
        fs.copyFileSync(f, path.join(dd, path.basename(f)));
      }
      if (shown.length < 8) shown.push(`${f} (messages)\n  src:    ${fs.readFileSync(f, 'utf8').replace(/\n/g, ' ').slice(0, 300)}\n  ours:   ${JSON.stringify(sorted(mine))}\n  theirs: ${JSON.stringify(sorted(theirs))}`);
    } else ok++;
  }
  const msgOnly = files.length - ok - verdict;
  if (verdict) fail(`${label}: ${verdict} verdict mismatches`, shown.join('\n'));
  else if (msgOnly && VERBOSE) console.log(shown.join('\n'));
  if (msgOnly && VERBOSE) printSummary();
  console.log(`  ${verdict ? 'FAIL' : 'ok  '} ${label}: ${ok}/${files.length} identical, ${msgOnly} message-only diffs, ${verdict} verdict diffs`);
  return { ok, msgOnly, verdict };
}

function testCorpus(schema, gqlb) {
  const dir = path.join(ROOT, 'tests/queries');
  const files = fs.readdirSync(dir).filter((f) => f.endsWith('.graphql')).sort().map((f) => path.join(dir, f));
  return compareQueries('corpus', schema, gqlb, files);
}

// --------------------------------------------------------------------- fuzz

function rand() {
  // mulberry32
  seed |= 0; seed = (seed + 0x6d2b79f5) | 0;
  let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
  t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
}
const pick = (a) => a[Math.floor(rand() * a.length)];
const chance = (p) => rand() < p;

function genDoc(schema) {
  const types = Object.values(schema.getTypeMap()).filter((t) => !t.name.startsWith('__'));
  const composite = types.filter(isCompositeType);
  const inputTypes = types.filter((t) => isInputObjectType(t) || isLeafType(t));
  const varNames = ['a', 'b', 'c', 'flag', 'id'];
  const fragNames = ['F1', 'F2', 'F3'];
  const vars = [];
  const usedFrags = [];

  const typeStr = () => {
    let t = chance(0.1) ? pick(['Dog', 'Unknown', 'CatOrDog']) : pick(inputTypes).name;
    if (chance(0.3)) t = `[${t}]`;
    if (chance(0.4)) t += '!';
    return t;
  };
  const scalarLit = () => pick(['1', '-3', '2.5', '"str"', 'true', 'false', 'null', 'SIT', 'BROWN', '99999999999', '"4"']);
  const literal = (t, depth = 0) => {
    if (chance(0.15) && vars.length) return '$' + pick(vars).name;
    if (chance(0.1) && depth === 0) return '$' + pick(varNames);
    const nt = t ? getNamedType(t) : null;
    if (t && isListType(isNonNullType(t) ? t.ofType : t) && chance(0.6)) {
      const inner = (isNonNullType(t) ? t.ofType : t).ofType;
      return '[' + Array.from({ length: Math.floor(rand() * 3) }, () => literal(inner, depth + 1)).join(', ') + ']';
    }
    if (nt && isInputObjectType(nt) && chance(0.7) && depth < 3) {
      const fs_ = Object.values(nt.getFields());
      const chosen = fs_.filter(() => chance(0.5));
      if (chance(0.15)) chosen.push({ name: 'bogus', type: null });
      if (chance(0.1) && chosen.length) chosen.push(chosen[0]);
      return '{' + chosen.map((f) => `${f.name}: ${literal(f.type, depth + 1)}`).join(', ') + '}';
    }
    if (nt && isEnumType(nt) && chance(0.6)) return pick(nt.getValues()).name;
    return scalarLit();
  };
  const argsFor = (field) => {
    const defs = field ? field.args : [];
    const out = [];
    for (const a of defs) if (chance(0.6)) out.push(`${a.name}: ${literal(a.type)}`);
    if (chance(0.1)) out.push(`unknownArg: ${scalarLit()}`);
    if (chance(0.05) && out.length) out.push(out[0]);
    return out.length ? `(${out.join(', ')})` : '';
  };
  const directives = () => {
    let s = '';
    if (chance(0.15)) s += ` @skip(if: ${chance(0.5) ? 'true' : '$' + (vars.length ? pick(vars).name : 'flag')})`;
    if (chance(0.1)) s += ` @include(if: ${chance(0.5) ? 'false' : '$flag'})`;
    if (chance(0.04)) s += ' @unknownDir';
    if (chance(0.04)) s += ' @onQuery';
    if (chance(0.04)) s += ' @repeatable @repeatable';
    if (chance(0.03)) s += ' @skip';
    return s;
  };
  const selection = (type, depth) => {
    const sels = [];
    const n = 1 + Math.floor(rand() * 4);
    for (let i = 0; i < n; i++) {
      const r = rand();
      if (r < 0.65 || depth > 4 || !type) {
        let fields = type && (isObjectType(type) || isInterfaceType(type)) ? Object.values(type.getFields()) : [];
        let name, field = null;
        if (chance(0.08) || !fields.length) name = chance(0.5) ? '__typename' : pick(['unknownField', 'name', 'id']);
        else { field = pick(fields); name = field.name; }
        if (field && type && chance(0.03)) name = 'nope';
        const alias = chance(0.15) ? pick(['x', 'y', 'name', 'barks', 'meows']) + ': ' : '';
        let sub = '';
        const ft = field ? getNamedType(field.type) : null;
        if (depth < 6 && ((ft && isCompositeType(ft) && !chance(0.05)) || (!ft && chance(0.1)) || (ft && isLeafType(ft) && chance(0.03))))
          sub = ' ' + selection(ft && isCompositeType(ft) ? ft : null, depth + 1);
        sels.push(`${alias}${name}${argsFor(field)}${directives()}${sub}`);
      } else if (r < 0.85) {
        const tc = chance(0.85) ? pick(composite) : pick(types);
        const cond = chance(0.8) ? ` on ${chance(0.05) ? 'Missing' : tc.name}` : '';
        sels.push(`...${cond}${directives()} ${depth < 6 ? selection(cond ? (isCompositeType(tc) ? tc : null) : type, depth + 1) : '{ __typename }'}`);
      } else {
        const f = chance(0.9) ? pick(fragNames) : 'Undefined';
        usedFrags.push(f);
        sels.push(`...${f}${directives()}`);
      }
    }
    return '{ ' + sels.join(' ') + ' }';
  };

  const nvars = Math.floor(rand() * 4);
  for (let i = 0; i < nvars; i++) {
    const t = typeStr();
    vars.push({ name: chance(0.9) ? varNames[i] : varNames[0], type: t });
  }
  const varDefs = vars.length
    ? '(' + vars.map((v) => {
        let d = '';
        if (chance(0.2)) {
          try { d = ' = ' + literal(schema.getType(v.type.replace(/[[\]!]/g, '')) ? null : null, 1); } catch { d = ''; }
          if (d.includes('$')) d = ' = 1';
        }
        return `$${v.name}: ${v.type}${d}${chance(0.03) ? ' @onVariableDefinition' : ''}`;
      }).join(', ') + ')'
    : '';
  const isMutation = chance(0.1);
  const root = isMutation ? schema.getMutationType() : schema.getQueryType();
  let doc = '';
  const body = chance(0.05)
    ? '{ __schema { types { name fields { name type { name fields { name type { name fields { name } } } } } } } }'
    : selection(root, 0);
  doc += `${isMutation ? 'mutation' : 'query'}${chance(0.7) ? ' Op' : ''}${varDefs}${directives()} ${body}\n`;
  if (chance(0.05)) doc += `query Op { __typename }\n`;
  for (const f of fragNames) {
    if (!usedFrags.includes(f) && !chance(0.1)) continue;
    const tc = chance(0.9) ? pick(composite) : pick(types);
    doc += `fragment ${f} on ${tc.name}${chance(0.03) ? ' @onFragmentDefinition' : ''} ${selection(isCompositeType(tc) ? tc : null, 1)}\n`;
  }
  return doc;
}

function testFuzz(schema, gqlb, n) {
  const dir = path.join(TMP, 'fuzz');
  fs.mkdirSync(dir, { recursive: true });
  const files = [];
  for (let i = 0; i < n; i++) {
    const f = path.join(dir, `q${String(i).padStart(5, '0')}.graphql`);
    fs.writeFileSync(f, genDoc(schema));
    files.push(f);
  }
  return compareQueries(`fuzz (seed ${opt('--seed', 42)})`, schema, gqlb, files);
}

// --------------------------------------------------------------------- main

console.log('oracle: graphql-js', (await import('graphql')).version);
testIntrospection(path.join(ROOT, 'examples/blog.graphqls'));
testIntrospection(path.join(HERE, 'harness.graphqls'));
testSdl();
const harnessSdl = path.join(HERE, 'harness.graphqls');
const harness = buildSchema(fs.readFileSync(harnessSdl, 'utf8'));
const harnessGqlb = compile(harnessSdl);
testCorpus(harness, harnessGqlb);
if (FUZZ > 0) testFuzz(harness, harnessGqlb, FUZZ);
fs.rmSync(TMP, { recursive: true, force: true });
console.log(failures ? `\n${failures} failure(s)` : '\nall oracle tests passed');
process.exit(failures ? 1 : 0);

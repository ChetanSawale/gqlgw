// Prints "ok" or the graphql-js syntax error (line:col: message) for each file.
import { parse } from 'graphql';
import { readFileSync } from 'node:fs';
for (const f of process.argv.slice(2)) {
  try {
    const doc = parse(readFileSync(f, 'utf8'));
    console.log(`${f}: ok (${doc.definitions.length} defs)`);
  } catch (e) {
    const l = e.locations?.[0];
    console.log(`${f}:${l?.line}:${l?.column}: ${e.message}`);
  }
}

#!/usr/bin/env node

import { copyFileSync, mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(scriptDirectory, '..', '..', '..');
const brandingDirectory = resolve(repositoryRoot, 'assets', 'branding');
const staticDirectory = resolve(scriptDirectory, '..', 'static');

mkdirSync(staticDirectory, { recursive: true });
for (const variant of ['static', 'boot', 'inference']) {
    const name = 'spark-edge-' + variant + '.svg';
    copyFileSync(resolve(brandingDirectory, name), resolve(staticDirectory, name));
}
copyFileSync(resolve(brandingDirectory, 'spark-edge.png'), resolve(staticDirectory, 'spark-edge.png'));
copyFileSync(resolve(brandingDirectory, 'spark-edge.ico'), resolve(staticDirectory, 'favicon.ico'));
copyFileSync(resolve(brandingDirectory, 'spark-edge.ico'), resolve(staticDirectory, 'favicon-dark.ico'));

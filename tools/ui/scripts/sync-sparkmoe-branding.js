#!/usr/bin/env node

import { copyFileSync, mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const repositoryRoot = resolve(scriptDirectory, '..', '..', '..');
const brandingDirectory = resolve(repositoryRoot, 'assets', 'branding');
const staticDirectory = resolve(scriptDirectory, '..', 'static');

mkdirSync(staticDirectory, { recursive: true });
copyFileSync(resolve(brandingDirectory, 'SparkMoE.png'), resolve(staticDirectory, 'SparkMoE.png'));
copyFileSync(resolve(brandingDirectory, 'SparkMoE.ico'), resolve(staticDirectory, 'favicon.ico'));
copyFileSync(resolve(brandingDirectory, 'SparkMoE.ico'), resolve(staticDirectory, 'favicon-dark.ico'));


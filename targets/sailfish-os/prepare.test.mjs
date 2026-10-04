import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import {
  appRelative, assertPackageVersion, nativeKind, resolveGeastackPackage, validateLibraryName,
} from './prepare-lib.mjs'

const here = path.dirname(fileURLToPath(import.meta.url))
const root = fs.mkdtempSync(path.join(os.tmpdir(), 'sailfish-prepare-'))
const app = path.join(root, 'app')
fs.mkdirSync(path.join(app, 'node_modules', '@geastack', 'core'), { recursive: true })
fs.writeFileSync(path.join(app, 'node_modules', '@geastack', 'core', 'package.json'), '{"name":"@geastack/core","version":"1.2.3"}\n')

assert.equal(resolveGeastackPackage(app, root, 'core'), path.join(app, 'node_modules', '@geastack', 'core'))
assert.throws(() => resolveGeastackPackage(app, root, 'engine'), /missing @geastack\/engine/)
assert.throws(() => assertPackageVersion('core', '1.2.3', '1.2.4'), /@geastack\/core version mismatch: app package 1.2.3, checkout 1.2.4/)
assert.throws(() => assertPackageVersion('host', '', '1.0.0'), /@geastack\/host version mismatch: app package missing/)
assert.doesNotThrow(() => assertPackageVersion('core', '1.2.3', '1.2.3'))

assert.equal(appRelative(app, 'native/host.cpp'), path.join(app, 'native', 'host.cpp'))
assert.throws(() => appRelative(app, '../secret.cpp'), /stay inside the app/)
assert.throws(() => appRelative(app, path.resolve(app, 'native/host.cpp')), /must be relative/)
assert.equal(nativeKind('native/host.cpp'), 'cxx')
assert.equal(nativeKind('native/host.h'), 'header')
assert.equal(nativeKind('native/host.c'), 'c')
assert.throws(() => nativeKind('native/host.ts'), /must be C, C\+\+, or headers/)
assert.equal(validateLibraryName('openssl'), 'openssl')
assert.throws(() => validateLibraryName('pkg;rm'), /pkg-config module names/)

const project = path.join(root, 'project')
fs.mkdirSync(project, { recursive: true })
fs.writeFileSync(path.join(project, 'empty.o'), '')
fs.writeFileSync(path.join(project, 'text.o'), 'not an object')
fs.writeFileSync(path.join(project, 'real.o'), Buffer.from([0x7f, 0x45, 0x4c, 0x46, 1, 2, 3, 4]))
const recover = spawnSync(process.execPath, [path.join(here, 'recover-build.mjs'), project], { encoding: 'utf8' })
assert.equal(recover.status, 0)
assert.equal(fs.existsSync(path.join(project, 'empty.o')), false)
assert.equal(fs.existsSync(path.join(project, 'text.o')), false)
assert.equal(fs.existsSync(path.join(project, 'real.o')), true)
fs.mkdirSync(path.join(project, 'RPMS'), { recursive: true })
fs.writeFileSync(path.join(project, 'RPMS', 'app.rpm'), 'rpm')
const cleaned = spawnSync(process.execPath, [path.join(here, 'recover-build.mjs'), project, '--clean'], { encoding: 'utf8' })
assert.equal(cleaned.status, 0)
assert.equal(fs.existsSync(path.join(project, 'RPMS')), false)
assert.equal(fs.existsSync(path.join(project, 'real.o')), false)

fs.rmSync(root, { recursive: true, force: true })
console.log('sailfish prepare tests passed')

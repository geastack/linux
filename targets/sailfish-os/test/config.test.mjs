import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { desktopEntry, readSailfishConfig, rpmSpec, sailfishDensity } from '../config.mjs'

const appDir = fs.mkdtempSync(path.join(os.tmpdir(), 'gea-sailfish-config-'))
const outside = fs.mkdtempSync(path.join(os.tmpdir(), 'gea-sailfish-outside-'))
process.on('exit', () => {
  fs.rmSync(appDir, { recursive: true, force: true })
  fs.rmSync(outside, { recursive: true, force: true })
})
fs.mkdirSync(path.join(appDir, 'native/include'), { recursive: true })
fs.writeFileSync(path.join(appDir, 'native/host.cpp'), '')
fs.writeFileSync(path.join(appDir, 'native/helper.c'), '')
fs.writeFileSync(path.join(appDir, 'native/notes.txt'), '')
fs.writeFileSync(path.join(outside, 'evil.cpp'), '')
fs.symlinkSync(path.join(outside, 'evil.cpp'), path.join(appDir, 'native/link.cpp'))

const base = (gea = {}, extra = {}) => ({ version: '1.2.3', license: 'MIT', ...extra, gea: { id: 'probe-app', name: 'Probe', ...gea } })
const read = (gea, extra) => readSailfishConfig(appDir, base(gea, extra))

let failures = 0
const test = (name, fn) => {
  try {
    fn()
    console.log(`ok - ${name}`)
  } catch (error) {
    failures++
    console.log(`not ok - ${name}\n  ${error.message}`)
  }
}

test('defaults keep the sandbox and the existing identity', () => {
  const config = read()
  assert.equal(config.packageName, 'harbour-gea-probe-app')
  assert.equal(config.applicationName, 'probe_app')
  assert.equal(config.sandboxing, true)
  const desktop = desktopEntry(config)
  assert.doesNotMatch(desktop, /Sandboxing/)
  assert.match(desktop, /\[X-Sailjail\]\nOrganizationName=org\.geastack\nApplicationName=probe_app\nPermissions=\n$/)
})

test('density defaults to auto: panel-derived on ARM, 1 on the emulator', () => {
  const config = read()
  assert.equal(config.devicePixelRatio, 'auto')
  // 0 tells the executable to derive an integer DPR from the panel.
  assert.deepEqual(sailfishDensity(config, 'aarch64'), { runtime: 0, fontRatios: [2, 3] })
  assert.deepEqual(sailfishDensity(config, 'armv7hl'), { runtime: 0, fontRatios: [2, 3] })
  assert.deepEqual(sailfishDensity(config, 'i486'), { runtime: 1, fontRatios: [1] })
})

test('an explicit density is used on every architecture', () => {
  const config = read({ sailfish: { devicePixelRatio: 2.5 } })
  assert.deepEqual(sailfishDensity(config, 'aarch64'), { runtime: 2.5, fontRatios: [2.5] })
  assert.deepEqual(sailfishDensity(config, 'i486'), { runtime: 2.5, fontRatios: [2.5] })
  assert.deepEqual(sailfishDensity(read({ sailfish: { devicePixelRatio: 'auto' } }), 'aarch64').runtime, 0)
  assert.throws(() => read({ sailfish: { devicePixelRatio: 'native' } }), /devicePixelRatio/)
  assert.throws(() => read({ sailfish: { devicePixelRatio: 0 } }), /devicePixelRatio/)
})

test('sandboxing: false opts out of Sailjail', () => {
  const desktop = desktopEntry(read({ sailfish: { sandboxing: false, permissions: ['Audio', 'Internet', 'Audio'] } }))
  assert.match(desktop, /\[X-Sailjail\]\nSandboxing=Disabled\nOrganizationName=/)
  assert.match(desktop, /Permissions=Audio;Internet\n$/)
})

test('sandboxing must be a boolean', () => {
  assert.throws(() => read({ sailfish: { sandboxing: 'no' } }), /sandboxing must be true or false/)
})

test('native sources resolve inside the app', () => {
  const config = read({ nativeSources: ['native/host.cpp', 'native/helper.c', 'native/host.cpp'] })
  assert.deepEqual(config.nativeSources.map((s) => s.relative), ['native/host.cpp', 'native/helper.c'])
  assert.equal(config.nativeSources[0].absolute, fs.realpathSync(path.join(appDir, 'native/host.cpp')))
})

test('native sources reject escapes, missing files and other extensions', () => {
  assert.throws(() => read({ nativeSources: ['../x.cpp'] }), /invalid path/)
  assert.throws(() => read({ nativeSources: [path.join(appDir, 'native/host.cpp')] }), /invalid path/)
  assert.throws(() => read({ nativeSources: ['native/missing.cpp'] }), /missing/)
  assert.throws(() => read({ nativeSources: ['native/notes.txt'] }), /unsupported source/)
  assert.throws(() => read({ nativeSources: ['native/link.cpp'] }), /escapes the app directory/)
  assert.throws(() => read({ nativeSources: 'native/host.cpp' }), /array of nonempty strings/)
})

test('include directories must be app directories', () => {
  const config = read({ sailfish: { nativeIncludeDirectories: ['native/include'] } })
  assert.deepEqual(config.nativeIncludeDirectories.map((d) => d.relative), ['native/include'])
  assert.throws(() => read({ sailfish: { nativeIncludeDirectories: ['native/host.cpp'] } }), /not a directory/)
})

test('system libraries and pkg-config modules are plain names', () => {
  const config = read({ sailfish: { nativeSystemLibraries: ['util'], pkgConfigModules: ['maliit-glib'] } })
  assert.deepEqual(config.nativeSystemLibraries, ['util'])
  assert.throws(() => read({ sailfish: { nativeSystemLibraries: ['-lfoo'] } }), /invalid library/)
  assert.throws(() => read({ sailfish: { nativeSystemLibraries: ['util;rm'] } }), /invalid library/)
  assert.throws(() => read({ sailfish: { pkgConfigModules: ['a b'] } }), /invalid module/)
})

test('spec lists pkg-config modules as BuildRequires', () => {
  const spec = rpmSpec(read({ sailfish: { pkgConfigModules: ['glib-2.0', 'zlib'] } }))
  assert.match(spec, /BuildRequires: pkgconfig\(libcurl\)\nBuildRequires: pkgconfig\(gio-2\.0\)\nBuildRequires: pkgconfig\(glib-2\.0\)\nBuildRequires: pkgconfig\(zlib\)\n\n%description/)
  assert.match(spec, /^Name: harbour-gea-probe-app\nVersion: 1\.2\.3\n/)
})

test('prerelease versions sort before the release in RPM', () => {
  assert.equal(read({}, { version: '1.2.3-beta-2' }).rpmVersion, '1.2.3~beta.2')
})

if (failures) {
  console.log(`${failures} failed`)
  process.exit(1)
}

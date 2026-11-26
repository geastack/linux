import assert from 'node:assert/strict'
import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import sharp from 'sharp'

const here = path.dirname(fileURLToPath(import.meta.url))
const targetDir = path.resolve(here, '..')
const linuxRoot = path.resolve(targetDir, '../..')
const prepareScript = path.join(targetDir, 'prepare.mjs')

const corePkgDir = path.join(linuxRoot, 'node_modules/@geastack/core')
const enginePkgDir = path.join(linuxRoot, 'node_modules/@geastack/engine')
const coreVersion = JSON.parse(fs.readFileSync(path.join(corePkgDir, 'package.json'), 'utf8')).version

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'gea-sailfish-prepare-'))
process.on('exit', () => {
  fs.rmSync(tmpRoot, { recursive: true, force: true })
})

// Build a synthetic core-repo fixture whose core/engine come from the installed
// packages and whose host/elements/geaos paths satisfy core/gea_sources.mjs.
async function createCoreRepoFixture(dir, versionOverride = coreVersion) {
  const pkgRoot = path.join(dir, 'packages')
  fs.mkdirSync(pkgRoot, { recursive: true })
  fs.cpSync(corePkgDir, path.join(pkgRoot, 'core'), { recursive: true, dereference: true })
  fs.cpSync(enginePkgDir, path.join(pkgRoot, 'engine'), { recursive: true, dereference: true })

  const coreMetaPath = path.join(pkgRoot, 'core/package.json')
  const coreMeta = JSON.parse(fs.readFileSync(coreMetaPath, 'utf8'))
  coreMeta.version = versionOverride
  fs.writeFileSync(coreMetaPath, JSON.stringify(coreMeta, null, 2))

  for (const name of ['host', 'elements', 'geaos']) {
    fs.mkdirSync(path.join(pkgRoot, name), { recursive: true })
    fs.writeFileSync(path.join(pkgRoot, name, 'package.json'), JSON.stringify({ name: `@geastack/${name}`, version: '0.0.0-test' }))
  }

  const env = {
    GEA_CORE: path.join(pkgRoot, 'core'),
    GEA_HOST_DIR: path.join(pkgRoot, 'host'),
    GEA_ENGINE_DIR: path.join(pkgRoot, 'engine'),
    GEA_ELEMENTS_DIR: path.join(pkgRoot, 'elements'),
    GEA_GEAOS_PACKAGE_DIR: path.join(pkgRoot, 'geaos'),
  }
  const { cSources, cxxSources } = await import(pathToFileURL(path.join(pkgRoot, 'core/gea_sources.mjs')))
  for (const file of [...cSources(env), ...cxxSources({ ...env, GEA_DESKTOP_WEBSOCKET_CURL: '1' })]) {
    if (!fs.existsSync(file)) {
      fs.mkdirSync(path.dirname(file), { recursive: true })
      fs.writeFileSync(file, '// test stub\n')
    }
  }
  return dir
}

function parseCmakeList(content, name) {
  const match = content.match(new RegExp(`set\\(${name}\\n([\\s\\S]*?)\\n\\)`))
  if (!match) return []
  return match[1]
    .split('\n')
    .map((line) => line.trim().replace(/^"|\s*"$/g, '').replaceAll('${CMAKE_CURRENT_SOURCE_DIR}/', ''))
    .filter(Boolean)
}

function runPrepare(appDir, arch, coreRepo) {
  return spawnSync(process.execPath, [prepareScript, appDir, arch, ...(coreRepo ? [coreRepo] : [])], {
    cwd: linuxRoot,
    encoding: 'utf8',
  })
}

const coreRepo = await createCoreRepoFixture(path.join(tmpRoot, 'core-repo'))
const mismatchCoreRepo = await createCoreRepoFixture(path.join(tmpRoot, 'core-mismatch'), '0.0.0-mismatch')

// Build a valid test app with nativeSources and Sailfish metadata.
const appDir = path.join(tmpRoot, 'sample-app')
fs.mkdirSync(path.join(appDir, 'native/include'), { recursive: true })
fs.writeFileSync(path.join(appDir, 'native/include/custom.h'), '#pragma once\n')
fs.writeFileSync(path.join(appDir, 'native/addon.cpp'), 'void addon_fn() {}\n')
fs.writeFileSync(path.join(appDir, 'native/helper.c'), 'void helper_fn(void) {}\n')
await sharp({
  create: { width: 256, height: 256, channels: 4, background: { r: 20, g: 120, b: 220, alpha: 1 } },
}).png().toFile(path.join(appDir, 'icon.png'))

fs.writeFileSync(path.join(appDir, 'index.tsx'), `import { Component } from '@geastack/core'
localStorage.setItem('k', 'v')
export default class App extends Component {
  template() { return <div><span>Sailfish Test</span></div> }
}
`)
fs.writeFileSync(path.join(appDir, 'package.json'), JSON.stringify({
  name: 'sample-app',
  version: '1.2.3-rc.1',
  license: 'Apache-2.0',
  gea: {
    id: 'sample-app',
    name: 'Sample App',
    entry: 'index.tsx',
    icons: { '256': 'icon.png' },
    nativeSources: ['native/addon.cpp', 'native/helper.c'],
    sailfish: {
      organizationName: 'org.example',
      applicationName: 'sample_app',
      permissions: ['Audio', 'Internet'],
      nativeIncludeDirectories: ['native/include'],
      nativeSystemLibraries: ['util'],
      pkgConfigModules: ['glib-2.0'],
    },
  },
}, null, 2))

let failures = 0
async function test(name, fn) {
  try {
    await fn()
    console.log(`ok - ${name}`)
  } catch (error) {
    failures++
    console.log(`not ok - ${name}\n  ${error.stack || error.message}`)
  }
}

await test('prepare.mjs stages complete project for aarch64, armv7hl, and i486', async () => {
  for (const [arch, expectedDpr] of [['aarch64', '0'], ['armv7hl', '0'], ['i486', '1']]) {
    const res = runPrepare(appDir, arch, coreRepo)
    assert.equal(res.status, 0, `prepare failed for ${arch}:\n${res.stdout}\n${res.stderr}`)

    const stage = path.join(appDir, '.gea-sailfish/build', `sample-app-${arch}`, 'project')
    assert.ok(fs.existsSync(path.join(stage, 'CMakeLists.txt')))
    assert.ok(fs.existsSync(path.join(stage, 'platform/include/rpios_keyboard.h')))
    assert.ok(fs.existsSync(path.join(stage, 'platform/include/sailfish_dpr.h')))
    assert.ok(fs.existsSync(path.join(stage, 'platform/include/sailfish_keyboard_logic.h')))

    const sourcesCmake = fs.readFileSync(path.join(stage, 'sources.cmake'), 'utf8')
    assert.match(sourcesCmake, new RegExp(`set\\(GEA_SAILFISH_DEFAULT_DPR ${expectedDpr}\\)`))
    assert.match(sourcesCmake, /set\(GEA_APP_PKG_MODULES "glib-2.0"\)/)
    assert.match(sourcesCmake, /set\(GEA_APP_SYSTEM_LIBRARIES "util"\)/)

    const cFiles = parseCmakeList(sourcesCmake, 'GEA_C_SOURCES')
    const cxxFiles = parseCmakeList(sourcesCmake, 'GEA_CXX_SOURCES')
    const incDirs = parseCmakeList(sourcesCmake, 'GEA_INCLUDE_DIRS')
    assert.ok(cFiles.includes('app_native/native/helper.c'))
    assert.ok(cxxFiles.includes('app_native/native/addon.cpp'))
    assert.ok(incDirs.includes('app_native/native/include'))
    assert.ok(cxxFiles.some((f) => f.endsWith('/host/websocket_curl.cpp')))
    assert.ok(!cxxFiles.some((f) => f.endsWith('/runtime.cpp')))
    assert.ok(!cxxFiles.some((f) => f.endsWith('/host/camera.cpp')))
    assert.ok(!cxxFiles.some((f) => f.endsWith('sailfish_storage_bridge.cpp')))

    for (const rel of [...cFiles, ...cxxFiles]) {
      assert.ok(fs.existsSync(path.join(stage, rel)), `missing staged source: ${rel}`)
    }

    for (const size of [86, 108, 128, 172]) {
      const iconPath = path.join(stage, 'icons', `${size}.png`)
      assert.ok(fs.existsSync(iconPath), `missing icon ${size}.png`)
      const meta = await sharp(iconPath).metadata()
      assert.equal(meta.format, 'png')
      assert.equal(meta.width, size)
      assert.equal(meta.height, size)
      assert.equal(meta.hasAlpha, true)
    }

    const desktop = fs.readFileSync(path.join(stage, 'harbour-gea-sample-app.desktop'), 'utf8')
    assert.match(desktop, /\[X-Sailjail\]\nOrganizationName=org\.example\nApplicationName=sample_app\nPermissions=Audio;Internet\n$/)

    const spec = fs.readFileSync(path.join(stage, 'rpm/harbour-gea-sample-app.spec'), 'utf8')
    assert.match(spec, /Version: 1\.2\.3~rc\.1/)
    assert.match(spec, /BuildRequires: pkgconfig\(glib-2\.0\)/)
  }
})

await test('prepare.mjs stages probe-app (real kitchen-sink fixture)', async () => {
  const probeApp = path.join(here, 'probe-app')
  fs.rmSync(path.join(probeApp, '.gea-sailfish'), { recursive: true, force: true })
  try {
    const res = runPrepare(probeApp, 'aarch64', coreRepo)
    assert.equal(res.status, 0, `probe-app prepare failed:\n${res.stdout}\n${res.stderr}`)
    const stage = path.join(probeApp, '.gea-sailfish/build/sailfish-probe-aarch64/project')
    assert.ok(fs.existsSync(path.join(stage, 'rpm/harbour-gea-sailfish-probe.spec')))
  } finally {
    fs.rmSync(path.join(probeApp, '.gea-sailfish'), { recursive: true, force: true })
  }
})

await test('prepare.mjs fails cleanly on invalid arch, version mismatch, missing entry, and missing icon', () => {
  const badArch = runPrepare(appDir, 'mips', coreRepo)
  assert.equal(badArch.status, 2)

  const mismatch = runPrepare(appDir, 'aarch64', mismatchCoreRepo)
  assert.notEqual(mismatch.status, 0)
  assert.match(mismatch.stderr, /core version mismatch/)

  const noIconApp = path.join(tmpRoot, 'no-icon-app')
  fs.cpSync(appDir, noIconApp, { recursive: true })
  fs.rmSync(path.join(noIconApp, 'icon.png'))
  const noIconRes = runPrepare(noIconApp, 'aarch64', coreRepo)
  assert.notEqual(noIconRes.status, 0)
  assert.match(noIconRes.stderr, /icon/)

  const noEntryApp = path.join(tmpRoot, 'no-entry-app')
  fs.cpSync(appDir, noEntryApp, { recursive: true })
  fs.rmSync(path.join(noEntryApp, 'index.tsx'))
  const noEntryRes = runPrepare(noEntryApp, 'aarch64', coreRepo)
  assert.notEqual(noEntryRes.status, 0)
  assert.match(noEntryRes.stderr, /app entry missing/)
})

await test('sailfish_main.cpp keeps required SDL keyboard and lifecycle wiring', () => {
  const mainCpp = fs.readFileSync(path.join(targetDir, 'main/sailfish_main.cpp'), 'utf8')
  for (const needle of [
    'case SDL_WINDOWEVENT_CLOSE:',
    'case SDL_KEYUP:',
    'case SDL_TEXTEDITING:',
    'case SDL_TEXTEDITING_EXT:',
    'g_keyboard.accept(event)',
    'PULSE_PROP_media.role',
    'sailfish_keyboard_update(-1, false)',
  ]) {
    assert.ok(mainCpp.includes(needle), `sailfish_main.cpp missing ${needle}`)
  }
  assert.doesNotMatch(mainCpp, /if\s*\([^)]*SDLK_ESCAPE[^)]*\)\s*(?:g_stop\s*=|return\b)/)
})

if (failures) {
  console.log(`${failures} failed`)
  process.exit(1)
}

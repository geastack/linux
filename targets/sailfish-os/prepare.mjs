#!/usr/bin/env node
import fs from 'node:fs'
import path from 'node:path'
import { spawnSync } from 'node:child_process'
import { fileURLToPath, pathToFileURL } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const linuxRoot = path.resolve(here, '../..')
const [appArg, arch = 'i486', coreArg] = process.argv.slice(2)
if (!appArg || !['i486', 'armv7hl', 'aarch64'].includes(arch)) {
  console.error('usage: node prepare.mjs <app-directory> [i486|armv7hl|aarch64] [core-repository]')
  process.exit(2)
}
const appDir = path.resolve(appArg)
const coreRepo = path.resolve(coreArg || path.join(linuxRoot, '../core'))
const meta = JSON.parse(fs.readFileSync(path.join(appDir, 'package.json'), 'utf8'))
const app = meta.gea
if (!app?.id || !/^[a-z0-9][a-z0-9-]*$/.test(app.id)) throw new Error('gea.id must be a lowercase package identifier')
const entry = app.entry || 'index.tsx'
if (!fs.existsSync(path.join(appDir, entry))) throw new Error(`app entry missing: ${entry}`)
const packageName = `harbour-gea-${app.id}`
const generated = path.join(here, 'generated', app.id)
const stage = path.join(here, 'build', `${app.id}-${arch}`, 'project')
fs.mkdirSync(path.dirname(generated), { recursive: true })
const npmCore = path.join(linuxRoot, 'node_modules/@geastack/core')
const args = [path.join(npmCore, 'scripts/build-gea-vite-geatsc.mjs'),
  '--app-dir', appDir, '--entry', entry, '--out-dir', generated,
  '--geatsc-bin', path.join(linuxRoot, 'node_modules/@geastack/compiler/dist/cli.js'),
  '--geatsc-gea-plugin', path.join(linuxRoot, 'node_modules/@geastack/geatsc-plugin-gea/dist/index.js'),
  '--font-viewport-width', '410', '--font-viewport-height', '502', '--font-device-pixel-ratio', '2']
const result = spawnSync(process.execPath, args, { stdio: 'inherit', cwd: linuxRoot })
if (result.status !== 0) process.exit(result.status || 1)

// sfdk mounts this project into its Linux build engine. Every compiled input
// must therefore be inside this directory, with no Windows absolute paths.
const resolvedStage = path.resolve(stage)
const allowedParent = path.resolve(here, 'build') + path.sep
if (!resolvedStage.startsWith(allowedParent)) throw new Error('unsafe stage directory')
fs.rmSync(stage, { recursive: true, force: true })
fs.mkdirSync(stage, { recursive: true })
const copyTree = (from, to) => fs.cpSync(from, to, { recursive: true,
  filter: (item) => !['.git', 'node_modules', 'dist', 'build'].includes(path.basename(item)) })
for (const name of ['core', 'host', 'engine', 'elements', 'geaos']) {
  const source = path.join(coreRepo, 'packages', name)
  if (!fs.existsSync(source)) throw new Error(`missing framework package: ${source}`)
  copyTree(source, path.join(stage, 'framework', name))
}
copyTree(generated, path.join(stage, 'generated'))
copyTree(path.join(linuxRoot, 'targets/raspberry-pi-os/main'), path.join(stage, 'platform/main'))
copyTree(path.join(linuxRoot, 'targets/raspberry-pi-os/include'), path.join(stage, 'platform/include'))
fs.copyFileSync(path.join(here, 'CMakeLists.txt'), path.join(stage, 'CMakeLists.txt'))

const env = Object.fromEntries(['core', 'host', 'engine', 'elements', 'geaos'].map((name) => [
  ({ core: 'GEA_CORE', host: 'GEA_HOST_DIR', engine: 'GEA_ENGINE_DIR', elements: 'GEA_ELEMENTS_DIR', geaos: 'GEA_GEAOS_PACKAGE_DIR' })[name],
  path.join(stage, 'framework', name).replaceAll('\\', '/')]))
const { includeFlags, cSources, cxxSources } = await import(pathToFileURL(path.join(coreRepo, 'packages/core/gea_sources.mjs')))
const rel = (file) => path.relative(stage, file).replaceAll('\\', '/')
const c = [...cSources(env).map(rel), 'platform/main/rpios_apps.c']
const cxx = cxxSources(env).filter((s) => !/\/(?:host\/camera|runtime|services\/[a-z_]+)\.cpp$/.test(s)).map(rel)
for (const name of ['display', 'audio', 'memory', 'network', 'sensors', 'storage', 'timers', 'app_platform', 'main']) cxx.push(`platform/main/rpios_${name}.cpp`)
cxx.push('framework/core/gea_app_entry.cpp')
for (const line of fs.readFileSync(path.join(generated, 'geatsc-sources.txt'), 'utf8').split(/\r?\n/).filter(Boolean)) {
  const file = path.resolve(line)
  if (!file.startsWith(path.resolve(generated) + path.sep)) throw new Error(`generated source outside output: ${file}`)
  cxx.push(`generated/${path.relative(generated, file).replaceAll('\\', '/')}`)
}
for (const name of ['gea_embedded_font_generated.cpp', 'gea_embedded_assets_generated.cpp']) {
  if (fs.existsSync(path.join(generated, name))) cxx.push(`generated/${name}`)
}
const includes = ['platform/include', 'generated', ...includeFlags(env).map((s) => rel(s.slice(2)))]
for (const file of [...c, ...cxx]) if (!fs.existsSync(path.join(stage, file))) throw new Error(`source missing: ${file}`)
const cmakeList = (key, values) => `set(${key}\n${values.map((s) => `  "${'${CMAKE_CURRENT_SOURCE_DIR}'}/${s}"`).join('\n')}\n)\n`
fs.writeFileSync(path.join(stage, 'sources.cmake'),
  `set(GEA_PACKAGE_NAME "${packageName}")\nset(GEA_APP_ID "${app.id}")\nset(GEA_APP_TITLE "${String(app.name || app.id).replaceAll('"', '')}")\n` +
  cmakeList('GEA_INCLUDE_DIRS', [...new Set(includes)]) + cmakeList('GEA_C_SOURCES', c) + cmakeList('GEA_CXX_SOURCES', cxx))
const icon = app.icons?.['128'] || app.icons?.['256'] || app.icons?.['512']
if (!icon || !fs.existsSync(path.join(appDir, icon))) throw new Error('app needs a PNG icon of at least 128px')
fs.copyFileSync(path.join(appDir, icon), path.join(stage, `${packageName}.png`))
fs.writeFileSync(path.join(stage, `${packageName}.desktop`), `[Desktop Entry]\nType=Application\nName=${app.name || app.id}\nExec=${packageName}\nIcon=${packageName}\n`)
fs.mkdirSync(path.join(stage, 'rpm'))
fs.writeFileSync(path.join(stage, 'rpm', `${packageName}.spec`), `Name: ${packageName}\nVersion: ${meta.version || '0.1.0'}\nRelease: 1\nSummary: Gea ${app.name || app.id} for Sailfish OS\nLicense: ${meta.license || 'MIT'}\nBuildRequires: cmake\nBuildRequires: pkgconfig(sdl2)\nBuildRequires: pkgconfig(libcurl)\n\n%description\nGea JSX application for Sailfish OS.\n\n%build\n%cmake .\n%make_build\n\n%install\n%make_install\n\n%files\n%{_bindir}/${packageName}\n%{_datadir}/applications/${packageName}.desktop\n%{_datadir}/icons/hicolor/128x128/apps/${packageName}.png\n`)
console.log(stage)

#!/usr/bin/env node
import fs from 'node:fs'
import path from 'node:path'
import { spawnSync } from 'node:child_process'
import { createRequire } from 'node:module'
import { fileURLToPath, pathToFileURL } from 'node:url'
import sharp from 'sharp'
import { desktopEntry, readSailfishConfig, rpmSpec, sailfishDensity } from './config.mjs'

const here = path.dirname(fileURLToPath(import.meta.url))
const linuxRoot = path.resolve(here, '../..')
const [appArg, arch = 'i486', coreArg] = process.argv.slice(2)
if (!appArg || !['i486', 'armv7hl', 'aarch64'].includes(arch)) {
  console.error('usage: node prepare.mjs <app-directory> [i486|armv7hl|aarch64] [core-repository]')
  process.exit(2)
}
const appDir = path.resolve(appArg)
const meta = JSON.parse(fs.readFileSync(path.join(appDir, 'package.json'), 'utf8'))
const config = readSailfishConfig(appDir, meta)
const density = sailfishDensity(config, arch)

// GeaStack packages resolve the way Node resolves them for the app, so an app
// that installs @geastack/linux from npm (with hoisted dependencies) works.
// This repository's own node_modules is the fallback for in-repo development.
const resolvers = [createRequire(path.join(appDir, 'package.json')), createRequire(path.join(linuxRoot, 'package.json'))]
const resolvePackage = (name) => {
  for (const resolver of resolvers) {
    try {
      return path.dirname(resolver.resolve(`${name}/package.json`))
    } catch {}
  }
  throw new Error(`cannot resolve ${name} from ${appDir} or ${linuxRoot}; run npm install in the app`)
}
const npmCore = resolvePackage('@geastack/core')
const npmCoreVersion = JSON.parse(fs.readFileSync(path.join(npmCore, 'package.json'), 'utf8')).version

// The framework sources come from the installed packages, or from a core
// checkout when one is given; a checkout must match the installed core.
const coreRepo = coreArg ? path.resolve(coreArg) : null
if (coreRepo) {
  const coreMetadata = path.join(coreRepo, 'packages/core/package.json')
  if (!fs.existsSync(coreMetadata)) throw new Error(`missing core package: ${coreMetadata}`)
  const checkoutVersion = JSON.parse(fs.readFileSync(coreMetadata, 'utf8')).version
  if (!checkoutVersion || checkoutVersion !== npmCoreVersion) {
    throw new Error(`core version mismatch: npm @geastack/core ${npmCoreVersion}, checkout ${checkoutVersion}. Use a core checkout matching the installed npm package, or install the matching npm version.`)
  }
}
const frameworkPackages = ['core', 'host', 'engine', 'elements', 'geaos']
const frameworkSource = (name) => coreRepo ? path.join(coreRepo, 'packages', name) : resolvePackage(`@geastack/${name}`)

if (!fs.existsSync(path.join(appDir, config.entry))) throw new Error(`app entry missing: ${config.entry}`)
// Build output belongs to the app, never to the installed package.
const output = path.join(appDir, '.gea-sailfish')
const generated = path.join(output, 'generated', config.id)
const stage = path.join(output, 'build', `${config.id}-${arch}`, 'project')
fs.mkdirSync(path.dirname(generated), { recursive: true })
const args = [path.join(npmCore, 'scripts/build-gea-vite-geatsc.mjs'),
  '--app-dir', appDir, '--entry', config.entry, '--out-dir', generated,
  '--geatsc-bin', path.join(resolvePackage('@geastack/compiler'), 'dist/cli.js'),
  '--geatsc-gea-plugin', path.join(resolvePackage('@geastack/geatsc-plugin-gea'), 'dist/index.js'),
  '--font-viewport-width', '410', '--font-viewport-height', '502',
  ...density.fontRatios.flatMap((ratio) => ['--font-device-pixel-ratio', String(ratio)]),
  '--runtime-ttf-fonts']
const result = spawnSync(process.execPath, args, { stdio: 'inherit', cwd: appDir })
if (result.status !== 0) process.exit(result.status || 1)

// sfdk mounts this project into its Linux build engine. Every compiled input
// must therefore be inside this directory, with no Windows absolute paths.
const resolvedStage = path.resolve(stage)
const allowedParent = path.resolve(output, 'build') + path.sep
if (!resolvedStage.startsWith(allowedParent)) throw new Error('unsafe stage directory')
fs.rmSync(stage, { recursive: true, force: true })
fs.mkdirSync(stage, { recursive: true })
const copyTree = (from, to) => fs.cpSync(from, to, { recursive: true, dereference: true,
  filter: (item) => !['.git', 'node_modules', 'dist', 'build', 'test', 'tests', '.build-test'].includes(path.basename(item)) })
for (const name of frameworkPackages) {
  const source = frameworkSource(name)
  if (!fs.existsSync(source)) throw new Error(`missing framework package: ${source}`)
  copyTree(source, path.join(stage, 'framework', name))
}
copyTree(generated, path.join(stage, 'generated'))
copyTree(path.join(here, 'main'), path.join(stage, 'platform/main'))
copyTree(path.join(here, 'include'), path.join(stage, 'platform/include'))
// The SDL keyboard producer is shared with the Raspberry Pi OS target.
fs.copyFileSync(path.join(here, '../raspberry-pi-os/main/rpios_keyboard.h'), path.join(stage, 'platform/include/rpios_keyboard.h'))
fs.copyFileSync(path.join(here, 'CMakeLists.txt'), path.join(stage, 'CMakeLists.txt'))

// App-owned native code keeps its app-relative layout under app_native/.
const appNative = (relative) => `app_native/${relative}`
for (const { relative, absolute } of [...config.nativeSources, ...config.nativeIncludeDirectories]) {
  const target = path.join(stage, appNative(relative))
  fs.mkdirSync(path.dirname(target), { recursive: true })
  fs.cpSync(absolute, target, { recursive: true, dereference: true })
}

const env = Object.fromEntries(frameworkPackages.map((name) => [
  ({ core: 'GEA_CORE', host: 'GEA_HOST_DIR', engine: 'GEA_ENGINE_DIR', elements: 'GEA_ELEMENTS_DIR', geaos: 'GEA_GEAOS_PACKAGE_DIR' })[name],
  path.join(stage, 'framework', name).replaceAll('\\', '/')]))
const { includeFlags, cSources, cxxSources } = await import(pathToFileURL(path.join(frameworkSource('core'), 'gea_sources.mjs')))
const rel = (file) => path.relative(stage, file).replaceAll('\\', '/')
const c = [...cSources(env).map(rel), 'platform/main/sailfish_apps.c']
const cxx = cxxSources({ ...env, GEA_DESKTOP_WEBSOCKET_CURL: '1' }).filter((s) => !/\/(?:host\/camera|runtime|services\/[a-z_]+)\.cpp$/.test(s)).map(rel)
if (!cxx.some((s) => s.endsWith('/host/websocket_curl.cpp'))) throw new Error('framework does not provide the native Linux WebSocket transport')
for (const name of ['display', 'audio', 'memory', 'network', 'sensors', 'storage', 'storage_bridge', 'timers', 'app_platform', 'keyboard', 'main']) cxx.push(`platform/main/sailfish_${name}.cpp`)
cxx.push('framework/core/gea_app_entry.cpp')
for (const line of fs.readFileSync(path.join(generated, 'geatsc-sources.txt'), 'utf8').split(/\r?\n/).filter(Boolean)) {
  const file = path.resolve(line)
  if (!file.startsWith(path.resolve(generated) + path.sep)) throw new Error(`generated source outside output: ${file}`)
  cxx.push(`generated/${path.relative(generated, file).replaceAll('\\', '/')}`)
}
for (const name of ['gea_embedded_font_generated.cpp', 'gea_embedded_assets_generated.cpp']) {
  if (fs.existsSync(path.join(generated, name))) cxx.push(`generated/${name}`)
}
for (const { relative } of config.nativeSources) (relative.endsWith('.c') ? c : cxx).push(appNative(relative))
const includes = ['platform/include', 'generated', ...includeFlags(env).map((s) => rel(s.slice(2))),
  ...config.nativeIncludeDirectories.map(({ relative }) => appNative(relative))]
for (const file of [...c, ...cxx]) if (!fs.existsSync(path.join(stage, file))) throw new Error(`source missing: ${file}`)
const cmakeList = (key, values) => `set(${key}\n${values.map((s) => `  "${'${CMAKE_CURRENT_SOURCE_DIR}'}/${s}"`).join('\n')}\n)\n`
const cmakeNames = (key, values) => `set(${key}${values.map((s) => ` "${s}"`).join('')})\n`
fs.writeFileSync(path.join(stage, 'sources.cmake'),
  `set(GEA_PACKAGE_NAME "${config.packageName}")\nset(GEA_APP_ID "${config.id}")\nset(GEA_APP_TITLE "${config.appName}")\nset(GEA_SAILFISH_ORGANIZATION_NAME "${config.organizationName}")\nset(GEA_SAILFISH_APPLICATION_NAME "${config.applicationName}")\n` +
  `set(GEA_SAILFISH_DEFAULT_DPR ${density.runtime})\n` +
  cmakeList('GEA_INCLUDE_DIRS', [...new Set(includes)]) + cmakeList('GEA_C_SOURCES', c) + cmakeList('GEA_CXX_SOURCES', cxx) +
  cmakeNames('GEA_APP_PKG_MODULES', config.pkgConfigModules) + cmakeNames('GEA_APP_SYSTEM_LIBRARIES', config.nativeSystemLibraries))
const icon = config.icons['512'] || config.icons['256'] || config.icons['128']
if (!icon || !fs.existsSync(path.join(appDir, icon))) throw new Error('app needs an icon source')
fs.mkdirSync(path.join(stage, 'icons'))
for (const size of [86, 108, 128, 172]) {
  await sharp(path.join(appDir, icon)).ensureAlpha().resize(size, size, {
    fit: 'contain', background: { r: 0, g: 0, b: 0, alpha: 0 },
  }).png().toFile(path.join(stage, 'icons', `${size}.png`))
}
fs.writeFileSync(path.join(stage, `${config.packageName}.desktop`), desktopEntry(config))
fs.mkdirSync(path.join(stage, 'rpm'))
fs.writeFileSync(path.join(stage, 'rpm', `${config.packageName}.spec`), rpmSpec(config))
console.log(stage)

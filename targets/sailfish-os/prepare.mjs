#!/usr/bin/env node
import fs from 'node:fs'
import path from 'node:path'
import { spawnSync } from 'node:child_process'
import { fileURLToPath, pathToFileURL } from 'node:url'
import sharp from 'sharp'
import {
  appRelative, assertPackageVersion, frameworkPackageNames, nativeKind,
  readPackageVersion, resolveGeastackPackage, validateLibraryName,
} from './prepare-lib.mjs'

const here = path.dirname(fileURLToPath(import.meta.url))
const linuxRoot = path.resolve(here, '../..')
const [appArg, arch = 'i486', coreArg] = process.argv.slice(2)
if (!appArg || !['i486', 'armv7hl', 'aarch64'].includes(arch)) {
  console.error('usage: node prepare.mjs <app-directory> [i486|armv7hl|aarch64] [core-repository]')
  process.exit(2)
}
const appDir = path.resolve(appArg)
const coreRepo = coreArg ? path.resolve(coreArg) : null
const meta = JSON.parse(fs.readFileSync(path.join(appDir, 'package.json'), 'utf8'))
const app = meta.gea
if (!app?.id || !/^[a-z0-9][a-z0-9-]*$/.test(app.id)) throw new Error('gea.id must be a lowercase package identifier')
const appName = String(app.name || app.id)
const rpmLicense = String(meta.license || 'MIT')
if (/[\x00-\x1f\x7f%\\";$]/.test(appName)) throw new Error('gea.name must not contain control characters or RPM/CMake syntax')
if (/[\x00-\x1f\x7f%]/.test(rpmLicense)) throw new Error('package license must not contain control characters or RPM macros')
const sailfish = app.sailfish || {}
const organizationName = sailfish.organizationName || 'org.geastack'
const applicationName = sailfish.applicationName || app.id.replaceAll('-', '_')
const permissions = sailfish.permissions || []
const devicePixelRatio = sailfish.devicePixelRatio ?? 1
if (typeof devicePixelRatio !== 'number' || !Number.isFinite(devicePixelRatio) || devicePixelRatio < 0.5 || devicePixelRatio > 8) throw new Error('gea.sailfish.devicePixelRatio must be a number between 0.5 and 8')
if (!/^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)+$/.test(organizationName)) throw new Error('gea.sailfish.organizationName must be a reverse-domain name')
if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(applicationName)) throw new Error('gea.sailfish.applicationName must contain only letters, digits, and underscores')
if (!Array.isArray(permissions) || !permissions.every((item) => typeof item === 'string' && /^[A-Za-z]+$/.test(item))) throw new Error('gea.sailfish.permissions must be an array of permission names')
const nativeSources = sailfish.nativeSources || []
const includePaths = sailfish.includePaths || []
const libraries = sailfish.libraries || []
if (!Array.isArray(nativeSources) || !nativeSources.every((item) => typeof item === 'string')) throw new Error('gea.sailfish.nativeSources must be an array of app-relative paths')
if (!Array.isArray(includePaths) || !includePaths.every((item) => typeof item === 'string')) throw new Error('gea.sailfish.includePaths must be an array of app-relative directories')
if (!Array.isArray(libraries)) throw new Error('gea.sailfish.libraries must be an array of pkg-config module names')
for (const name of libraries) validateLibraryName(name)
const npmVersion = meta.version || '0.1.0'
const versionMatch = typeof npmVersion === 'string' && npmVersion.match(/^(\d+\.\d+\.\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$/)
if (!versionMatch) throw new Error('package.json version must be a valid x.y.z version with optional prerelease and build metadata')
const rpmVersion = versionMatch[1] + (versionMatch[2] ? `~${versionMatch[2].replaceAll('-', '.')}` : '')
const entry = app.entry || 'index.tsx'
if (!fs.existsSync(path.join(appDir, entry))) throw new Error(`app entry missing: ${entry}`)
const packageName = `harbour-gea-${app.id}`

const npmPackages = Object.fromEntries(frameworkPackageNames().map((name) => [name, resolveGeastackPackage(appDir, linuxRoot, name)]))
const compilerDir = resolveGeastackPackage(appDir, linuxRoot, 'compiler')
const pluginDir = resolveGeastackPackage(appDir, linuxRoot, 'geatsc-plugin-gea')
if (coreRepo) {
  for (const name of frameworkPackageNames()) {
    const checkoutMeta = path.join(coreRepo, 'packages', name, 'package.json')
    if (!fs.existsSync(checkoutMeta)) throw new Error(`missing @geastack/${name} checkout package: ${checkoutMeta}`)
    assertPackageVersion(name, readPackageVersion(path.join(npmPackages[name], 'package.json')), readPackageVersion(checkoutMeta))
  }
}
const frameworkSource = (name) => coreRepo
  ? path.join(coreRepo, 'packages', name)
  : npmPackages[name]

const workRoot = path.join(appDir, '.gea-sailfish')
const generated = path.join(workRoot, 'generated', app.id)
const stage = path.join(workRoot, 'build', `${app.id}-${arch}`, 'project')
fs.mkdirSync(path.dirname(generated), { recursive: true })
const args = [path.join(npmPackages.core, 'scripts/build-gea-vite-geatsc.mjs'),
  '--app-dir', appDir, '--entry', entry, '--out-dir', generated,
  '--geatsc-bin', path.join(compilerDir, 'dist/cli.js'),
  '--geatsc-gea-plugin', path.join(pluginDir, 'dist/index.js'),
  '--font-viewport-width', '410', '--font-viewport-height', '502', '--font-device-pixel-ratio', String(devicePixelRatio),
  '--runtime-ttf-fonts']
const usingAppPackages = fs.existsSync(path.join(appDir, 'node_modules', '@geastack', 'core', 'package.json'))
const result = spawnSync(process.execPath, args, { stdio: 'inherit', cwd: usingAppPackages ? appDir : linuxRoot })
if (result.status !== 0) process.exit(result.status || 1)

// sfdk mounts this project into its Linux build engine. Every compiled input
// must therefore be inside this directory, with no Windows absolute paths.
const resolvedStage = path.resolve(stage)
const allowedParent = path.resolve(workRoot, 'build') + path.sep
if (!resolvedStage.startsWith(allowedParent)) throw new Error('unsafe stage directory')
fs.rmSync(stage, { recursive: true, force: true })
fs.mkdirSync(stage, { recursive: true })
const copyTree = (from, to) => fs.cpSync(from, to, { recursive: true,
  filter: (item) => !['.git', 'node_modules', 'dist', 'build', 'test', 'tests', '.build-test'].includes(path.basename(item)) })
for (const name of frameworkPackageNames()) {
  const source = frameworkSource(name)
  if (!fs.existsSync(source)) throw new Error(`missing @geastack/${name}: ${source}`)
  copyTree(source, path.join(stage, 'framework', name))
}
copyTree(generated, path.join(stage, 'generated'))
copyTree(path.join(here, 'main'), path.join(stage, 'platform/main'))
copyTree(path.join(here, 'include'), path.join(stage, 'platform/include'))
fs.copyFileSync(path.join(here, 'CMakeLists.txt'), path.join(stage, 'CMakeLists.txt'))

const nativeRoot = path.join(stage, 'app_native')
const stagedNative = []
for (const rel of nativeSources) {
  const abs = appRelative(appDir, rel)
  if (!fs.existsSync(abs) || !fs.statSync(abs).isFile()) throw new Error(`gea.sailfish.nativeSources file missing: ${rel}`)
  nativeKind(rel)
  const dest = path.join(nativeRoot, rel)
  fs.mkdirSync(path.dirname(dest), { recursive: true })
  fs.copyFileSync(abs, dest)
  stagedNative.push(rel.replaceAll('\\', '/'))
}
const stagedIncludes = []
for (const rel of includePaths) {
  const abs = appRelative(appDir, rel)
  if (!fs.existsSync(abs) || !fs.statSync(abs).isDirectory()) throw new Error(`gea.sailfish.includePaths directory missing: ${rel}`)
  const dest = path.join(nativeRoot, rel)
  copyTree(abs, dest)
  stagedIncludes.push(`app_native/${rel.replaceAll('\\', '/')}`)
}

const env = Object.fromEntries(frameworkPackageNames().map((name) => [
  ({ core: 'GEA_CORE', host: 'GEA_HOST_DIR', engine: 'GEA_ENGINE_DIR', elements: 'GEA_ELEMENTS_DIR', geaos: 'GEA_GEAOS_PACKAGE_DIR' })[name],
  path.join(stage, 'framework', name).replaceAll('\\', '/')]))
const sourcesModule = coreRepo
  ? path.join(coreRepo, 'packages/core/gea_sources.mjs')
  : path.join(npmPackages.core, 'gea_sources.mjs')
const { includeFlags, cSources, cxxSources } = await import(pathToFileURL(sourcesModule))
const relStage = (file) => path.relative(stage, file).replaceAll('\\', '/')
const c = [...cSources(env).map(relStage), 'platform/main/sailfish_apps.c']
const cxx = cxxSources({ ...env, GEA_DESKTOP_WEBSOCKET_CURL: '1' }).filter((s) => !/\/(?:host\/camera|runtime|services\/[a-z_]+)\.cpp$/.test(s)).map(relStage)
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
for (const rel of stagedNative) {
  const kind = nativeKind(rel)
  const staged = `app_native/${rel}`
  if (kind === 'c') c.push(staged)
  else if (kind === 'cxx') cxx.push(staged)
  const parent = path.posix.dirname(rel)
  if (parent && parent !== '.') stagedIncludes.push(`app_native/${parent}`)
}
const includes = ['platform/include', 'generated', ...stagedIncludes, ...includeFlags(env).map((s) => relStage(s.slice(2)))]
if (stagedNative.length > 0) includes.push('app_native')
for (const file of [...c, ...cxx]) if (!fs.existsSync(path.join(stage, file))) throw new Error(`source missing: ${file}`)
const cmakeList = (key, values) => `set(${key}\n${values.map((s) => `  "${'${CMAKE_CURRENT_SOURCE_DIR}'}/${s}"`).join('\n')}\n)\n`
const cmakePlainList = (key, values) => `set(${key}\n${values.map((s) => `  "${s}"`).join('\n')}\n)\n`
fs.writeFileSync(path.join(stage, 'sources.cmake'),
  `set(GEA_PACKAGE_NAME "${packageName}")\nset(GEA_APP_ID "${app.id}")\nset(GEA_APP_TITLE "${appName}")\nset(GEA_SAILFISH_ORGANIZATION_NAME "${organizationName}")\nset(GEA_SAILFISH_APPLICATION_NAME "${applicationName}")\n` +
  `set(GEA_SAILFISH_DEFAULT_DPR ${devicePixelRatio})\n` +
  cmakePlainList('GEA_APP_LIBRARIES', libraries) +
  cmakeList('GEA_INCLUDE_DIRS', [...new Set(includes)]) + cmakeList('GEA_C_SOURCES', c) + cmakeList('GEA_CXX_SOURCES', cxx))
const icon = app.icons?.['512'] || app.icons?.['256'] || app.icons?.['128']
if (!icon || !fs.existsSync(path.join(appDir, icon))) throw new Error('app needs an icon source')
fs.mkdirSync(path.join(stage, 'icons'))
for (const size of [86, 108, 128, 172]) {
  await sharp(path.join(appDir, icon)).ensureAlpha().resize(size, size, {
    fit: 'contain', background: { r: 0, g: 0, b: 0, alpha: 0 },
  }).png().toFile(path.join(stage, 'icons', `${size}.png`))
}
fs.writeFileSync(path.join(stage, `${packageName}.desktop`), `[Desktop Entry]\nType=Application\nName=${appName}\nExec=${packageName}\nIcon=${packageName}\nX-Nemo-Application-Type=generic\n\n[X-Sailjail]\nOrganizationName=${organizationName}\nApplicationName=${applicationName}\nPermissions=${[...new Set(permissions)].join(';')}\n`)
const libraryRequires = libraries.map((name) => `BuildRequires: pkgconfig(${name})\n`).join('')
fs.mkdirSync(path.join(stage, 'rpm'))
fs.writeFileSync(path.join(stage, 'rpm', `${packageName}.spec`), `Name: ${packageName}\nVersion: ${rpmVersion}\nRelease: 1\nSummary: Gea ${appName} for Sailfish OS\nLicense: ${rpmLicense}\nBuildRequires: cmake\nBuildRequires: pkgconfig(sdl2)\nBuildRequires: pkgconfig(libcurl)\nBuildRequires: pkgconfig(glib-2.0)\nBuildRequires: pkgconfig(gobject-2.0)\nBuildRequires: pkgconfig(gio-2.0)\n${libraryRequires}\n%description\nGea JSX application for Sailfish OS.\n\n%build\n%cmake .\n%make_build\n\n%install\n%make_install\n\n%files\n%{_bindir}/${packageName}\n%{_datadir}/applications/${packageName}.desktop\n%{_datadir}/icons/hicolor/*/apps/${packageName}.png\n`)
console.log(stage)

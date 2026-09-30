#!/usr/bin/env node
import fs from 'node:fs'
import path from 'node:path'
import { spawnSync } from 'node:child_process'
import { fileURLToPath, pathToFileURL } from 'node:url'
import sharp from 'sharp'

const here = path.dirname(fileURLToPath(import.meta.url))
const linuxRoot = path.resolve(here, '../..')
const [appArg, arch = 'i486', coreArg] = process.argv.slice(2)
if (!appArg || !['i486', 'armv7hl', 'aarch64'].includes(arch)) {
  console.error('usage: node prepare.mjs <app-directory> [i486|armv7hl|aarch64] [core-repository]')
  process.exit(2)
}
const appDir = path.resolve(appArg)
const coreRepo = path.resolve(coreArg || path.join(linuxRoot, '../core'))
const corePackage = path.join(coreRepo, 'packages/core')
const npmCore = path.join(linuxRoot, 'node_modules/@geastack/core')
const coreMetadata = path.join(corePackage, 'package.json')
if (!fs.existsSync(coreMetadata)) throw new Error(`missing core package: ${coreMetadata}`)
const checkoutVersion = JSON.parse(fs.readFileSync(coreMetadata, 'utf8')).version
const installedVersion = JSON.parse(fs.readFileSync(path.join(npmCore, 'package.json'), 'utf8')).version
if (!checkoutVersion || checkoutVersion !== installedVersion) {
  throw new Error(`core version mismatch: npm @geastack/core ${installedVersion}, checkout ${checkoutVersion}. Use a core checkout matching the installed npm package, or install the matching npm version.`)
}
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
const npmVersion = meta.version || '0.1.0'
const versionMatch = typeof npmVersion === 'string' && npmVersion.match(/^(\d+\.\d+\.\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$/)
if (!versionMatch) throw new Error('package.json version must be a valid x.y.z version with optional prerelease and build metadata')
const rpmVersion = versionMatch[1] + (versionMatch[2] ? `~${versionMatch[2].replaceAll('-', '.')}` : '')
const entry = app.entry || 'index.tsx'
if (!fs.existsSync(path.join(appDir, entry))) throw new Error(`app entry missing: ${entry}`)
const packageName = `harbour-gea-${app.id}`
const generated = path.join(here, 'generated', app.id)
const stage = path.join(here, 'build', `${app.id}-${arch}`, 'project')
fs.mkdirSync(path.dirname(generated), { recursive: true })
// The npm scripts/runtime and native checkout must have the same core version.
const args = [path.join(npmCore, 'scripts/build-gea-vite-geatsc.mjs'),
  '--app-dir', appDir, '--entry', entry, '--out-dir', generated,
  '--geatsc-bin', path.join(linuxRoot, 'node_modules/@geastack/compiler/dist/cli.js'),
  '--geatsc-gea-plugin', path.join(linuxRoot, 'node_modules/@geastack/geatsc-plugin-gea/dist/index.js'),
  '--font-viewport-width', '410', '--font-viewport-height', '502', '--font-device-pixel-ratio', String(devicePixelRatio),
  '--runtime-ttf-fonts']
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
  filter: (item) => !['.git', 'node_modules', 'dist', 'build', 'test', 'tests', '.build-test'].includes(path.basename(item)) })
for (const name of ['core', 'host', 'engine', 'elements', 'geaos']) {
  const source = path.join(coreRepo, 'packages', name)
  if (!fs.existsSync(source)) throw new Error(`missing framework package: ${source}`)
  copyTree(source, path.join(stage, 'framework', name))
}
copyTree(generated, path.join(stage, 'generated'))
copyTree(path.join(here, 'main'), path.join(stage, 'platform/main'))
copyTree(path.join(here, 'include'), path.join(stage, 'platform/include'))
fs.copyFileSync(path.join(here, 'CMakeLists.txt'), path.join(stage, 'CMakeLists.txt'))

const env = Object.fromEntries(['core', 'host', 'engine', 'elements', 'geaos'].map((name) => [
  ({ core: 'GEA_CORE', host: 'GEA_HOST_DIR', engine: 'GEA_ENGINE_DIR', elements: 'GEA_ELEMENTS_DIR', geaos: 'GEA_GEAOS_PACKAGE_DIR' })[name],
  path.join(stage, 'framework', name).replaceAll('\\', '/')]))
const { includeFlags, cSources, cxxSources } = await import(pathToFileURL(path.join(coreRepo, 'packages/core/gea_sources.mjs')))
const rel = (file) => path.relative(stage, file).replaceAll('\\', '/')
const c = [...cSources(env).map(rel), 'platform/main/sailfish_apps.c']
const cxx = cxxSources(env).filter((s) => !/\/(?:host\/camera|runtime|services\/[a-z_]+)\.cpp$/.test(s)).map(rel)
for (const name of ['display', 'audio', 'memory', 'network', 'sensors', 'storage', 'storage_bridge', 'timers', 'app_platform', 'main']) cxx.push(`platform/main/sailfish_${name}.cpp`)
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
  `set(GEA_PACKAGE_NAME "${packageName}")\nset(GEA_APP_ID "${app.id}")\nset(GEA_APP_TITLE "${appName}")\nset(GEA_SAILFISH_ORGANIZATION_NAME "${organizationName}")\nset(GEA_SAILFISH_APPLICATION_NAME "${applicationName}")\n` +
  `set(GEA_SAILFISH_DEFAULT_DPR ${devicePixelRatio})\n` +
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
fs.mkdirSync(path.join(stage, 'rpm'))
fs.writeFileSync(path.join(stage, 'rpm', `${packageName}.spec`), `Name: ${packageName}\nVersion: ${rpmVersion}\nRelease: 1\nSummary: Gea ${appName} for Sailfish OS\nLicense: ${rpmLicense}\nBuildRequires: cmake\nBuildRequires: pkgconfig(sdl2)\nBuildRequires: pkgconfig(libcurl)\n\n%description\nGea JSX application for Sailfish OS.\n\n%build\n%cmake .\n%make_build\n\n%install\n%make_install\n\n%files\n%{_bindir}/${packageName}\n%{_datadir}/applications/${packageName}.desktop\n%{_datadir}/icons/hicolor/*/apps/${packageName}.png\n`)
console.log(stage)

// Reads and validates an app's package.json for the Sailfish OS target, and
// renders the .desktop entry and RPM spec. No side effects besides reading the
// files the app names, so it can be tested without the SDK or the framework.
import fs from 'node:fs'
import path from 'node:path'

const NATIVE_SOURCE = /\.(c|cc|cpp|cxx)$/
const LIBRARY_NAME = /^[A-Za-z][A-Za-z0-9_.+-]*$/
const PKG_CONFIG_MODULE = /^[A-Za-z0-9][A-Za-z0-9_.+-]*$/

const stringList = (value, name) => {
  if (value === undefined) return []
  if (!Array.isArray(value) || value.some((item) => typeof item !== 'string' || !item)) throw new Error(`${name} must be an array of nonempty strings`)
  return [...new Set(value)]
}

// An app-relative path that stays inside the app after symlinks are resolved.
const appPath = (appDir, value, name, kind) => {
  if (path.isAbsolute(value) || value.split(/[\\/]/).includes('..') || /[\x00-\x1f\x7f"$;\\]/.test(value)) throw new Error(`${name}: invalid path ${value}`)
  const realApp = fs.realpathSync(appDir)
  let real
  try {
    real = fs.realpathSync(path.join(appDir, value))
  } catch {
    throw new Error(`${name}: missing ${value}`)
  }
  const relative = path.relative(realApp, real)
  if (!relative || relative.startsWith('..') || path.isAbsolute(relative)) throw new Error(`${name}: ${value} escapes the app directory`)
  const stat = fs.statSync(real)
  if (kind === 'file' ? !stat.isFile() : !stat.isDirectory()) throw new Error(`${name}: ${value} is not a ${kind === 'file' ? 'regular file' : 'directory'}`)
  return { relative: relative.split(path.sep).join('/'), absolute: real }
}

export function readSailfishConfig(appDir, meta) {
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
  const devicePixelRatio = sailfish.devicePixelRatio ?? 'auto'
  const sandboxing = sailfish.sandboxing ?? true
  if (devicePixelRatio !== 'auto' && (typeof devicePixelRatio !== 'number' || !Number.isFinite(devicePixelRatio) || devicePixelRatio < 0.5 || devicePixelRatio > 8)) throw new Error('gea.sailfish.devicePixelRatio must be "auto" or a number between 0.5 and 8')
  if (!/^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)+$/.test(organizationName)) throw new Error('gea.sailfish.organizationName must be a reverse-domain name')
  if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(applicationName)) throw new Error('gea.sailfish.applicationName must contain only letters, digits, and underscores')
  if (!Array.isArray(permissions) || !permissions.every((item) => typeof item === 'string' && /^[A-Za-z]+$/.test(item))) throw new Error('gea.sailfish.permissions must be an array of permission names')
  if (typeof sandboxing !== 'boolean') throw new Error('gea.sailfish.sandboxing must be true or false')

  const npmVersion = meta.version || '0.1.0'
  const versionMatch = typeof npmVersion === 'string' && npmVersion.match(/^(\d+\.\d+\.\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$/)
  if (!versionMatch) throw new Error('package.json version must be a valid x.y.z version with optional prerelease and build metadata')
  const rpmVersion = versionMatch[1] + (versionMatch[2] ? `~${versionMatch[2].replaceAll('-', '.')}` : '')

  const nativeSources = stringList(app.nativeSources, 'gea.nativeSources').map((value) => {
    if (!NATIVE_SOURCE.test(value)) throw new Error(`gea.nativeSources: unsupported source ${value} (use .c, .cc, .cpp or .cxx)`)
    return appPath(appDir, value, 'gea.nativeSources', 'file')
  })
  const nativeIncludeDirectories = stringList(sailfish.nativeIncludeDirectories, 'gea.sailfish.nativeIncludeDirectories')
    .map((value) => appPath(appDir, value, 'gea.sailfish.nativeIncludeDirectories', 'directory'))
  const nativeSystemLibraries = stringList(sailfish.nativeSystemLibraries, 'gea.sailfish.nativeSystemLibraries')
  for (const name of nativeSystemLibraries) if (!LIBRARY_NAME.test(name)) throw new Error(`gea.sailfish.nativeSystemLibraries: invalid library ${name}`)
  const pkgConfigModules = stringList(sailfish.pkgConfigModules, 'gea.sailfish.pkgConfigModules')
  for (const name of pkgConfigModules) if (!PKG_CONFIG_MODULE.test(name)) throw new Error(`gea.sailfish.pkgConfigModules: invalid module ${name}`)

  return {
    id: app.id,
    entry: app.entry || 'index.tsx',
    icons: app.icons || {},
    appName,
    rpmLicense,
    rpmVersion,
    packageName: `harbour-gea-${app.id}`,
    organizationName,
    applicationName,
    permissions: [...new Set(permissions)],
    devicePixelRatio,
    sandboxing,
    nativeSources,
    nativeIncludeDirectories,
    nativeSystemLibraries,
    pkgConfigModules,
  }
}

// Integer DPRs an "auto" build is expected to pick on Sailfish phones: 2 on
// 720-pixel-wide panels (Jolla C2) and 3 on 1080-pixel-wide panels (Xperia XA2,
// Xperia 10 through 10 V, Jolla Phone). Build-time fonts are generated for both;
// other sizes fall back to the runtime TTF rasterizer.
const PHONE_DENSITIES = [2, 3]

// The DPR compiled into the executable (0: derive an integer DPR from the
// panel at startup, see sailfish_main.cpp) and the ratios fonts are generated
// for. "auto" is panel-derived on ARM phones; the x86 emulator keeps one CSS
// pixel per canvas pixel.
export function sailfishDensity(config, arch) {
  const ratio = config.devicePixelRatio
  if (ratio !== 'auto') return { runtime: ratio, fontRatios: [ratio] }
  if (arch === 'i486') return { runtime: 1, fontRatios: [1] }
  return { runtime: 0, fontRatios: [...PHONE_DENSITIES] }
}

export function desktopEntry(config) {
  const sailjail = [
    ...(config.sandboxing ? [] : ['Sandboxing=Disabled']),
    `OrganizationName=${config.organizationName}`,
    `ApplicationName=${config.applicationName}`,
    `Permissions=${config.permissions.join(';')}`,
  ]
  return `[Desktop Entry]\nType=Application\nName=${config.appName}\nExec=${config.packageName}\nIcon=${config.packageName}\nX-Nemo-Application-Type=generic\n\n[X-Sailjail]\n${sailjail.join('\n')}\n`
}

export function rpmSpec(config) {
  const buildRequires = [...new Set(['cmake', 'pkgconfig(sdl2)', 'pkgconfig(libcurl)', 'pkgconfig(maliit-glib)', ...config.pkgConfigModules.map((name) => `pkgconfig(${name})`)])]
  return `Name: ${config.packageName}\nVersion: ${config.rpmVersion}\nRelease: 1\nSummary: Gea ${config.appName} for Sailfish OS\nLicense: ${config.rpmLicense}\n${buildRequires.map((item) => `BuildRequires: ${item}\n`).join('')}\n%description\nGea JSX application for Sailfish OS.\n\n%build\n%cmake .\n%make_build\n\n%install\n%make_install\n\n%files\n%{_bindir}/${config.packageName}\n%{_datadir}/applications/${config.packageName}.desktop\n%{_datadir}/icons/hicolor/*/apps/${config.packageName}.png\n`
}

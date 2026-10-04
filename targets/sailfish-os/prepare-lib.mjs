import fs from 'node:fs'
import path from 'node:path'

const frameworkPackages = ['core', 'host', 'engine', 'elements', 'geaos']

export function frameworkPackageNames() {
  return frameworkPackages
}

export function appRelative(appDir, rel) {
  if (typeof rel !== 'string' || rel.length === 0 || path.isAbsolute(rel)) {
    throw new Error(`Sailfish native path must be relative to the app: ${rel}`)
  }
  if (rel.split(/[\\/]/).includes('..')) {
    throw new Error(`Sailfish native path must stay inside the app: ${rel}`)
  }
  const root = path.resolve(appDir)
  const abs = path.resolve(root, rel)
  if (abs !== root && !abs.startsWith(root + path.sep)) {
    throw new Error(`Sailfish native path must stay inside the app: ${rel}`)
  }
  return abs
}

export function resolveGeastackPackage(appDir, linuxRoot, name) {
  const candidates = [
    path.join(appDir, 'node_modules', '@geastack', name),
    path.join(linuxRoot, 'node_modules', '@geastack', name),
  ]
  for (const dir of candidates) {
    if (fs.existsSync(path.join(dir, 'package.json'))) return dir
  }
  throw new Error(`missing @geastack/${name}. Run npm ci in the app so the locked package is installed.`)
}

export function readPackageVersion(packageJsonPath) {
  if (!fs.existsSync(packageJsonPath)) return ''
  const version = JSON.parse(fs.readFileSync(packageJsonPath, 'utf8')).version
  return typeof version === 'string' ? version : ''
}

export function assertPackageVersion(name, installedVersion, checkoutVersion) {
  if (!installedVersion || !checkoutVersion || installedVersion !== checkoutVersion) {
    throw new Error(`@geastack/${name} version mismatch: app package ${installedVersion || 'missing'}, checkout ${checkoutVersion || 'missing'}. Use a checkout matching the app's installed package, or omit the core checkout to build from the app's packages.`)
  }
}

export function validateLibraryName(name) {
  if (typeof name !== 'string' || !/^[A-Za-z0-9][A-Za-z0-9_.+-]*$/.test(name)) {
    throw new Error(`gea.sailfish.libraries entries must be pkg-config module names: ${name}`)
  }
  return name
}

export function nativeKind(file) {
  const ext = path.extname(file).toLowerCase()
  if (ext === '.c') return 'c'
  if (ext === '.cc' || ext === '.cpp' || ext === '.cxx') return 'cxx'
  if (ext === '.h' || ext === '.hh' || ext === '.hpp' || ext === '.hxx') return 'header'
  throw new Error(`gea.sailfish.nativeSources files must be C, C++, or headers: ${file}`)
}

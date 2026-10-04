#!/usr/bin/env node
// Drop object files an interrupted sfdk/cmake run left empty or truncated.
// sfdk otherwise treats a zero-byte .o as up to date and the link fails with
// undefined references. --clean also removes the staged RPM/CMake outputs.
import fs from 'node:fs'
import path from 'node:path'

const args = process.argv.slice(2)
const clean = args.includes('--clean')
const projectArg = args.find((arg) => arg !== '--clean')
if (!projectArg) {
  console.error('usage: node recover-build.mjs <staged-project> [--clean]')
  process.exit(2)
}
const project = path.resolve(projectArg)
if (!fs.existsSync(project)) {
  console.error(`staged project missing: ${project}`)
  process.exit(1)
}

function walk(dir, files) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name)
    if (entry.isDirectory()) {
      if (entry.name === 'node_modules' || entry.name === '.git') continue
      walk(full, files)
    } else if (entry.isFile() && entry.name.endsWith('.o')) {
      files.push(full)
    }
  }
}

function isElfObject(file) {
  const stat = fs.statSync(file)
  if (!stat.isFile() || stat.size < 4) return false
  const fd = fs.openSync(file, 'r')
  try {
    const header = Buffer.alloc(4)
    if (fs.readSync(fd, header, 0, 4, 0) !== 4) return false
    return header[0] === 0x7f && header[1] === 0x45 && header[2] === 0x4c && header[3] === 0x46
  } finally {
    fs.closeSync(fd)
  }
}

if (clean) {
  for (const name of ['RPMS', 'SRPMS', 'BUILD', 'build']) {
    fs.rmSync(path.join(project, name), { recursive: true, force: true })
  }
  const cmakeDirs = []
  const visit = (dir) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      const full = path.join(dir, entry.name)
      if (!entry.isDirectory()) continue
      if (entry.name === 'CMakeFiles') cmakeDirs.push(full)
      else if (entry.name !== 'node_modules' && entry.name !== '.git') visit(full)
    }
  }
  visit(project)
  for (const dir of cmakeDirs) fs.rmSync(dir, { recursive: true, force: true })
  for (const name of ['CMakeCache.txt', 'cmake_install.cmake', 'Makefile']) {
    const file = path.join(project, name)
    if (fs.existsSync(file)) fs.rmSync(file, { force: true })
  }
}

const objects = []
walk(project, objects)
let removed = 0
for (const file of objects) {
  if (clean || !isElfObject(file)) {
    fs.rmSync(file, { force: true })
    removed += 1
  }
}
if (removed > 0 || clean) {
  console.error(`[sailfish] ${clean ? 'cleaned staged build; ' : ''}removed ${removed} object file(s)`)
}

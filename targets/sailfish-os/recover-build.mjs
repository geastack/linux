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

function readElfNumber(buf, offset, bytes, little) {
  if (bytes === 2) return little ? buf.readUInt16LE(offset) : buf.readUInt16BE(offset)
  if (bytes === 4) return little ? buf.readUInt32LE(offset) : buf.readUInt32BE(offset)
  const value = little ? buf.readBigUInt64LE(offset) : buf.readBigUInt64BE(offset)
  return value <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(value) : -1
}

function elfTableFits(fileSize, offset, count, entrySize) {
  if (offset < 0 || offset > fileSize) return false
  if (count === 0) return true
  if (entrySize <= 0) return false
  return count <= Math.floor((fileSize - offset) / entrySize)
}

function isElfObject(file) {
  const stat = fs.statSync(file)
  // A complete ELF header is 52 bytes (32-bit) or 64 bytes (64-bit). Magic alone is truncated.
  if (!stat.isFile() || stat.size < 52) return false
  const fd = fs.openSync(file, 'r')
  try {
    const header = Buffer.alloc(64)
    const bytes = fs.readSync(fd, header, 0, Math.min(header.length, stat.size), 0)
    if (header[0] !== 0x7f || header[1] !== 0x45 || header[2] !== 0x4c || header[3] !== 0x46) return false
    const elfClass = header[4]
    const elfData = header[5]
    const little = elfData === 1
    if ((elfClass !== 1 && elfClass !== 2) || (elfData !== 1 && elfData !== 2) || header[6] !== 1) return false
    const headerSize = elfClass === 2 ? 64 : 52
    if (bytes < headerSize) return false
    if (readElfNumber(header, elfClass === 2 ? 52 : 40, 2, little) !== headerSize) return false
    const wide = elfClass === 2
    const phoff = readElfNumber(header, wide ? 32 : 28, wide ? 8 : 4, little)
    const shoff = readElfNumber(header, wide ? 40 : 32, wide ? 8 : 4, little)
    const phentsize = readElfNumber(header, wide ? 54 : 42, 2, little)
    const phnum = readElfNumber(header, wide ? 56 : 44, 2, little)
    const shentsize = readElfNumber(header, wide ? 58 : 46, 2, little)
    const shnum = readElfNumber(header, wide ? 60 : 48, 2, little)
    return elfTableFits(stat.size, phoff, phnum, phentsize) && elfTableFits(stat.size, shoff, shnum, shentsize)
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

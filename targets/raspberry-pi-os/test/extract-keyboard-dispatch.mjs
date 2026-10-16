import fs from 'node:fs';import path from 'node:path';import {fileURLToPath} from 'node:url';
const target=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..'),file=path.join(target,'main/rpios_main.cpp'),source=fs.readFileSync(file,'utf8')
const start=source.indexOf('void dispatchKeyInput()'),open=source.indexOf('{',start);let end=open+1,depth=1;for(;depth;end++){if(source[end]==='{')depth++;if(source[end]==='}')depth--}
if(start<0)throw Error('Key drain absent');const out=path.resolve(target,'../../dist/keyboard');fs.mkdirSync(out,{recursive:true});fs.writeFileSync(path.join(out,'keyboard-dispatch.inc'),source.slice(start,end)+'\n')
if(/if\s*\(sym\s*==\s*SDLK_ESCAPE\)/.test(source))throw Error('Escape must not bypass native event dispatch to quit')
for(const needle of ['case SDL_KEYUP:','case SDL_TEXTEDITING:','case SDL_TEXTEDITING_EXT:','g_keyboard.accept(event)','case SDL_WINDOWEVENT_CLOSE:'])if(!source.includes(needle))throw Error('Missing main wiring: '+needle)

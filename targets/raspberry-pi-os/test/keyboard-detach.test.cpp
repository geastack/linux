// Actual SDL structs + production Producer + real Core focus/topology. No SDL init/OS devices.
#include "rpios_keyboard.h"
#include <cassert>
#include <cstdio>
namespace gea::framework::graphics::generated {void ensureLinked(){}}
using namespace gea::embedded::ui;
using namespace gea::framework::events;
static SDL_Event key(){SDL_Event e{};e.type=SDL_KEYDOWN;e.key.type=SDL_KEYDOWN;e.key.windowID=7;e.key.keysym.sym=SDLK_BACKSPACE;e.key.keysym.scancode=SDL_SCANCODE_BACKSPACE;return e;}
static SDL_Event text(const char*value,Uint32 type=SDL_TEXTINPUT){SDL_Event e{};e.type=type;if(type==SDL_TEXTINPUT){e.text.windowID=7;std::strncpy(e.text.text,value,sizeof(e.text.text)-1);}else{e.edit.windowID=7;std::strncpy(e.edit.text,value,sizeof(e.edit.text)-1);}return e;}
int main(){auto&t=Tree::instance();auto&d=Document::instance();t.clear();auto root=d.querySelector("#app"),wrapper=d.createElement("div"),input=d.createElement("input"),other=d.createElement("div");root.appendChild(wrapper);wrapper.appendChild(input);root.appendChild(other);d.mount(root,320,240);input.setAttribute("value","seed");input.focus();gea::rpios::keyboard::Producer producer;producer.bind(7,true);int keys=0,inputs=0;bool detachInKey=false;input.addEventListener("keydown",[&](PointerEvent&e){++keys;assert(!e.isComposing);if(detachInKey){detachInKey=false;wrapper.removeChild(input);wrapper.appendChild(input);input.focus();}});input.addEventListener("input",[&](auto&){++inputs;});
 assert(producer.accept(text("composing",SDL_TEXTEDITING)));assert(producer.accept(key()));assert(producer.accept(text("old")));const void*birth=input.identity();wrapper.removeChild(input);wrapper.appendChild(input);input.focus();assert(input.identity()==birth);producer.drain();assert(keys==0&&inputs==0&&std::string(input.getAttribute("value"))=="seed");
 assert(producer.accept(key()));assert(producer.accept(text("z")));producer.drain();assert(keys==1&&inputs==2&&std::string(input.getAttribute("value"))=="seez");
 detachInKey=true;assert(producer.accept(key()));assert(producer.accept(text("stale")));producer.drain();assert(keys==2&&inputs==2&&std::string(input.getAttribute("value"))=="seez");
 assert(producer.accept(key()));assert(producer.accept(text("r")));root.replaceChildren({input,other});producer.drain();assert(keys==3&&inputs==4&&std::string(input.getAttribute("value"))=="seer");
 assert(producer.accept(key()));root.removeChild(other);producer.drain();assert(keys==4&&inputs==5&&std::string(input.getAttribute("value"))=="see");
 producer.shutdown();t.clear();puts("PASS actual SDL Producer detach/refocus cancels queued/local-batch/IME; retained descendant and unrelated removal preserve input; no SDL/OS input");}

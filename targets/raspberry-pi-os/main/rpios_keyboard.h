// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <SDL2/SDL.h>
#if !SDL_VERSION_ATLEAST(2,0,22)
#error "Raspberry Pi OS native keyboard requires SDL2 >= 2.0.22 for extended IME event ownership"
#endif
#include "events.h"
#include "ui/document.h"
#include "ui/node.h"
#include "ui/tree_internal.h"
#include "ui/tree_events.h"
#include "ui/selector.h"
#include <cstring>
#include <deque>
#include <memory>
#include <string>

namespace gea::rpios::keyboard {
using gea::embedded::ui::NodeHandle;
using gea::framework::events::PointerEvent;
using gea::framework::events::PointerEventType;
inline int keyCode(SDL_Keycode sym) {
 if(sym>=SDLK_a&&sym<=SDLK_z)return 'A'+sym-SDLK_a;
 if(sym>=SDLK_0&&sym<=SDLK_9)return '0'+sym-SDLK_0;
 switch(sym){case SDLK_BACKSPACE:return 8;case SDLK_TAB:return 9;case SDLK_RETURN:case SDLK_KP_ENTER:return 13;case SDLK_ESCAPE:return 27;case SDLK_SPACE:return 32;case SDLK_PAGEUP:return 33;case SDLK_PAGEDOWN:return 34;case SDLK_END:return 35;case SDLK_HOME:return 36;case SDLK_LEFT:return 37;case SDLK_UP:return 38;case SDLK_RIGHT:return 39;case SDLK_DOWN:return 40;case SDLK_DELETE:return 46;default:break;}
 if(sym>=SDLK_F1&&sym<=SDLK_F12)return 112+sym-SDLK_F1;
 return 0;
}
inline std::string code(SDL_Scancode scan){
 if(scan>=SDL_SCANCODE_A&&scan<=SDL_SCANCODE_Z)return "Key"+std::string(1,char('A'+scan-SDL_SCANCODE_A));
 if(scan>=SDL_SCANCODE_1&&scan<=SDL_SCANCODE_9)return "Digit"+std::string(1,char('1'+scan-SDL_SCANCODE_1));
 if(scan>=SDL_SCANCODE_F1&&scan<=SDL_SCANCODE_F12)return "F"+std::to_string(1+scan-SDL_SCANCODE_F1);
 switch(scan){case SDL_SCANCODE_LSHIFT:return "ShiftLeft";case SDL_SCANCODE_RSHIFT:return "ShiftRight";case SDL_SCANCODE_LCTRL:return "ControlLeft";case SDL_SCANCODE_RCTRL:return "ControlRight";case SDL_SCANCODE_LALT:return "AltLeft";case SDL_SCANCODE_RALT:return "AltRight";case SDL_SCANCODE_LGUI:return "MetaLeft";case SDL_SCANCODE_RGUI:return "MetaRight";case SDL_SCANCODE_MINUS:return "Minus";case SDL_SCANCODE_EQUALS:return "Equal";case SDL_SCANCODE_LEFTBRACKET:return "BracketLeft";case SDL_SCANCODE_RIGHTBRACKET:return "BracketRight";case SDL_SCANCODE_BACKSLASH:return "Backslash";case SDL_SCANCODE_SEMICOLON:return "Semicolon";case SDL_SCANCODE_APOSTROPHE:return "Quote";case SDL_SCANCODE_GRAVE:return "Backquote";case SDL_SCANCODE_COMMA:return "Comma";case SDL_SCANCODE_PERIOD:return "Period";case SDL_SCANCODE_SLASH:return "Slash";case SDL_SCANCODE_0:return "Digit0";case SDL_SCANCODE_SPACE:return "Space";case SDL_SCANCODE_RETURN:return "Enter";case SDL_SCANCODE_KP_ENTER:return "NumpadEnter";case SDL_SCANCODE_TAB:return "Tab";case SDL_SCANCODE_ESCAPE:return "Escape";case SDL_SCANCODE_BACKSPACE:return "Backspace";case SDL_SCANCODE_DELETE:return "Delete";case SDL_SCANCODE_LEFT:return "ArrowLeft";case SDL_SCANCODE_RIGHT:return "ArrowRight";case SDL_SCANCODE_UP:return "ArrowUp";case SDL_SCANCODE_DOWN:return "ArrowDown";case SDL_SCANCODE_HOME:return "Home";case SDL_SCANCODE_END:return "End";case SDL_SCANCODE_PAGEUP:return "PageUp";case SDL_SCANCODE_PAGEDOWN:return "PageDown";default:return "Unidentified";}
}
inline std::string key(SDL_Keycode sym,SDL_Keymod mod){
 if(sym>=SDLK_a&&sym<=SDLK_z){const bool upper=bool(mod&KMOD_SHIFT)!=bool(mod&KMOD_CAPS);return std::string(1,char((upper?'A':'a')+sym-SDLK_a));}
 switch(sym){case SDLK_SPACE:return " ";case SDLK_RETURN:case SDLK_KP_ENTER:return "Enter";case SDLK_ESCAPE:return "Escape";case SDLK_BACKSPACE:return "Backspace";case SDLK_TAB:return "Tab";case SDLK_DELETE:return "Delete";case SDLK_LEFT:return "ArrowLeft";case SDLK_RIGHT:return "ArrowRight";case SDLK_UP:return "ArrowUp";case SDLK_DOWN:return "ArrowDown";case SDLK_HOME:return "Home";case SDLK_END:return "End";case SDLK_PAGEUP:return "PageUp";case SDLK_PAGEDOWN:return "PageDown";case SDLK_LSHIFT:case SDLK_RSHIFT:return "Shift";case SDLK_LCTRL:case SDLK_RCTRL:return "Control";case SDLK_LALT:case SDLK_RALT:return "Alt";case SDLK_LGUI:case SDLK_RGUI:return "Meta";default:break;}
 if(sym>=SDLK_F1&&sym<=SDLK_F12)return "F"+std::to_string(1+sym-SDLK_F1);
 // SDL2 keysym does not encode layout-specific shifted punctuation. Do not
 // invent a US layout. Text comes exclusively from actual SDL_TEXTINPUT UTF-8.
 if(sym>=33&&sym<127&&!(mod&KMOD_SHIFT))return std::string(1,char(sym));
 return "Unidentified";
}
inline PointerEvent translate(const SDL_KeyboardEvent& native,bool composing){
 PointerEvent event{};event.type=native.type==SDL_KEYUP?PointerEventType::KeyUp:PointerEventType::KeyDown;
 const auto mod=static_cast<SDL_Keymod>(native.keysym.mod);event.key=key(native.keysym.sym,mod);event.code=code(native.keysym.scancode);event.keyCode=keyCode(native.keysym.sym);
 event.altKey=bool(mod&KMOD_ALT);event.ctrlKey=bool(mod&KMOD_CTRL);event.metaKey=bool(mod&KMOD_GUI);event.shiftKey=bool(mod&KMOD_SHIFT);event.repeat=native.repeat!=0;event.isComposing=composing;return event;
}
class Producer {
 struct Pending{NodeHandle owner;PointerEvent event;std::string text;bool textInput=false;unsigned long epoch=0;};
 struct FocusObserver{Producer*owner;};
 std::shared_ptr<FocusObserver> observer_;bool stopped_=false;
 std::deque<Pending> pending_;Uint32 window_=0;bool focused_=false,composing_=false,suppressText_=false;unsigned long epoch_=0;NodeHandle suppressOwner_;
 static NodeHandle target(){auto owner=gea::embedded::ui::Document::instance().activeElement();if(!owner.valid())owner=NodeHandle(gea::embedded::ui::Tree::instance().mountedRoot());return owner;}
 bool current(const NodeHandle& owner)const{return focused_&&owner.valid()&&owner.isConnected()&&!owner.disabled()&&!gea::embedded::ui::selector::disabled(gea::embedded::ui::Tree::instance(),owner.id())&&target().id()==owner.id();}
 static void dirty(int id){auto&t=gea::embedded::ui::Tree::instance();t.markNodeDisplayCommandsDirty(id);t.setDisplayListRebuildRequired(true);auto&n=t.node(id);n.render.dirty=1;n.render.layout_dirty=1;n.render.non_scroll_dirty=1;}
 static void input(NodeHandle owner,const std::string&value){auto&t=gea::embedded::ui::Tree::instance();if(t.activeInputId()!=owner.id())return;const char*previous=t.getAttribute(owner.id(),"value");if(value==(previous?previous:""))return;t.setAttribute(owner.id(),"value",value.c_str());t.markActiveInputEdited();dirty(owner.id());PointerEvent event{};event.type=PointerEventType::Input;event.targetId=owner.id();t.dispatchEvent(event);}
public:
 Producer()=default;Producer(const Producer&)=delete;Producer&operator=(const Producer&)=delete;
 ~Producer(){if(observer_)observer_->owner=nullptr;}
 void shutdown(){stopped_=true;reset();focused_=false;if(observer_){observer_->owner=nullptr;gea::embedded::ui::removeDocumentEventListener("focusin",observer_.get(),true);gea::embedded::ui::removeDocumentEventListener("focusout",observer_.get(),true);observer_.reset();}}
 void reset(){++epoch_;pending_.clear();composing_=false;suppressText_=false;suppressOwner_=NodeHandle();}
 void bind(Uint32 window,bool focused){
  if(stopped_)return;
  if(!observer_)observer_=std::make_shared<FocusObserver>(FocusObserver{this});
  // Refresh registrations at the actual frame boundary. Ordinary Tree.clear
  // resets the document realm; no cached registration may survive that reset.
  gea::embedded::ui::removeDocumentEventListener("focusin",observer_.get(),true);gea::embedded::ui::removeDocumentEventListener("focusout",observer_.get(),true);
  const std::weak_ptr<FocusObserver> weak=observer_;auto changed=[weak](auto&){if(auto live=weak.lock();live&&live->owner)live->owner->reset();};
  const bool in=gea::embedded::ui::addDocumentEventListener("focusin",observer_.get(),changed,true,false),out=gea::embedded::ui::addDocumentEventListener("focusout",observer_.get(),changed,true,false);
  if(!in||!out){reset();focused_=false;return;}
  const bool actualFocus=focused&&window!=0;if(window_!=window||focused_!=actualFocus){reset();window_=window;focused_=actualFocus;}
 }
 bool accept(const SDL_Event& native){
  if(stopped_||!window_)return false;
  if(native.type==SDL_WINDOWEVENT){if(native.window.windowID!=window_)return false;switch(native.window.event){case SDL_WINDOWEVENT_FOCUS_GAINED:reset();focused_=true;return true;case SDL_WINDOWEVENT_FOCUS_LOST:case SDL_WINDOWEVENT_HIDDEN:case SDL_WINDOWEVENT_MINIMIZED:reset();focused_=false;return true;default:return false;}}
  Uint32 id=0;switch(native.type){case SDL_KEYDOWN:case SDL_KEYUP:id=native.key.windowID;break;case SDL_TEXTEDITING:id=native.edit.windowID;break;case SDL_TEXTEDITING_EXT:id=native.editExt.windowID;break;case SDL_TEXTINPUT:id=native.text.windowID;break;default:return false;}
  if(id!=window_||!focused_)return false;
  if(native.type==SDL_TEXTEDITING){composing_=native.edit.text[0]!=0;return true;}
  if(native.type==SDL_TEXTEDITING_EXT){composing_=native.editExt.text&&native.editExt.text[0]!=0;return true;}
  auto owner=target();if(!current(owner))return false;
  if(pending_.size()>=256){reset();return false;}
  Pending entry;entry.owner=owner;entry.epoch=epoch_;
  if(native.type==SDL_TEXTINPUT){entry.textInput=true;entry.text.assign(native.text.text,strnlen(native.text.text,sizeof(native.text.text)));composing_=false;}
  else entry.event=translate(native.key,composing_);
  pending_.push_back(std::move(entry));return true;
 }
 void drain(void(*toggleFullscreen)()=nullptr){
  std::deque<Pending> batch;batch.swap(pending_);
  for(auto&entry:batch){if(entry.epoch!=epoch_||!current(entry.owner))continue;
   if(entry.textInput){if(suppressText_&&suppressOwner_.valid()&&suppressOwner_.id()==entry.owner.id())continue;auto&t=gea::embedded::ui::Tree::instance();if(t.activeInputId()!=entry.owner.id())continue;const char*old=t.getAttribute(entry.owner.id(),"value");input(entry.owner,(old?std::string(old):std::string())+entry.text);continue;}
   auto&event=entry.event;event.targetId=entry.owner.id();gea::embedded::ui::dispatchRootKeyEvent(event);
   if(entry.epoch!=epoch_)continue; // Focus A→B→A during callbacks cannot revive defaults.
   if(event.type!=PointerEventType::KeyDown)continue;
   suppressOwner_=entry.owner;suppressText_=bool(event.defaultPrevented)||!current(entry.owner);
   if(!current(entry.owner)||event.defaultPrevented||event.isComposing)continue;
   if(event.key=="Enter"&&!event.repeat&&!event.altKey&&!event.ctrlKey&&!event.metaKey){auto&t=gea::embedded::ui::Tree::instance();if(t.activeInputId()==entry.owner.id())t.commitActiveInput();}
   if(event.key=="F11"&&!event.repeat&&!event.altKey&&!event.ctrlKey&&!event.metaKey&&toggleFullscreen)toggleFullscreen();
   if(event.key=="Backspace"&&!event.altKey&&!event.ctrlKey&&!event.metaKey){auto&t=gea::embedded::ui::Tree::instance();if(t.activeInputId()!=entry.owner.id())continue;const char*old=t.getAttribute(entry.owner.id(),"value");std::string value=old?old:"";if(!value.empty()){size_t start=value.size()-1;while(start&&((unsigned char)value[start]&0xc0)==0x80)--start;value.erase(start);input(entry.owner,value);}}
  }
 }
};
} // namespace gea::rpios::keyboard

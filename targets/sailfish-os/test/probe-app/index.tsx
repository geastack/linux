import { Component, mount } from '@geastack/core'
import type { InputEvent } from '@geastack/core'
import store from './store'
import './probe.css'

store.init()

export default class SailfishProbeApp extends Component {
  template() {
    const rows = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
    return (
      <div class="probe-root">
        <div class="probe-title">Sailfish Probe - Harbour aarch64</div>
        <div class="probe-meta">
          Counter/Storage: {store.count} (saved: {store.stored})
        </div>
        <button class="probe-btn probe-btn-storage" onClick={() => store.increment()}>
          Increment & Save localStorage
        </button>
        <button class="probe-btn probe-btn-audio" onClick={() => store.playTone()}>
          Play Audio ({store.audioStatus})
        </button>
        <button class="probe-btn probe-btn-net" onClick={() => store.fetchExample()}>
          HTTPS Fetch ({store.netStatus})
        </button>
        <input
          class="probe-input"
          type="text"
          value={store.inputText}
          onInput={(e: InputEvent) => store.setInput(e.target.value)}
          placeholder="Text input (Maliit)"
        />
        <input
          class="probe-input"
          type="password"
          onInput={(e: InputEvent) => store.setSecret(e.target.value)}
          placeholder="Password (hiddenText)"
        />
        <div class="probe-sub">
          Input: {store.inputText} | Secret len: {store.secretLen}
        </div>
        {rows.map((row) => (
          <div key={row} class="probe-row">
            Scroll row #{row} - 1px border & touch hit target
          </div>
        ))}
      </div>
    )
  }
}

mount(SailfishProbeApp)

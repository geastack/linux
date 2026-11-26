import { Store } from '@geastack/core'

class ProbeStore extends Store {
  count = 0
  stored = 'none'
  netStatus = 'idle'
  inputText = ''
  secretLen = 0
  audioStatus = 'ready'

  init() {
    const saved = localStorage.getItem('probe_counter')
    if (saved) {
      this.stored = saved
      const parsed = parseInt(saved, 10)
      if (!Number.isNaN(parsed)) this.count = parsed
    }
  }

  increment() {
    this.count += 1
    const next = String(this.count)
    localStorage.setItem('probe_counter', next)
    this.stored = next
    console.log(`[probe] counter=${next} saved=${localStorage.getItem('probe_counter')}`)
  }

  playTone() {
    try {
      const ctx = new AudioContext()
      const osc = ctx.createOscillator()
      osc.type = 'sine'
      osc.frequency.setValueAtTime(440, ctx.currentTime)
      osc.frequency.setValueAtTime(660, ctx.currentTime + 0.12)
      osc.connect(ctx.destination)
      osc.start(ctx.currentTime)
      osc.stop(ctx.currentTime + 0.25)
      this.audioStatus = 'played 440->660Hz'
      console.log('[probe] audio played 440->660Hz')
    } catch {
      this.audioStatus = 'audio-error'
      console.log('[probe] audio error')
    }
  }

  fetchExample() {
    this.netStatus = 'fetching...'
    const res = fetch('https://example.com')
    this.netStatus = res.ok ? `ok (${res.status})` : `http ${res.status}`
    console.log(`[probe] fetch https://example.com -> ok=${res.ok} status=${res.status}`)
  }

  setInput(val: string) {
    this.inputText = val
  }

  setSecret(val: string) {
    this.secretLen = val.length
  }
}

export default new ProbeStore()

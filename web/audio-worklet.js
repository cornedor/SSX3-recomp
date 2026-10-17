// Plays the APU's output ring (apu_xaudio2.c, web_ring_t) straight out of the
// wasm memory: 48 kHz stereo s16, positions in frames, only ever growing.
//   +0 magic 'XA2R'  +4 frames  +8 wr  +12 rd  +16 underruns  +20 data
class RingPlayer extends AudioWorkletProcessor {
  constructor() {
    super();
    this.u32 = null;
    this.port.onmessage = (e) => {
      const { buffer, ring } = e.data;
      this.u32 = new Uint32Array(buffer, ring, 5);
      this.data = new Int16Array(buffer, ring + 20, this.u32[1] * 2);
      this.mask = this.u32[1] - 1;
      this.muted = false;
    };
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const l = out[0], r = out[1] || out[0];
    const n = l.length;
    if (!this.u32) return true;
    const wr = Atomics.load(this.u32, 2);
    let rd = Atomics.load(this.u32, 3);
    const avail = (wr - rd) >>> 0;
    const take = Math.min(avail, n);
    for (let i = 0; i < take; i++) {
      const at = ((rd + i) & this.mask) * 2;
      l[i] = this.data[at] / 32768;
      r[i] = this.data[at + 1] / 32768;
    }
    for (let i = take; i < n; i++) { l[i] = 0; r[i] = 0; }
    if (take < n && rd !== 0) Atomics.add(this.u32, 4, 1);
    Atomics.store(this.u32, 3, (rd + take) >>> 0);
    return true;
  }
}
registerProcessor('ring-player', RingPlayer);

// Audio thread of the browser client (see client.html).
//
// Downmixes the microphone to mono and posts it in batches tagged with the
// AudioContext frame number of their first sample. A batch is always
// gap-free: if the input skips frames, the current batch is sent early so the
// main thread sees the discontinuity.

const BATCH = 2048;

class CaptureProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.buf = new Float32Array(BATCH);
    this.len = 0;
    this.first = 0;
  }

  flush() {
    if (this.len === 0) return;
    const pcm = this.buf.slice(0, this.len);
    this.port.postMessage({ frame: this.first, pcm }, [pcm.buffer]);
    this.len = 0;
  }

  process(inputs) {
    const channels = inputs[0];
    if (!channels || channels.length === 0) return true; // no input connected yet
    const n = channels[0].length;
    if (this.len > 0 && currentFrame !== this.first + this.len) this.flush();
    if (this.len === 0) this.first = currentFrame;
    for (let i = 0; i < n; i++) {
      let s = 0;
      for (const ch of channels) s += ch[i];
      this.buf[this.len++] = s / channels.length;
      if (this.len === BATCH) {
        this.flush();
        this.first = currentFrame + i + 1;
      }
    }
    return true;
  }
}

registerProcessor("bsp-capture", CaptureProcessor);

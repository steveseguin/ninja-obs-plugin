// A sink-only worklet stays on the AudioContext render clock without playing the
// probe or routing silence through a gain node to the hardware destination.
function definePcmCaptureWorklet() {
  class PcmCapture extends AudioWorkletProcessor {
    constructor() {
      super();
      this.samples = new Float32Array(4096);
      this.used = 0;
      this.active = true;
      this.port.onmessage = ({data}) => {
        if (data === 'flush') {
          this.active = false;
          this.flush();
          this.port.postMessage({done:true});
        }
      };
    }
    flush() {
      if (!this.used) return;
      const samples = this.samples.slice(0,this.used);
      this.port.postMessage({samples},[samples.buffer]);
      this.used = 0;
    }
    process(inputs) {
      if (!this.active) return false;
      const input = inputs[0] && inputs[0][0];
      const count = input ? input.length : 128;
      for (let i=0;i<count;i++) {
        this.samples[this.used++] = input ? input[i] : 0;
        if (this.used === this.samples.length) this.flush();
      }
      return true;
    }
  }
  registerProcessor('vdoninja-pcm-capture',PcmCapture);
}
const pcmCaptureWorkletSource = `(${definePcmCaptureWorklet.toString()})()`;
module.exports = {pcmCaptureWorkletSource};

// Ponto de entrada do Web Worker: o núcleo roda aqui, longe da thread da página.
import { createCore } from './emu-core.js';

const handle = createCore((msg, transfer) => self.postMessage(msg, transfer || []));
self.onmessage = (e) => handle(e.data);

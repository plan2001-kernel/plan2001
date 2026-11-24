// The page side of drawterm's gui-web boundary (monolith.h).  drawterm calls
// these through gui-web/library.js as Module.browserDrawterm.*, on the main
// thread; drawterm's exports arrive in attach().  The screen is a Canvas 2D
// surface, and every drawterm connection is one binary WebSocket to the
// same-origin gateway, which maps only allowlisted service ports to the
// configured CPU/auth server.
'use strict';

function BrowserDrawterm(opts) {
	const ctx = opts.canvas.getContext('2d', { alpha: false });
	const cursorcanvas = document.createElement('canvas');
	cursorcanvas.width = cursorcanvas.height = 16;
	const socks = Object.create(null);
	const log = opts.log || (() => {});
	let wasm = null, flushes = 0, inputs = 0, lastInput = null;
	const Maxsend = 32 * 1024 * 1024;

	return {
		attach(exports) {
			wasm = exports;
			if (opts.onready)
				opts.onready();
		},
		get ready() { return wasm !== null; },
		get flushes() { return flushes; },
		get inputs() { return inputs; },
		get lastInput() { return lastInput; },
		get connections() { return Object.keys(socks).length; },

		// from the page: mouse (1: x, y, buttons), key (2: rune, down),
		// resize (3: w, h)
		input(type, a, b, c) {
			if (!wasm)
				return;
			inputs++;
			lastInput = [type, a, b, c];
			wasm.input(type, a, b, c);
		},

		// the screen
		size: () => opts.size(),
		resize(w, h) {
			const c = opts.canvas;
			c.width = w;
			c.height = h;
			c.style.width = (w / opts.dpr()) + 'px';
			c.style.height = (h / opts.dpr()) + 'px';
		},
		present(img, x, y) {
			ctx.putImageData(img, x, y);
			flushes++;
			if (opts.onflush)
				opts.onflush(flushes);
		},
		cursor(img, hx, hy) {
			cursorcanvas.getContext('2d').putImageData(img, 0, 0);
			hx = Math.min(15, Math.max(0, hx));
			hy = Math.min(15, Math.max(0, hy));
			opts.canvas.style.cursor = `url(${cursorcanvas.toDataURL()}) ${hx} ${hy}, default`;
		},

		// drawterm's connections: one WebSocket each; gen tells an old
		// socket's late events from the new one's in the same slot
		netopen(conn, gen, url) {
			const ws = new WebSocket(url);
			ws.binaryType = 'arraybuffer';
			socks[conn] = ws;
			ws.onopen = () => wasm && wasm.netstate(conn, gen, 3);
			ws.onclose = e => {
				if (socks[conn] === ws)
					delete socks[conn];
				log(`connection ${conn} closed (${e.code})`);
				if (wasm)
					wasm.netstate(conn, gen, 4);
			};
			ws.onerror = () => log(`connection ${conn} failed`);
			ws.onmessage = e => {
				if (wasm && typeof e.data !== 'string')
					wasm.netdata(conn, gen, new Uint8Array(e.data));
			};
		},
		netsend(conn, bytes) {
			const ws = socks[conn];
			if (!ws || ws.readyState !== 1)
				return;
			if (ws.bufferedAmount + bytes.length > Maxsend) {
				log(`connection ${conn}: send buffer over ${Maxsend} bytes`);
				ws.close(1009, 'send buffer');
				return;
			}
			ws.send(bytes);
		},
		netclose(conn) {
			const ws = socks[conn];
			if (!ws)
				return;
			delete socks[conn];
			ws.close();
		},
	};
}

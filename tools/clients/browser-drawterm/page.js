// The page side of drawterm's gui-web boundary (monolith.h).  drawterm calls
// these through gui-web/library.js as Module.browserDrawterm.*, on the main
// thread; drawterm's exports arrive in attach().  The screen is a Canvas 2D
// surface, and every drawterm connection is one binary WebSocket to the
// same-origin gateway, which maps only allowlisted service ports to the
// configured CPU/auth server.  The page never chooses the WebSocket URL.
'use strict';

function BrowserDrawterm(opts) {
	const ctx = opts.canvas.getContext('2d', { alpha: false });
	const cursorcanvas = document.createElement('canvas');
	cursorcanvas.width = cursorcanvas.height = 16;
	const socks = Object.create(null);
	const log = opts.log || (() => {});
	let wasm = null, flushes = 0, resizes = 0, inputs = 0, lastInput = null;
	const Maxsend = 8 * 1024 * 1024;
	const Opentimeout = 10000;

	function fail(conn, gen, why) {
		log(`connection ${conn}: ${why}`);
		if (wasm)
			wasm.netstate(conn, gen, 4);
	}

	return {
		attach(exports) {
			wasm = exports;
			if (opts.onready)
				opts.onready();
		},
		get ready() { return wasm !== null; },
		get flushes() { return flushes; },
		get resizes() { return resizes; },
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
			resizes++;
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
			let u;
			try {
				u = new URL(url, location.href);
			} catch (e) {
				fail(conn, gen, 'bad WebSocket URL');
				return;
			}
			const wantproto = location.protocol === 'https:' ? 'wss:' : 'ws:';
			if (u.protocol !== wantproto || u.host !== location.host ||
			    !/^\/ws\/tcp\/[0-9]+$/.test(u.pathname) ||
			    u.search || u.hash) {
				fail(conn, gen, 'WebSocket URL is not same-origin /ws/tcp/PORT');
				return;
			}
			if (socks[conn])
				socks[conn].ws.close();
			const ws = new WebSocket(u.href);
			const slot = { ws, gen, timer: 0 };
			socks[conn] = slot;
			ws.binaryType = 'arraybuffer';
			slot.timer = setTimeout(() => {
				if (socks[conn] === slot && ws.readyState !== WebSocket.OPEN) {
					log(`connection ${conn}: open timeout`);
					ws.close();
				}
			}, Opentimeout);
			ws.onopen = () => {
				clearTimeout(slot.timer);
				slot.timer = 0;
				if (socks[conn] === slot && wasm)
					wasm.netstate(conn, gen, 3);
			};
			ws.onclose = e => {
				clearTimeout(slot.timer);
				slot.timer = 0;
				if (socks[conn] === slot)
					delete socks[conn];
				log(`connection ${conn} closed (${e.code})`);
				if (wasm)
					wasm.netstate(conn, gen, 4);
			};
			ws.onerror = () => log(`connection ${conn} failed`);
			ws.onmessage = e => {
				if (socks[conn] !== slot)
					return;
				if (typeof e.data === 'string') {
					log(`connection ${conn}: text frame from gateway`);
					ws.close(1003, 'binary only');
					return;
				}
				if (wasm)
					wasm.netdata(conn, gen, new Uint8Array(e.data));
			};
		},
		netsend(conn, gen, bytes) {
			const slot = socks[conn];
			if (!slot || slot.gen !== gen || slot.ws.readyState !== WebSocket.OPEN) {
				fail(conn, gen, 'send on closed connection');
				return;
			}
			const ws = slot.ws;
			if (bytes.length > Maxsend || ws.bufferedAmount + bytes.length > Maxsend) {
				log(`connection ${conn}: send buffer over ${Maxsend} bytes`);
				ws.close(1009, 'send buffer');
				return;
			}
			try {
				ws.send(bytes);
			} catch (e) {
				log(`connection ${conn}: send failed`);
				ws.close();
			}
		},
		netclose(conn, gen) {
			const slot = socks[conn];
			if (!slot || slot.gen !== gen)
				return;
			delete socks[conn];
			clearTimeout(slot.timer);
			slot.ws.close();
		},
	};
}

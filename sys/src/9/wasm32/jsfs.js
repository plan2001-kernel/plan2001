// jsfs.js: sivun tiedosto-operaatiot WASM32-koneen natiivissa
// nimiavaruudessa (aux/jsio, #j/js, devjs.c).  Ei omaa tiedostojärjestelmää
// eikä 9P:tä: jokainen kutsu on yksi jsio:n pyyntö, jonka natiivi ohjelma
// tekee järjestelmäkutsuina; etätiedostot kulkevat ytimen 9P:n ja verkon
// kautta.  Kaikki kutsut ovat asynkronisia (Promise), ja data liikkuu
// enintään CHUNK tavun paloina.
//
//   import { boot } from './platform.js';
//   import { JsFs } from './jsfs.js';
//   const fs = new JsFs();
//   await boot(kernel, { ...front, js: fs.receive });	// fs.attach(boot():n jssend)
//
// Rajapinta (polut koneen nimiavaruudessa):
//   stat(path), fstat(fid)          -> Dir { name, qid: { path, vers, type }, mode, length, mtime, atime, uid, gid, muid }
//   open(path, mode), create(path, mode, perm) -> { fid, iounit }
//   read(fid, offset, count) -> Uint8Array;  write(fid, offset, bytes) -> n
//   dirread(fid) -> [Dir] ([] luettu loppuun);  close(fid)
//   readdir(path) -> [Dir];  readFile(path, { max }) -> Uint8Array;  readText(path)
//   writeFile(path, bytes, { perm, excl, publish: { name, gid, mode } })
//   remove(path), wstat(path, { name, mode, gid, length, mtime }), fwstat(fid, ...)
//   bind(new, old, flags), mount(addr, old, flags, spec), unmount(new, old)
// Virhe hylkää lupauksen: Error(jsio:n syy).

export const OREAD = 0, OWRITE = 1, ORDWR = 2, OTRUNC = 16, OEXCL = 0x1000;
export const MREPL = 0, MBEFORE = 1, MAFTER = 2, MCREATE = 4;
export const DMDIR = 0x80000000;
export const CHUNK = 60 * 1024;	/* jsio:n Maxio on 64 KiB */

const enc = new TextEncoder(), dec = new TextDecoder();

// rc:n lainaus kuten tokenize sen purkaa
export function quote(s) {
	s = String(s);
	return s !== '' && !/[\s'"\\]/.test(s) && !/[\0\n]/.test(s) ? s : "'" + s.replace(/'/g, "''") + "'";
}

// tokenize: sanat, '...'-lainaus ja '' lainauksen sisällä
export function words(line) {
	const out = [];
	let i = 0;
	while (i < line.length) {
		while (i < line.length && /[ \t\r]/.test(line[i])) i++;
		if (i >= line.length) break;
		let w = '';
		while (i < line.length && !/[ \t\r]/.test(line[i])) {
			if (line[i] === "'") {
				i++;
				while (i < line.length) {
					if (line[i] === "'") {
						if (line[i + 1] === "'") { w += "'"; i += 2; continue; }
						i++;
						break;
					}
					w += line[i++];
				}
			} else w += line[i++];
		}
		out.push(w);
	}
	return out;
}

function parseDir(line) {
	const f = words(line);
	if (f.length < 11) return null;
	return {
		name: f[0], qid: { path: f[1], vers: Number(f[2]), type: Number(f[3]) },
		mode: Number(f[4]) >>> 0, length: Number(f[5]), mtime: Number(f[6]), atime: Number(f[7]),
		uid: f[8], gid: f[9], muid: f[10],
	};
}

const dirs = (text) => text.split('\n').filter((l) => l).map(parseDir).filter(Boolean);

export class JsFs {
	constructor() {
		this.tag = 0;
		this.pending = new Map();
		this.send = null;
		this.queue = [];
		this.receive = this.receive.bind(this);
	}

	// boot():n palauttama jssend (platform.js)
	attach(send) {
		this.send = send;
		for (const m of this.queue.splice(0)) send(m);
	}

	// kernelin viesti (front.js): vastaus, tai null: jsio sulki kanavan
	receive(b) {
		if (b === null) {
			for (const p of this.pending.values()) p.reject(new Error('jsio: the machine closed the channel'));
			this.pending.clear();
			return;
		}
		const nl = b.indexOf(10);
		const head = words(dec.decode(b.subarray(0, nl < 0 ? b.length : nl)));
		const data = nl < 0 ? new Uint8Array(0) : b.subarray(nl + 1);
		const p = this.pending.get(head[0]);
		if (!p) return;
		this.pending.delete(head[0]);
		if (head[1] === 'ok') p.resolve({ args: head.slice(2), data });
		else p.reject(new Error(head.slice(2).join(' ') || 'jsio: error'));
	}

	call(op, args = [], data = null) {
		const tag = String(++this.tag);
		const line = enc.encode([tag, op, ...args.map(quote)].join(' ') + '\n');
		const msg = new Uint8Array(line.length + (data ? data.length : 0));
		msg.set(line);
		if (data) msg.set(data, line.length);
		return new Promise((resolve, reject) => {
			this.pending.set(tag, { resolve, reject });
			if (this.send) this.send(msg); else this.queue.push(msg);
		});
	}

	ping() { return this.call('ping'); }
	async stat(path) { return parseDir(dec.decode((await this.call('stat', [path])).data)); }
	async fstat(fid) { return parseDir(dec.decode((await this.call('fstat', [fid])).data)); }
	async open(path, mode = OREAD) { const r = await this.call('open', [path, mode]); return { fid: r.args[0], iounit: Number(r.args[1]) }; }
	async create(path, mode, perm) { const r = await this.call('create', [path, mode, String(perm >>> 0)]); return { fid: r.args[0] }; }
	async read(fid, offset, count) { return (await this.call('read', [fid, offset, Math.min(count, CHUNK)])).data; }
	// Cancel a blocking wait using the watched service's own control operation
	// before closing its fid; the worker owns a duplicate until read returns.
	async wait(fid, offset, count) { return (await this.call('wait', [fid, offset, Math.min(count, CHUNK)])).data; }
	async write(fid, offset, bytes) { return Number((await this.call('write', [fid, offset], bytes)).args[0]); }
	async dirread(fid) { return dirs(dec.decode((await this.call('dirread', [fid])).data)); }
	async close(fid) { await this.call('close', [fid]); }
	async remove(path) { await this.call('remove', [path]); }
	async bind(nw, old, flags = MREPL) { await this.call('bind', [nw, old, flags]); }
	async mount(addr, old, flags = MREPL, spec) { await this.call('mount', spec === undefined ? [addr, old, flags] : [addr, old, flags, spec]); }
	async unmount(nw, old) { await this.call('unmount', [nw ?? '-', old]); }
	static fields(d) { return Object.entries(d).filter(([, v]) => v !== undefined).map(([k, v]) => k + '=' + v); }
	async wstat(path, d) { await this.call('wstat', [path, ...JsFs.fields(d)]); }
	async fwstat(fid, d) { await this.call('fwstat', [fid, ...JsFs.fields(d)]); }

	// hakemisto kokonaan (open, dirread kunnes tyhjä, close)
	async readdir(path) {
		const { fid } = await this.open(path, OREAD);
		try {
			const all = [];
			for (;;) {
				const d = await this.dirread(fid);
				if (!d.length) return all;
				all.push(...d);
			}
		} finally {
			this.close(fid).catch(() => {});
		}
	}

	// tiedosto paloina; ensimmäinen pala samalla pyynnöllä kuin avaus (get)
	async readFile(path, { max = 16 * 1024 * 1024 } = {}) {
		const first = await this.call('get', [path, 0, CHUNK]);
		const length = Number(first.args[1]);
		if (length > max) throw new Error(path + ': too large');
		if (first.data.length >= length) return first.data.slice();
		const out = new Uint8Array(length);
		out.set(first.data);
		const { fid } = await this.open(path, OREAD);
		try {
			let off = first.data.length;
			while (off < length) {
				const b = await this.read(fid, off, CHUNK);
				if (!b.length) break;
				out.set(b, off);
				off += b.length;
			}
			return out.subarray(0, off);
		} finally {
			this.close(fid).catch(() => {});
		}
	}

	async readText(path, opt) { return dec.decode(await this.readFile(path, opt)); }

	/*
	 * Kirjoitus paloina.  publish: tiedosto kirjoitetaan ensin nimellä path
	 * (esim. vaiheistusnimi samassa hakemistossa) ja julkaistaan yhdellä
	 * fwstatilla: uusi nimi, ryhmä ja oikeudet kerralla - lukija näkee
	 * joko ei mitään tai koko tiedoston.  Epäonnistuessa keskeneräinen
	 * poistetaan, mutta vain kun se on varmasti tämän kutsun luoma (excl)
	 * tai vaiheistusnimi (publish): muuten create katkaisi olemassa
	 * olevan tiedoston, ja poisto hävittäisi sen kokonaan.  Plan 9:n
	 * fwstat ei korvaa olemassa olevaa nimeä, joten publish julkaisee vain
	 * uuden nimen.
	 */
	async writeFile(path, bytes, { perm = 0o644, excl = false, publish = null } = {}) {
		if (typeof bytes === 'string') bytes = enc.encode(bytes);
		const { fid } = await this.create(path, OWRITE | (excl ? OEXCL : 0), perm);
		try {
			for (let off = 0; off < bytes.length; off += CHUNK) {
				const n = await this.write(fid, off, bytes.subarray(off, Math.min(bytes.length, off + CHUNK)));
				if (n !== Math.min(CHUNK, bytes.length - off)) throw new Error(path + ': short write');
			}
			if (publish) await this.fwstat(fid, publish);
		} catch (e) {
			await this.close(fid).catch(() => {});
			if (excl || publish) await this.remove(path).catch(() => {});
			throw e;
		}
		await this.close(fid);
	}
}

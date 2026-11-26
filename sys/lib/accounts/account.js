// The accounts' page's client (account.html): auth/accountd's requests
// (sys/src/cmd/accounts/accountd.c) over this origin's /account
// WebSocket, never another's (a page that took the address from its URL
// could be made to relay someone's passkey to a stranger's server).
// A ceremony asks its challenge, makes the passkey, and sends its request
// on a new connection: a phone may end the first while another app makes
// the passkey, and accountd finds the challenge by the client data.
'use strict';

const $ = (id) => document.getElementById(id);
const rpid = document.querySelector('meta[name="p2-rpid"]')?.content || location.hostname;
const cookie = 'p2session';

function b64u(buf) {
	let s = '';
	for (const b of new Uint8Array(buf)) s += String.fromCharCode(b);
	return btoa(s).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
}
function unb64u(s) {
	s = s.replace(/-/g, '+').replace(/_/g, '/');
	const t = atob(s + '='.repeat((4 - s.length % 4) % 4));
	return Uint8Array.from(t, (c) => c.charCodeAt(0));
}
// a passkey's name: at most 64 bytes of UTF-8 (accountd's limit), whole characters
function label(s) {
	let t = s.trim();
	while (new TextEncoder().encode(t).length > 64) t = Array.from(t).slice(0, -1).join('');
	return t ? b64u(new TextEncoder().encode(t)) : '-';
}
const unlabel = (s) => (s === '-' ? '' : new TextDecoder().decode(unb64u(s)));

function status(text, bad) {
	$('status').textContent = text;
	$('status').className = bad ? 'bad' : '';
}

// one connection's requests: ask(line) resolves to accountd's answer
function connect() {
	return new Promise((resolve, reject) => {
		const ws = new WebSocket(`${location.protocol === 'https:' ? 'wss:' : 'ws:'}//${location.host}/account`);
		const waiting = [];
		ws.onmessage = (e) => waiting.shift()?.resolve(String(e.data));
		ws.onclose = () => { for (const w of waiting.splice(0)) w.reject(new Error('the server ended the connection')); };
		ws.onerror = () => reject(new Error('no connection to the server'));
		ws.onopen = () => resolve({
			ask: (line) => new Promise((res, rej) => { waiting.push({ resolve: res, reject: rej }); ws.send(line); }),
			close: () => ws.close(),
		});
	});
}
async function once(line) {
	const c = await connect();
	try { return await c.ask(line); } finally { c.close(); }
}
function answer(a) {
	if (a.startsWith('error ')) throw new Error(a.slice(6));
	return a;
}

// the session: its token in a cookie this page reads (SameSite=Strict; Secure on https)
function token() {
	return document.cookie.split('; ').find((c) => c.startsWith(`${cookie}=`))?.slice(cookie.length + 1) || '';
}
function keep(tok, expires) {
	const secure = location.protocol === 'https:' ? '; Secure' : '';
	const age = tok ? Math.max(0, expires - Math.floor(Date.now() / 1000)) : 0;
	document.cookie = `${cookie}=${tok}; Max-Age=${age}; Path=/; SameSite=Strict${secure}`;
}

// the hints: none (the device offers where), a phone's (hybrid: the browser's QR code), a security key's
function attachment(hint) {
	return hint ? 'cross-platform' : undefined;
}
const hints = (hint) => (hint ? [hint] : []);

async function create(c, name, handle, hint, exclude) {
	const cred = await navigator.credentials.create({ publicKey: {
		challenge: unb64u(c),
		rp: { id: rpid, name: 'Plan2001' },
		user: { id: unb64u(handle), name, displayName: name },
		pubKeyCredParams: [{ type: 'public-key', alg: -7 }],
		authenticatorSelection: { authenticatorAttachment: attachment(hint), residentKey: 'required', requireResidentKey: true, userVerification: 'required' },
		attestation: 'none',
		hints: hints(hint),
		excludeCredentials: exclude.map((id) => ({ type: 'public-key', id: unb64u(id) })),
		extensions: { prf: {} },
		timeout: 110000,
	} });
	const prf = cred.getClientExtensionResults().prf?.enabled;
	return { line: `${b64u(cred.rawId)} ${b64u(cred.response.getAuthenticatorData())} ${b64u(cred.response.clientDataJSON)}`, prf };
}

async function enroll(hint) {
	const invite = new URLSearchParams(location.hash.slice(1)).get('invite');
	const c = await connect();
	try {
		const [, chal, name, handle] = answer(await c.ask(`challenge enroll ${invite}`)).split(' ');
		const cr = await create(chal, name, handle, hint, []);
		const [, tok, who, expires] = answer(await once(`enroll ${invite} ${cr.line} ${label($('enrolllabel').value)}`)).split(' ');
		keep(tok, Number(expires));
		history.replaceState(null, '', location.pathname);	// the invitation is used up
		await show(who);
		status(`The account ${who} is made${cr.prf ? ' (the passkey has PRF)' : ''}.`);
	} finally { c.close(); }
}

async function login(hint) {
	const c = await connect();
	try {
		const [, chal] = answer(await c.ask('challenge login')).split(' ');
		const cred = await navigator.credentials.get({ publicKey: {
			challenge: unb64u(chal), rpId: rpid, userVerification: 'required', hints: hints(hint), timeout: 110000,
		} });
		const r = cred.response;
		const handle = r.userHandle && r.userHandle.byteLength ? b64u(r.userHandle) : '-';
		const [, tok, who, expires] = answer(await once(`login ${b64u(cred.rawId)} ${b64u(r.authenticatorData)} ${b64u(r.clientDataJSON)} ${b64u(r.signature)} ${handle}`)).split(' ');
		keep(tok, Number(expires));
		await show(who);
		status(`Signed in as ${who}.`);
	} finally { c.close(); }
}

async function add(hint) {
	const c = await connect();
	try {
		const [, chal, name, handle, ...ids] = answer(await c.ask(`challenge add ${token()}`)).split(' ');
		const cr = await create(chal, name, handle, hint, ids);
		answer(await once(`add ${token()} ${cr.line} ${label($('addlabel').value)}`));
		$('addlabel').value = '';
		await show(name);
		status('The passkey is added.');
	} finally { c.close(); }
}

async function revoke(h) {
	answer(await once(`revoke ${token()} ${h}`));
	await show($('accountname').textContent);
	status('The passkey is removed.');
}

async function show(name) {
	for (const s of ['enroll', 'signin']) $(s).hidden = true;
	$('account').hidden = false;
	$('accountname').textContent = name;
	const lines = answer(await once(`creds ${token()}`)).split('\n').slice(1);
	const ul = $('creds');
	ul.replaceChildren();
	for (const l of lines) {
		const [h, , lab, created, last] = l.split(' ');
		const li = document.createElement('li');
		li.dataset.cred = h;
		const when = (t) => (Number(t) ? new Date(Number(t) * 1000).toLocaleString() : 'never');
		const span = document.createElement('span');
		span.innerHTML = '<span></span><br><small></small>';
		span.firstChild.textContent = unlabel(lab) || 'passkey';
		span.lastChild.textContent = `made ${when(created)}, last used ${when(last)}`;
		const b = document.createElement('button');
		b.textContent = 'Remove';
		b.onclick = () => run(() => revoke(h));
		li.append(span, b);
		ul.append(li);
	}
}

async function run(fn) {
	status('…');
	try { await fn(); } catch (e) { status(e.message || String(e), true); }
}

async function start() {
	if (!window.PublicKeyCredential) { status('This browser has no passkeys (WebAuthn).', true); return; }
	for (const b of document.querySelectorAll('button[data-op]')) {
		const op = { enroll, login, add }[b.dataset.op];
		b.onclick = () => run(() => op(b.dataset.hint));
	}
	$('logout').onclick = () => run(async () => {
		await once(`logout ${token()}`).catch(() => {});
		keep('', 0);
		$('account').hidden = true;
		$('signin').hidden = false;
		status('Signed out.');
	});
	const invite = new URLSearchParams(location.hash.slice(1)).get('invite');
	if (invite) {
		$('enroll').hidden = false;
		await run(async () => {
			// the name only: the challenge is asked again when a passkey is made
			const [, , name] = answer(await once(`challenge enroll ${invite}`)).split(' ');
			$('enrollname').textContent = name;
			status('');
		});
		return;
	}
	if (token()) {
		try {
			const [, who] = answer(await once(`session ${token()}`)).split(' ');
			await show(who);
			return;
		} catch { keep('', 0); }
	}
	$('signin').hidden = false;
}

start();

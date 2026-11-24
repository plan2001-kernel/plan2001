/*
 * The standalone PWA's service worker: its fetch handler makes the page
 * installable, and it keeps the page's own firmware for an offline start.
 *
 * Asennettu järjestelmä käynnistyy OPFS-levyltään (sdW0) levyn omalla
 * firmwarella, ytimellä ja juurella; verkosta tarvitaan vain sivu ja
 * platform.js (myös levy-Workerille).  Ne ja manifestin kuvakkeet
 * tallennetaan asennuksessa yhdellä kertaa tämän version välimuistiin
 * (addAll: kaikki tai ei mitään).  Epäonnistunut tai keskeneräinen haku
 * hylkää asennuksen, jolloin edellinen versio välimuisteineen jää voimaan.
 *
 * Sivu ja platform.js haetaan ensin verkosta, joten päivitykset toimivat
 * kuten ennenkin; välimuisti vastaa vain, kun verkko ei vastaa.  Verkon
 * vastauksia ei kirjoiteta välimuistiin, jotta sivu ja platform.js ovat
 * aina samaa versiota.  Muita pyyntöjä (plan9.ini, 9wasm32.wasm, .fs,
 * ISO:n Range-pyynnöt, /ether) ei käsitellä: ne menevät verkkoon kuten
 * ennenkin, eikä verkon live-levyä väitetä välimuistiin tallennetuksi.
 * OPFS:ään (käyttäjän levy) tämä ei koske.
 */
const VERSION = '@VERSION@';	/* tools/build, tools/dist: sama kuin sivun */
const CACHE = 'plan9-wasm32-shell ' + VERSION;
const SHELL = ['./', 'index.html', 'platform.js', 'manifest.webmanifest', 'icon.svg', 'icon-192.png', 'icon-512.png'];
const scope = new URL(self.registration.scope);
const shellPaths = new Set(SHELL.map((p) => new URL(p, scope).pathname));

self.addEventListener('install', (e) => {
	e.waitUntil((async () => {
		const c = await caches.open(CACHE);
		/* cache: 'reload': palvelimelta, ei selaimen HTTP-välimuistista */
		await c.addAll(SHELL.map((p) => new Request(new URL(p, scope), { cache: 'reload' })));
		await self.skipWaiting();
	})().catch(async (err) => {
		await caches.delete(CACHE);	/* keskeneräinen pois; edellinen versio jää */
		throw err;
	}));
});

self.addEventListener('activate', (e) => {
	e.waitUntil((async () => {
		/* vanhat versiot pois vasta, kun tämän versio on kokonainen */
		for (const k of await caches.keys())
			if (k.startsWith('plan9-wasm32-shell ') && k !== CACHE)
				await caches.delete(k);
		await self.clients.claim();
	})());
});

self.addEventListener('fetch', (e) => {
	const r = e.request;
	if (r.method !== 'GET')
		return;
	const u = new URL(r.url);
	if (u.origin !== scope.origin || !shellPaths.has(u.pathname))
		return;
	e.respondWith((async () => {
		try {
			return await fetch(r);
		} catch (err) {
			/* verkko ei vastaa: tämän version firmware; ?boot= ja ?iso= pysyvät URL:ssa */
			const c = await caches.open(CACHE);
			const key = r.mode === 'navigate' ? new URL('index.html', scope).href : r;
			const hit = await c.match(key, { ignoreSearch: true });
			if (hit)
				return hit;
			throw err;
		}
	})());
});

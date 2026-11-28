# The machine's network

A machine in a browser has an Ethernet card (`#l0`, `etherwasm32.c`)
whose wire is a WebSocket to its page's origin, `/ether`
(`docs/kernel.md`).  A frame is one binary WebSocket message each way.
The link connects at boot: there is no passkey, no ticket and no network
prompt.

## Local

`tools/serve` relays the page's `/ether` (the next port on loopback) to
`tools/gateway`, the other end of the wire.  `tools/gateway` is a switch
and DHCP server:

    tools/gateway            # the Internet through passt, no root
    tools/gateway -n         # a network of its own, 10.0.2.0/24, no way out (tests)
    tools/gateway -t tap0    # a TAP made by root, bridged or NATed to the LAN

Its DHCP hands out 10.0.2.15 and up by MAC, router 10.0.2.2, and DNS
(`GATEWAY_DNS`, default 1.1.1.1 and 8.8.8.8).

## Origin relay

The firmware connects to its own origin's `/ether` (`wss://` on HTTPS,
or the local relay on loopback). A public reverse proxy must forward
WebSocket upgrades and the browser's `Origin` to `tools/gateway`.
Start the gateway with `-o https://HOSTNAME` to restrict that origin.
The gateway's port, TAP setup and host forwarding must match the actual
host configuration; they are not configured by the browser runtime.

## Trust

The machine's page is its firmware; an installed machine's firmware comes
from its own disk (`index.html` and `platform.js` on its 9fat), which its
owner can change.  Any of it runs with the page's origin's rights.  So
the origin holds nothing that is not the machine's own: its disk (OPFS)
and its BIOS settings.  It no longer holds a network ticket: there is
none.

## Tests

`tools/test/accounts` tests `aux/ethernetd`'s session sign-in through
`aux/etherws` (a valid, forged and malformed session, the machine origin,
concurrent checks); `tools/test/run net` tests the switch, DHCP, ARP, ping
and NAT.  A public gateway can use the same `tools/gateway` with a TAP and host NAT.

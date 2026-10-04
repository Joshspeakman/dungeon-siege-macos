# Multiplayer: findings and plan

Status: not implemented yet. Goal: Mac and Windows players in the same games.

## How the game does multiplayer

| Piece | Finding |
|---|---|
| Unlocking it | The GOG executable's main-menu button only shows "Disabled". Started with the argument `zonematch=true` (the documented way on Windows), the game opens its multiplayer screens directly; the app passes it with `DS_ARGS="zonematch=true"`. No patch is needed. |
| Modes | **Local** (LAN, hosts found by broadcast) and **Internet** (join by the host's IP address). The original ZoneMatch lobby service no longer exists. |
| Transport | Microsoft **DirectPlay 8**, client/server only (`CLSID/IID_DirectPlay8Client`, `…Server`, `…Address`, the TCP/IP service provider), created with `CoCreateInstance`. Today the app answers "class not registered" and the game stops with "Could not initialize DirectPlay 8". |
| Ports | UDP 6073 for the first contact, UDP 2302–2400 for game traffic. |
| On Windows | Players enable *DirectPlay* (Windows Features → Legacy Components), launch with `zonematch=true`, and connect over LAN, by IP with port forwarding on the host, or through a virtual network such as Tailscale or ZeroTier. |
| ZoneMatch files | `GunDll.dll` (ZoneTech "GUN 2.0" lobby client) and `LtDll.dll` (its sign-in) serve the dead lobby service and are not needed. |

## Plan

1. **DirectPlay 8 in the runtime** (`runtime/win32/dpnet.c`): `IDirectPlay8Server`, `IDirectPlay8Client`,
   `IDirectPlay8Address`, the TCP/IP provider on UDP sockets, following Microsoft's published protocol specifications
   ([MC-DPL8CS] core and service providers, [MC-DPL8R] reliable delivery) so that Mac players and Windows players running
   Microsoft's DirectPlay can host and join each other: host enumeration (LAN broadcast and direct address), connect,
   reliable and unreliable sends, player create/destroy, session termination, and the game's message handler called
   from service threads as DirectPlay does.
2. **WSOCK32**: the six ordinals the game imports (`WSAStartup`, `gethostname`, `gethostbyname`, …) on BSD sockets.
3. **Making connections easy from a Mac**: a Multiplayer button in the launch window (starts the game in its
   multiplayer screens); automatic port mapping (UPnP / NAT-PMP) when a Mac hosts; the address to give friends, with a
   copy button; a guide covering LAN, direct IP and Tailscale.
4. **Tests**: two copies on one Mac over the loopback interface (scripted), then a Mac with a Windows PC on a LAN, then
   over the internet; join-in-progress, trading and PvP options.
5. Later, if needed: peer-to-peer connections through home routers without port forwarding or a VPN (NAT hole punching
   with a small rendezvous service). Windows players use Microsoft's DirectPlay, which cannot do this, so it would need a
   helper on their side.

## Risks

- DirectPlay's reliable protocol (sequencing, acknowledgements, retransmission, coalescing) is the largest part.
- All players need the same game version (1.11.1); whether the Steam release is the same build is unverified.
- The multiplayer paths of the GOG executable have been exercised far less than single player.

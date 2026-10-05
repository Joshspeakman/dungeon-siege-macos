# Multiplayer

The app plays the game's own multiplayer: LAN games, games over the internet by address, and the multiplayer maps
(Utraean Peninsula, the multiplayer Kingdom of Ehb, and Yesterhaven if you add it). It speaks the same network protocol
as Microsoft DirectPlay 8 on Windows, implemented from Microsoft's published specifications, so Mac and Windows players
can be in the same game.

## Starting

In the launch window set **Game** to **Multiplayer** and press **Play**. The game opens on its multiplayer menu
(**Internet** or **Network**; ZoneMatch was an online service that no longer exists). The note under the setting
shows this Mac's address for other players: its LAN address and, if you use Tailscale or a similar virtual network,
that address too (shown as *VPN*).

The first time you host or search for games, macOS asks whether Dungeon Siege may find devices on your local network,
and the firewall (if it is on) whether it may accept incoming connections. Allow both.

## On the same network (LAN)

- **Host:** Network → enter your name → **Host Game**.
- **Join:** Network → the game appears in the list → select it → **Join Game**.

## Over the internet

The host and everyone joining use the **Internet** screen: the game only lets players who joined from the Internet
screen into a game hosted from it (and LAN players into LAN games).

- **Host:** Internet → **Host Game**. Give the other players the first address under *Host IP Address* (your
  router's public address).
- **Join:** Internet → type the host's address under *Enter host's IP address* → **Connect**.

The host's router must let the game's traffic in. When a Mac hosts, this normally happens by itself: the app asks the
router to forward the game's ports (NAT-PMP or UPnP, which most home routers have switched on), shows the router's
public address first in the address lines, and removes the forwarding when the game ends. Players joining need
nothing. (Tested: a PC on the open internet joined a Mac behind a home router this way.) If the router doesn't do
either, or for a Windows host without DirectPlay's own UPnP support:

- **Forward UDP ports 6073 and 2302–2400** to the host's computer on its router, and give players the router's public
  address (shown by any "what is my IP" site); or
- **Use a virtual network** such as [Tailscale](https://tailscale.com) or [ZeroTier](https://www.zerotier.com): every
  player joins it, and the host gives out its address on that network. Nothing needs to be forwarded, and the game
  behaves as if everyone were on one LAN (both the Network and Internet screens work).

## Windows and Linux players

Mac and PC players can be in the same game, in either direction (tested against the original game on Linux through
Proton, with Microsoft's DirectPlay). The game itself checks that everyone runs exactly the same thing, so:

- **The same game build.** Every player needs the GOG release, version 1.11.1. The Steam release is a slightly
  different build: Steam and GOG copies refuse each other even when both run on Windows.
- **The executable named `DungeonSiege.exe`.** Its file name is part of the fingerprint the game compares; a renamed
  copy (e.g. `DungeonSiege_GOG.exe`) is refused.
- **The same resource files.** Every player needs the same set of `.dsres` archives in `Resources` (and maps in
  `Maps`): if one player has Yesterhaven or a mod such as `fairyfix.dsres`, everyone needs it. A player who is
  missing one is told which.
- **DirectPlay.** On Windows: Control Panel → Programs → *Turn Windows features on or off* → **Legacy Components** →
  **DirectPlay**. On Linux with Proton or Wine, Wine's own DirectPlay is not enough: install Microsoft's with
  `protontricks <the game's Steam app id> directplay` (or `winetricks directplay` for a plain Wine prefix).
- **The multiplayer screens.** Start the game with the `zonematch=true` argument: on Windows, make a shortcut to
  `DungeonSiege.exe` and add ` zonematch=true` at the end of its *Target*; in Steam, add it to the launch options.

Then host or join exactly as above.

## Yesterhaven

Yesterhaven is Gas Powered Games' free multiplayer adventure for Dungeon Siege (two files, `Yesterhaven.dsmap` and
`Yesterhaven.dsres`). If you have them (for example in a Windows installation of the game, or from the original
download), add them with:

```sh
./install.sh --yesterhaven "/path/to/folder/with/the/files"
```

They are copied into the app's data folder (`~/Games/DungeonSiegeNative/game`), which the game sees as part of its own
`Maps` and `Resources` folders; your game folder is not changed. The host picks **Yesterhaven** under **Map Settings**
in the staging area. Every player needs the files, Windows players in their game's `Maps` and `Resources` folders.

## Security

Dungeon Siege's network protocol (DirectPlay 8, 2002) has no encryption or authentication. Anyone who can reach a host
can send it packets, and someone on the path between two players can read or alter their traffic. The app's
implementation is hardened against malformed and hostile packets:

- Players can only join through the game's own accept step, which includes the session password.
- Connection floods and oversized messages are capped.
- Crafted replies to a game search are rejected.

`recomp/tests/dp8fuzz.c` throws random and hostile traffic at it.

The protocol's own limits can't be closed without breaking play with Windows and Linux players: packets can be
forged by someone who can see the traffic, and a session's identity is chosen by whoever connects.

- For games over the internet, a virtual network such as Tailscale or ZeroTier keeps the game's ports closed to
  everyone else.
- To host without asking the router to open ports, start the app with `DS_NO_PORTMAP=1`.
- Port forwarding is removed when the game ends normally. If the app is force-quit, some routers keep it until
  they restart.

## How it works

`recomp/runtime/win32/dpnet.c` provides the DirectPlay 8 objects the game uses (`IDirectPlay8Client`, `…Server`,
`…Address`) and the client/server session protocol ([MC-DPL8CS]); `dp8proto.c` the reliable transport over UDP
([MC-DPL8R]: sequencing, acknowledgements, retransmission, keep-alives, coalesced frames) and host enumeration
([MC-DPLHP]). `recomp/tests/dp8test.c` checks the transport against the example packets in the specifications and runs
a session over loopback with simulated packet loss. `recomp/tests/dp8fuzz.c` is a sanitizer fuzz test of the same transport (see
*Security*).

## Status

- Hosting, finding and joining games (LAN and by address), the staging area, and playing together on Utraean
  Peninsula and Yesterhaven work between copies of the app.
- Playing with Windows players follows Microsoft's protocol specifications but has not been tested against Windows yet.
- Known quirk: when a player joins, the host's chat shows "… has entered the game" followed by "… has left the game",
  although the player stays in the game.

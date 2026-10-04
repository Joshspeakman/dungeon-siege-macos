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

- **Host:** Internet → **Host Game**. Give the other players your address.
- **Join:** Internet → type the host's address under *Enter host's IP address* → **Connect**.

The host's router must let the game's traffic in. Either:

- **Forward UDP ports 6073 and 2302–2400** to the host's computer on its router, and give players the router's public
  address (shown by any "what is my IP" site); or
- **Use a virtual network** such as [Tailscale](https://tailscale.com) or [ZeroTier](https://www.zerotier.com): every
  player joins it, and the host gives out its address on that network. Nothing needs to be forwarded, and the game
  behaves as if everyone were on one LAN (both the Network and Internet screens work).

## Windows players

All players need the same game version, 1.11.1 (the GOG release). On Windows:

1. Turn on DirectPlay: Control Panel → Programs → *Turn Windows features on or off* → **Legacy Components** →
   **DirectPlay**.
2. Start the game with the `zonematch=true` argument to open its multiplayer screens: make a shortcut to
   `DungeonSiege.exe` and add ` zonematch=true` at the end of its *Target*.

Then host or join exactly as above; Mac and Windows players do not need anything else to play together.

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

## How it works

`recomp/runtime/win32/dpnet.c` provides the DirectPlay 8 objects the game uses (`IDirectPlay8Client`, `…Server`,
`…Address`) and the client/server session protocol ([MC-DPL8CS]); `dp8proto.c` the reliable transport over UDP
([MC-DPL8R]: sequencing, acknowledgements, retransmission, keep-alives, coalesced frames) and host enumeration
([MC-DPLHP]). `recomp/tests/dp8test.c` checks the transport against the example packets in the specifications and runs
a session over loopback with simulated packet loss.

## Status

- Hosting, finding and joining games (LAN and by address), the staging area, and playing together on Utraean
  Peninsula and Yesterhaven work between copies of the app.
- Playing with Windows players follows Microsoft's protocol specifications but has not been tested against Windows yet.
- Known quirk: when a player joins, the host's chat shows "… has entered the game" followed by "… has left the game",
  although the player stays in the game.

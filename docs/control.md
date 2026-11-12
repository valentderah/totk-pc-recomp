# Control socket

A local text endpoint that a companion app on the same PC, such as a progress map or a script, uses to read live game
state: where Link is and which way he faces. It is off by default and only reads: no command writes game memory or
files, moves anything or ends the game. Code: `runtime/control.c`. The protocol is the one Savage Wind's control
endpoint uses, so a tool written for one works with the other.

## Turning it on

Any of these (the environment variable wins over the other two):

- the F1 menu, Menu tab, "Connection for map apps": On listens on port 7777, or on the port `control` in `totk.ini`
  names. The menu saves it to `totk.ini` and it applies at once; switching it off closes the port and every connection.
  The note under the value says `listening`, or `port in use` when another program has the port.
- `totk.ini`, section `[companion apps]`: `control = 7777`
- the environment: `TOTK_CONTROL=7777`, then start `totk.exe`. In PowerShell:

  ```powershell
  $env:TOTK_CONTROL = '7777'; .\totk.exe
  ```

The log says `control: listening on 127.0.0.1:7777` when it is up, or why not (the port is taken, for example), and
`control: stopped listening` when it is switched off. When `TOTK_CONTROL` is set, the menu row is greyed out like every
setting the environment fixes.

## Protocol

- TCP on `127.0.0.1` only: other machines cannot reach it.
- One request per line (`\n`, a `\r` before it is ignored), one JSON reply per line, in order:
  `{"ok":true,"data":{...}}` or `{"ok":false,"error":"..."}`.
- A connection whose first line looks like an HTTP request is closed before anything runs. A web page in a browser on
  the same PC can send requests to `127.0.0.1`, and this keeps it from reading the game.
- At most 4 connections, 8 waiting requests each; a line longer than 512 bytes closes the connection.
- Commands run between frames, on the thread that presents them, so a reply comes within about one frame.

```
$ python -c "import socket; s=socket.create_connection(('127.0.0.1',7777)); s.sendall(b'pos\n'); print(s.makefile().readline())"
{"ok":true,"data":{"x":843.40,"y":119.10,"z":849.60,"yaw":-136.3,"heading":316.3,"place":"totk:1 x=843.40 y=119.10 z=849.60 yaw=-136.3"}}
```

## Commands

| command | what it answers |
|---|---|
| `help` | `commands`: each command's `name`, `args` and `help` |
| `pos` | Link's world position `x`, `y`, `z` (y up, the same coordinates as `PlayerStatus.SavePos` in a save), `yaw` (the way he faces in degrees: 0 along +z, 90 along +x, as Savage Wind's `pos`), `heading` (the same direction as a map bearing, clockwise from north, -z), and `place`, one line with all of it. `No save loaded` on the title screen and while a save loads. No layer field: the sky, surface and Depths follow from y |
| `state` | `game` (1.4.3), `recomp` (this build's version), `frame` (presents since the start), `fps` (over the last second), `uptime_s`, `world_s` (game time since the save loaded, left out before), `in_world` |

The position and facing come from the resident Player actor, found as `runtime/frame_rate.c` finds it for the frame
step's speed lines; the facing was measured on 1.4.3 (`runtime/control.c` has the details).

## Checking it

`totk.exe --probe-control` runs the socket without the game (no game files needed): it listens on `127.0.0.1:47651`
(or the `TOTK_CONTROL` port), sends requests as a client and checks each answer, the HTTP guard, the line limit, and
switching it off and on again as the menu does.

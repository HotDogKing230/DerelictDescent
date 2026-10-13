# Derelict Descent

A turn-based survival roguelike for PocketMage, wrapped around a small grid
physics simulation (heat conduction, gas diffusion, phase change, electrical
heating).

## The game

Your ship's reactor is dead and the air is going with it. You are the salvage
probe sent to bring it back online.

- **Goal** - cross the derelict and reach the reactor room (`R`), then step onto
  the reactor. The readout shows how many rooms away it is (`REACTOR n`).
- **O2** - your suit tank starts at 100% and drains one point per turn (four per
  turn on a hazardous tile). At zero the suit fails and the run ends.
- **Canisters** - step onto an `o` to recover 50% O2. Spend them well.
- **Hazards** drain the tank fast: hot rooms (`!`), cold rooms (`x`), breached
  wiring (`e`), and oxygen-poor rooms. The status band warns when you are
  standing in one.

The world only advances when you act, so the e-ink panel refreshes only when
something on it actually changes.

## Controls

| Key              | Action                                   |
| ---------------- | ---------------------------------------- |
| `W` `A` `S` `D`  | Move (one turn)                          |
| `SPACE`          | Wait (one turn, lets the physics settle) |
| `H` / `?`        | Show the briefing again                  |
| `N`              | New run                                  |
| `Q`              | Save world + current room                |
| `ESC` / exit key | Quit                                     |

The field kit edits the tile you are standing on and is part of the physics
sandbox: `L` water, `G` oxygen, `T` heat, `E` toggle electricity.

## Map legend

```text
#  wall        ~  water      *  ice
o  O2 canister R  reactor    .  floor
!  hot         x  cold / low O2      e  live wiring
```

The filled block is you.

## Build

```sh
pm build     # build/Oni.app.elf
pm check     # gate undefined symbols against the host export surface
pm pack      # build/Oni.tar for the APPLOADER
```

## Layout

```text
main.cpp              app entry (extern "C" int main) and the whole game
Makefile              SDK build recipe
assets/icon.png       source artwork for the APPLOADER card
tools/make_icon.py    icon converter
```

## License

Apache-2.0 (see LICENSE).

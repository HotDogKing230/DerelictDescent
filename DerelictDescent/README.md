# Derelict Descent

A turn-based, grid-based spaceship survival and construction game for PocketMage.
The compact C++ simulation keeps the original 8x8 rooms, save files, e-ink-aware
rendering, heat/gas/electrical simulation, and adds mining, building, renewable
water, hydroponics, food, power, and replayable seeded expeditions.

## Survival and colony loop

- Mine nearby stone walls with `X` to recover stone; ore can be mined for scrap.
- Select a structure with `1`-`6`, then press `B` to build on the tile you're
  standing on. Build only on empty floor.
- Solar arrays generate power each turn. Water recyclers convert power into
  renewable water. Gardens consume water and power to grow food and add oxygen
  while reducing local CO2. Fabricators convert stone and power into scrap.
- Food is consumed over time. Press `F` to eat one food and restore 10 suit O2.
- Water is a renewable closed-loop resource in this first implementation; the
  recycler produces water when it has power.
- New runs use a new seed. Saved rooms from older runs are regenerated on demand
  when their seed doesn't match, so each expedition can be replayed from scratch.

## Controls

| Key | Action |
| --- | --- |
| `W A S D` | Move one tile / one turn |
| `SPACE` | Wait one turn; machines and physics advance |
| `X` | Mine the first adjacent mineable tile (north, east, south, west) |
| `1` | Select wall |
| `2` | Select hydroponic garden |
| `3` | Select water recycler |
| `4` | Select solar array |
| `5` | Select battery |
| `6` | Select fabricator |
| `B` | Build selected structure on current tile |
| `F` | Eat food to restore suit oxygen |
| `N` | Start a fresh seeded expedition |
| `Q` | Save world and current room |
| `H` / `?` | Show briefing |
| `ESC` / exit key | Quit |

The original field-kit sandbox keys remain: `L` adds water, `G` adds oxygen,
`T` adds heat, and `E` toggles electricity.

## Structures

| Symbol | Structure | Effect |
| --- | --- | --- |
| `#` | Wall | Blocks movement and separates atmosphere |
| `g` | Garden | Uses water and power; produces food and oxygen |
| `r` | Water recycler | Consumes power and generates water |
| `s` | Solar array | Generates power |
| `b` | Battery | Structure placeholder for future local energy storage |
| `f` | Fabricator | Uses power and stone to create scrap |
| `o` | O2 canister | Restores suit oxygen |
| `R` | Reactor | Original objective tile |

Resources shown along the bottom are `S` stone, `M` scrap, `W` water, `F` food,
and `P` power. The simulation is deliberately integer-based and bounded for the
PocketMage hardware.

## Build

```sh
pm build
pm check
pm pack
```

Set `PM_SDK_ROOT` to the PocketMage SDK directory, or pass `SDK_ROOT` to make.

## License

Apache-2.0 (see LICENSE).

# Tool Target Marker

A mod for **Harvest Moon 64: Recompiled**, built on the
[N64Recomp](https://github.com/N64Recomp/N64Recomp) modding framework and the
[HM64RecompModTemplate](https://github.com/HarvestMoon64Recomp/HM64RecompModTemplate).

### What it does

Shows a translucent green marker on the ground over the farm tile your
currently equipped tool or seed packet (sickle, hoe, axe, hammer, watering
can, or any of the plantable seeds - turnip through blue mist, plus grass
seeds) is aimed at. The decal sits flat on the tile and follows your
facing, including diagonals and camera rotation.

The marker is hidden whenever:
* you're not on the Farm map (the only map this mod currently supports -
  see Ground decal below for why);
* you're holding an item (`gPlayer.heldItem != 0`);
* your equipped tool isn't one of the above - chicken feed and the miracle
  potion were checked and ruled out deliberately: they don't target a tile
  at all (`useChickenFeed` checks a coop interaction zone,
  `useMiraclePotion` targets a sick animal directly), not just "a tile we
  don't support yet." The fishing pole is the same story;
* a tool is actually being swung (`gPlayer.actionHandler != 0`);
* the aimed spot is off the field grid.

### How it's implemented

All in `src/tool_target.c`, as a single `RECOMP_HOOK_RETURN("updatePlayerAction")`,
plus a `RECOMP_HOOK_RETURN("loadMapAtSpawnPoint")` hook to (re)load the decal's
texture whenever Farm is entered. Every update, it checks the conditions
above. If they pass, it calls `getOffsetTileCoordinates(range, 8)` for the
aimed tile - `range` is `1.0f` for the field tools (the same call the axe
and hammer use, and that the base-level hoe uses too - see
`useAx`/`useHammer`/`useHoe` in `hm64-decomp/src/game/items.c`) but `0.0f`
for seeds, which plant one tile closer to the player
(`useTurnipSeeds`/`useGrassSeeds`/etc. all call
`getOffsetTileCoordinates(0.0f, temp)`, where `temp` is direction-dependent
in general but resolves to the same "straight ahead" code `8` for a single
planting action). Then `getGroundObjectWorldPosition` for that tile's
world-space center (including terrain height), and moves the decal there.
If either call returns the `y == 65535.0f` off-grid sentinel, or any
condition above fails, the decal is released instead (see Ground decal).

### Ground decal (Farm only)

The marker is a flat, ground-aligned translucent overlay - closer to what
the brief's "Flat highlight on the ground" alternative described - sharing
the tile-targeting logic above (`computeTargetWorldPos`).

**It turned out more tractable than the brief estimated, with a catch.**
The brief described this as needing hand-authored N64 display-list code
(decal render mode, `gEXMatrixGroup` tags to avoid jitter) - "several days
of graphics work." In fact the game already has a reusable flat-sprite
pipeline for exactly this: `SPRITE_BILLBOARD_XZ` (ground-aligned, as opposed
to the camera-facing `SPRITE_BILLBOARD_XY` entities use - see
`setupBitmapVertices` in `hm64-decomp/src/system/graphic.c`) combined with
`setMapObject`. It's already used for several translucent ground overlays:
mountain water puddles, the pond, beach waves, mountain path tiles (all in
`hm64-decomp/src/game/level.c`, `loadLevelMapObjects`). Reusing that
existing, already-recomp-compatible pipeline avoids both the display-list
authoring and the jitter concern.

**The catch: there are only 6 of these slots in the entire game**
(`MAP_OBJECT_1`..`MAP_OBJECT_6`), shared globally across every map and
already budgeted per-map for existing decorations - the same class of
constraint the entity marker hit with `showAnimalExpressionBubble`, except
tighter. An audit of `loadLevelMapObjects` before writing any code found:
* **Farm**: slots 1-2 used (a home decoration, the farm pond) - slots 3-6
  free.
* **Mountain 1**: all of slots 1-3 used (two water puddles, a bridge/pillar)
  - **zero free**.
* Mountain 2, Top of Mountain 1, Pond: at least one slot free, not fully
  audited.
* Greenhouse, Cave, Mine, Mine 2, Ranch, Moon Mountain: not audited.

Given Mountain 1 alone already has no free slot, matching the entity
marker's full field-map coverage isn't possible without stealing an
always-visible decoration somewhere - so rather than build that out fully,
this implementation is hard-restricted to `gBaseMapIndex == FARM` as a
proof of concept, using slot 4 (`MAP_OBJECT_4`, array index 3 into
`mainMap[...].mapObjects[]`), confirmed free there by reading Farm's own
case in `loadLevelMapObjects`, not assumed.

**A second, buffer-level version of the same problem.** The per-slot
texture buffers aren't the only shared resource - `hm64-decomp/src/buffers/mapObjectBuffer.h`
explicitly groups the palette/animation-frame/spritesheet-index buffers into
three pools ("Shared Metadata Groups (used by multiple map objects)"),
labeled A/B/C, each reused by *different* map objects on *different* maps.
Farm's existing slot 1 and slot 2 decorations use groups A and B
respectively - both persistently active. Using either for this decal would
corrupt their animation state while both are loaded, the same cross-talk
problem entity 48 had with `showAnimalExpressionBubble`'s shared metadata
buffer. Group C is unused anywhere in Farm's own `loadLevelMapObjects`
case, so this decal uses that - paired with the `MAP_OBJECT_BRIDGE_SLOT_4_TEXTURE_1/2`
destination buffers, a combination the game already uses together
elsewhere (Mountain 2 / Top of Mountain 1's bridges), rather than guessing
at buffer sizes.

Because group C is also used by other maps' bridges, the decal's texture
is reloaded every time Farm is (re)entered (`RECOMP_HOOK_RETURN("loadMapAtSpawnPoint")`,
gated to `gBaseMapIndex == FARM`), not just once ever - otherwise a visit to
one of those other maps in between would silently corrupt it with their
texture data.

**Texture:** reuses the farm pond's own ROM asset (`_farmPondTexture...`)
into a *different* destination buffer/metadata group than the real farm
pond uses. This was a deliberate fallback, not the first choice - the
generic character shadow blob texture (`_shadowsTextureSegmentRomStart`,
used by every character's shadow) looked like a better visual fit, but
it's loaded via `initializeShadowSprite`, which takes a different, shorter
list of ROM segment pointers than `dmaSprite` expects (no spritesheet-index
segment) - a format mismatch that was not confirmed safe to feed into this
pipeline, so a texture already proven to load correctly via `dmaSprite` into
a `MAP_OBJECT` slot was used instead. The visible result is a water texture
sitting on the target tile, not a highlight glow - an accepted placeholder
for a proof of concept, not a final look.

**Repositioning:** `setMapObject` increments `mainMap[...].mapState.mapObjectCount`
on every call; grepping the whole decomp shows nothing ever reads that
field (write-only dead data), so calling it every frame to reposition the
decal is safe despite looking like a one-time registration function
everywhere it's used in vanilla code. `deactivateMapObject` is called once
on the visible -> hidden transition, so the decal only claims the slot
while it's actually shown.

**Known limitations, beyond the water-texture placeholder:**
* Farm only - the map-object-slot audit above hasn't been done for every
  other field map, and Mountain 1 alone is confirmed to have zero free
  slots.

### Out of scope for v1

* Multi-tile targeting for upgraded sickle sweeps and the watering can's
  wider patterns - the single-tile marker is exact for the sickle, hoe, axe,
  hammer and watering can at their base tier, but upgraded tools hit more
  than one tile per swing (see `arr2`, `arr5`, `arr6` and friends in
  `hm64-decomp/src/game/items.c`).
* Precisely marking all 9 tiles a seed packet plants (see "Seeds plant a
  3x3 block" below) - the decal approximates it by scaling up a single
  decal instead of showing 9 individual tiles.
* Custom marker art (currently reusing an existing sprite/animation frame).
* Full field-map coverage (see Known limitations above).

### Config

No config options.

### Building

1. Install `clang`/`llvm` (MIPS-capable; LLVM 18.1.8 is recommended, LLVM
   19.1.0 is known to break MIPS codegen for this workflow) and `make`.
2. `make` (optionally `make -jN`).
3. Run `RecompModTool mod.toml build` to produce the `.nrm` file in `build/`.

On macOS:

```sh
CC=/opt/homebrew/opt/llvm/bin/clang LD=/opt/homebrew/bin/ld.lld make
```

(The `ld.lld` binary ships in Homebrew's separate `lld` package, not inside
`llvm`'s own `bin/`.)

### Testing

What was actually verified:
* `make` compiles clean (one pre-existing header-guard warning from
  `hm64-decomp/src/game/items.h` itself, unrelated to this mod) and links.
* `RecompModTool mod.toml build` produces a valid `.nrm`; both hook names
  resolve against `HarvestMoon64RecompSyms`.
* Every fact this mod's logic depends on was checked directly against the
  current decomp source rather than assumed: the single-tile call used by
  the axe/hammer/base hoe (`useAx`/`useHammer`/`useHoe` in
  `hm64-decomp/src/game/items.c`), the `heldItem`/`currentTool`/
  `actionHandler` gating (`updatePlayerAction` and the button-press handler
  in `hm64-decomp/src/game/player.c`), and the map-object-slot usage
  analysis above.

Still to check by hand:
1. Facings: walk the farm with a hoe and face all 8 directions, especially
   diagonals, and confirm the marker still sits on the tile that actually
   gets tilled (spot-checked but not exhaustively across all 8 facings yet).
2. Hidden cases: holding an item, mid-swing, off the field grid, off the
   Farm map.
3. Map changes: leave the farm and come back repeatedly - no crash, no
   stale decal, marker still works.
4. Mountain 2 / Top of Mountain 1: visit either (both reuse metadata group
   C for their bridges), then return to Farm and confirm the decal's
   texture didn't get left corrupted by their bridge texture.

**Ground decal - test history:**

* **Round 5**: first in-game test. The decal rendered (confirming the
  `SPRITE_BILLBOARD_XZ` + `setMapObject` approach genuinely works, and the
  symbol resolution / metadata-group-C / texture-reuse choices above were
  all correct enough to load and display *something*), but offset toward
  one corner of the tile instead of centered, and seemed to disappear after
  walking around for a while.
* **Round 6**: fixed the position offset. Root cause: found by reading
  `setupMapObjectSprites` in `hm64-decomp/src/system/map.c`, which computes
  `finalX = (coordinates.x + mapWorldCenterX) - tileSizeX/2` before applying
  the camera/view offset - i.e. `setMapObject`'s x/z are *not* the same
  coordinate space `getGroundObjectWorldPosition` returns (the space
  `setEntityCoordinates` uses directly, confirmed correct by the
  floating-icon style). The code now adds `tileSizeX/2` and `tileSizeZ/2`
  before calling `setMapObject`, canceling exactly the offset that formula
  subtracts. **Confirmed in-game**: centering is correct now, and - without
  any related change - the "disappears after walking around" symptom from
  round 5 stopped happening too (walking around a lot no longer hides it),
  so that was likely never a separate bug, just the same off-grid hiding
  the icon style has always had, surfaced by a round-5 test session that
  happened to wander further.
* **Round 7 (this version)**: fixed a real, reproducible disappearing bug -
  using the hoe to till the target tile, or picking up and dropping a weed
  on it, made the decal vanish permanently (not just hidden - gone for the
  rest of that Farm visit). Root cause: `hideGroundDecal` calls
  `deactivateMapObject`, which calls `deactivateSprite` on the underlying
  sprite - clearing its `SPRITE_ACTIVE` flag
  (`hm64-decomp/src/system/globalSprites.c`). Reshowing only called
  `setMapObject` again, which never reactivates the sprite itself - only
  `dmaSprite` (via `setSpriteType1`/`setSpriteType2`) sets `SPRITE_ACTIVE`
  back on. Any hide at all - a tool swing, holding an item, even briefly
  stepping off-grid - left it dead for good afterward. Fixed by calling
  `loadGroundDecalTexture()` (which wraps `dmaSprite`) every frame the decal
  is shown, not just once: `dmaSprite` already guards on
  `!(stateFlags & SPRITE_ACTIVE)` and no-ops instantly if the sprite is
  already active, so this only actually re-DMAs on the frame right after a
  hide - cheap the rest of the time. The map-load hook also now calls
  `deactivateSprite` before reloading, to force a real reload (instead of a
  silent no-op) if Mountain 2 or Top of Mountain 1 - both metadata group C -
  left the sprite active with their own bridge texture in between.
  **Confirmed in-game**: the decal now survives tilling the target tile
  with the hoe and picking up/dropping a weed on it.
* **Round 8**: swapped the source texture and retuned scale/tint using a
  standalone visual tool (see below), rather than guessing values and
  reinstalling repeatedly. Source changed from `_farmPondTexture*` to
  `_pondWaterTexture*` (the Pond map's own water asset, also confirmed
  `dmaSprite`-compatible); `GROUND_DECAL_SCALE` changed from `1.0f` to
  `2.9f`; `setSpriteBaseRGBA` changed from a neutral half-transparent white
  (255,255,255,128) to a green tint (50,236,81,133) - doing the work of
  reading as "marked tile" instead of "puddle," since the underlying asset
  is still a water texture either way.
* **Round 9**: `GROUND_DECAL_SCALE 2.9f` from round 8 was comically large
  in-game - reverted to `1.0f`. The tuning tool's scale slider isn't
  calibrated against the real engine's units (its preview maps "scale" to
  on-screen size using a guessed ratio, not a measured one), so it's
  useful for comparing textures and tints side by side, but its specific
  scale *numbers* aren't trustworthy without an in-game check - which this
  round is the reason why. Texture and tint from round 8 are unaffected
  and still in place.
* **Round 10**: reverted the source texture from `_pondWaterTexture*` back
  to `_farmPondTexture*` (the original) per direct request, with no reason
  given beyond preferring the old one - the Pond map's water and the farm's
  own pond water may simply look different enough in motion/at the game's
  actual resolution that the tuning tool's static crop comparison didn't
  fully capture. Scale (`1.0f`, round 9) and the green tint (round 8) were
  left as they were - only the texture source changed.
* **Round 11**: `GROUND_DECAL_SCALE` bumped from `1.0f` to `1.3f` - `1.0f`
  read a bit small in-game. Tuned directly against the real game this time,
  not the tuning tool (per round 9's finding that the tool's scale slider
  isn't calibrated to real engine units).
* **Round 12**: replaced the hardcoded `GROUND_DECAL_SCALE` constant and
  added two new position-nudge controls with live, in-game config sliders
  (`decal_scale`, `decal_offset_x`, `decal_offset_y` - "Ground Decal Size /
  X Offset / Y Offset" in the mod's config menu), since reinstalling the
  mod for every half-step of scale/position tuning (rounds 6-11) was slow.
  Size is capped at `3.0` (round 8's `2.9f` was already "comically large,"
  so there's little reason to allow much past that). X/Y offset range is
  `-16.0` to `16.0` - half a tile each way, assuming a 32-unit tile (not
  measured directly for Farm; inferred from the `/32` tile-to-world
  conversions used throughout `hm64-decomp/src/game/items.c` - if that's
  off, the practical effect is just that the slider's extremes don't
  exactly reach one full tile, not a hard limit anywhere else). "X/Y" here
  are the two horizontal/ground-plane axes (what the engine itself calls X
  and Z - `GROUND_DECAL_HEIGHT_OFFSET`, the vertical axis, is still a fixed
  `#define`, not exposed as a slider, since only size and horizontal
  position were asked for). All three are read fresh every frame the decal
  is shown (`loadGroundDecalTexture`/`updateGroundDecal`, already running
  every frame per round 7's fix), so dragging a slider in the config menu
  updates the decal immediately - no map reload or mod restart needed.
* **Round 13**: final values found by hand with the live sliders - size
  `2.0`, X offset `2.5`, Y offset `0.0` - baked in as `mod.toml`'s shipped
  defaults (`decal_scale`/`decal_offset_x`/`decal_offset_y`), so a fresh
  install starts tuned instead of at the round-12 placeholder defaults.
  Anyone who already has the mod installed keeps whatever they'd set in
  the config menu regardless - `mod.toml`'s `default` only applies to a
  config key with no saved value yet.
* **Round 14**: added seed support (`isSeedTool`), after confirming in
  `hm64-decomp/src/game/items.c` that chicken feed and the miracle potion
  genuinely don't target a tile at all (so leaving them out is correct, not
  an oversight), while seeds do - just via `getOffsetTileCoordinates(0.0f, ...)`
  instead of the field tools' `(1.0f, ...)`. **Not yet confirmed in-game**
  for any seed type or map, including whether the one-tile-closer target
  actually lines up with where the seed lands versus where the floating
  icon or ground decal renders.
* **Round 15**: confirmed the round 14 target tile was correct, but seeds
  actually plant a 3x3 block, not just that one tile - see "Seeds plant a
  3x3 block" below for why. Scaled the ground decal up 3x
  (`GROUND_DECAL_SEED_SCALE_MULTIPLIER`) specifically when a seed is
  equipped, to approximate the full affected area instead of marking only
  its center. **Not yet confirmed in-game.**
* **Round 16 (release cleanup)**: removed the floating-icon style and the
  `marker_style`/`decal_scale`/`decal_offset_x`/`decal_offset_y` config
  options entirely - the ground decal was the only style anyone actually
  wanted, and the round 13 tuned values (`2.0` size, `2.5`/`0.0` X/Y offset)
  are now baked in as fixed `#define`s instead of config defaults. This mod
  only supports the Farm map now; `computeTargetWorldPos`'s map check was
  simplified from the old multi-map `isFieldMap` list straight to
  `gBaseMapIndex == FARM` to match, since the ground decal was already
  hard-restricted to Farm and the other maps in that list were dead weight.

### Seeds plant a 3x3 block, not one tile

`getOffsetTileCoordinates(0.0f, 8)` (what seeds use) resolves to the
player's own tile exactly - range `0.0f` cancels the "step forward in the
facing direction" term in `getOffsetTileCoordinates` entirely (see
`hm64-decomp/src/game/player.c`), and `directionalOffset == 8` skips the
function's second offset term too. That's the single tile the marker shows,
and round 14 confirmed it's the right one.

But `useTurnipSeeds`/`usePotatoSeeds`/etc. don't call `getOffsetTileCoordinates`
once - `toolUse.stepIndex` increments every call and the action continues
until it hits `9`, cycling `directionalOffset` through `toolSweepOffsetsLeft`
or `toolSweepOffsetsRight` (`hm64-decomp/src/game/items.c`): `{8, 0, 7, 6,
5, 4, 3, 2, 1}` or `{8, 0, 1, 2, 3, 4, 5, 6, 7}` depending on facing. Offset
`8` is the center tile (the one the marker shows); offsets `0`-`7` each add
one more step in a different compass direction from that same center -
together, every direction exactly once. The result: the full 3x3 block
centered on the marker's tile gets planted, not just the tile itself.

**Tooling note:** these last few rounds of tuning used a throwaway
interactive web tool built mid-session (not part of this repo) that
composited real cropped pixels from `_pondWaterTexture*`,
`_mountainWaterTexture*` and `_mountainPathTilesTexture*` (extracted from
the ROM and shared into the conversation) with the same alpha-modulate math
`SPRITE_BLEND_ALPHA_MODULATED` uses, so texture and tint choices could be
checked against the real asset before touching the mod and reinstalling.
Scale is the one control that tool doesn't model faithfully (see round 9).
The farm pond and beach waves textures were never extracted, so that tool
still shows those two as procedural color approximations - something to
redo if those become relevant later.

The built `.nrm` is in `build/hm64_recomp_tool_target_marker.nrm`.

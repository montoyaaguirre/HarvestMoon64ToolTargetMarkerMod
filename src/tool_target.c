#include "modding.h"

#include "game/player.h"
#include "game/items.h"
#include "game/level.h"
#include "system/map.h"
#include "system/mapController.h"
#include "system/sprite.h"
#include "system/globalSprites.h"
#include "assetIndices/maps.h"
#include "assetIndices/sprites.h"
#include "buffers/buffers.h"
#include "ld_symbols.h"

static u8 isFieldTool(u8 tool) {
    switch (tool) {
        case SICKLE:
        case HOE:
        case AX:
        case HAMMER:
        case WATERING_CAN:
            return TRUE;
        default:
            return FALSE;
    }
}

// Seeds target a tile too, but via a different call than the field tools:
// useTurnipSeeds/useGrassSeeds/etc. (hm64-decomp/src/game/items.c) all call
// getOffsetTileCoordinates(0.0f, temp) - range 0, one tile closer to the
// player than the field tools' range 1. `temp` comes from a direction-
// dependent lookup (toolSweepOffsetsLeft/Right[toolUse.stepIndex]), but
// both arrays hold 8 (the same "straight ahead" code the field tools use)
// at index 0, which is what a single planting action reads - the
// direction split only matters for consecutive multi-tile sweeps, out of
// scope here same as the upgraded sickle/watering can.
static u8 isSeedTool(u8 tool) {
    return (tool >= TURNIP_SEEDS && tool <= BLUE_MIST_SEEDS) || tool == GRASS_SEEDS;
}

static u8 isTargetableTool(u8 tool) {
    return isFieldTool(tool) || isSeedTool(tool);
}

// Shared targeting: the tile the equipped tool is aimed at, in world space.
// Returns FALSE (and leaves *outWorldPos untouched) when the marker
// shouldn't be shown at all - holding an item, wrong tool, mid-swing, off
// the field grid, or not on the Farm map (the only map the ground decal
// below is confirmed to have a free map-object slot on - see below).
static u8 computeTargetWorldPos(Vec3f* outWorldPos) {

    Vec3f tile;
    Vec3f worldPos;
    f32 range;

    if (gBaseMapIndex != FARM || gPlayer.heldItem != 0 ||
        !isTargetableTool(gPlayer.currentTool) || gPlayer.actionHandler != 0) {
        return FALSE;
    }

    // Plain one-tile target: exact for the basic tools, the axe, the
    // hammer, and single-packet seed planting. Multi-tile sickle/watering
    // can/upgraded-seed-bag sweeps are out of scope for v1.
    range = isSeedTool(gPlayer.currentTool) ? 0.0f : 1.0f;
    tile = getOffsetTileCoordinates(range, 8);
    if (tile.y == 65535.0f) {
        return FALSE;
    }

    worldPos = getGroundObjectWorldPosition(MAIN_MAP_INDEX, (u8)tile.x, (u8)tile.z);
    if (worldPos.y == 65535.0f) {
        return FALSE;
    }

    *outWorldPos = worldPos;
    return TRUE;
}

// ============================================================================
// Ground decal (Farm only) - see README for why this is scoped to one map.
// ============================================================================

// Flat, ground-aligned sprites use SPRITE_BILLBOARD_XZ (see setupBitmapVertices
// in hm64-decomp/src/system/graphic.c) instead of the camera-facing
// SPRITE_BILLBOARD_XY entities use. The game already draws several
// translucent flat ground overlays this way: mountain water puddles, the
// pond, beach waves, mountain path tiles (all in hm64-decomp/src/game/level.c,
// loadLevelMapObjects). Reusing that existing pipeline via setMapObject is
// far less work than hand-authoring N64 display lists from scratch.
//
// There are only 6 of these "map object" sprite slots in total
// (MAP_OBJECT_1..6, hm64-decomp/src/assetIndices/sprites.h), shared globally
// across every map and already budgeted per-map for existing decorations.
// On Farm specifically: slot 1 (a home decoration) and slot 2 (the farm
// pond) are both persistently active, each using "metadata group A" and "B"
// respectively (hm64-decomp/src/buffers/mapObjectBuffer.h explicitly labels
// these "Shared Metadata Groups (used by multiple map objects)" - the
// palette/animation-frame/spritesheet-index buffers are shared pools, not
// per-slot). Slots 3-6 and metadata group C are not referenced anywhere in
// Farm's own loadLevelMapObjects case, so slot 4 (MAP_OBJECT_4, array index
// 3) with metadata group C is used here - confirmed free on Farm, not just
// assumed. This has NOT been checked for every other field map (Mountain 1
// alone is confirmed to have zero free slots, using all three of 1-3 for
// puddles and a bridge/pillar), which is why this mod is hard-restricted to
// Farm only.
//
// The buffer pairing (MAP_OBJECT_BRIDGE_SLOT_4_TEXTURE_1/2 + metadata group
// C) mirrors a combination the game already uses together elsewhere
// (Mountain 2 / Top of Mountain 1's bridges), rather than guessing at
// buffer sizes ourselves.
//
// Texture: reuses the farm pond's own ROM asset (_farmPondTexture...) into
// a *different* RAM destination/metadata group than the real farm pond
// uses, to avoid the cross-talk problem above. It's still a water texture,
// not a purpose-made "highlight" asset; the green tint below is doing the
// work of making that read as a marker instead of a puddle.
#define GROUND_DECAL_SPRITE_ID MAP_OBJECT_4
#define GROUND_DECAL_OBJECT_INDEX 3
#define GROUND_DECAL_HEIGHT_OFFSET 2.0f // lift off the terrain to avoid Z-fighting; untuned

// Final tuned values, found by hand in-game (see README).
#define GROUND_DECAL_SCALE 2.0f
#define GROUND_DECAL_OFFSET_X 2.5f
#define GROUND_DECAL_OFFSET_Y 0.0f

// Seeds plant a 3x3 block, not one tile: getOffsetTileCoordinates(0.0f, 8)
// resolves to the player's own tile (range 0 cancels the forward step - see
// computeTargetWorldPos), and toolUse.stepIndex then cycles through every
// one of the 9 possible directionalOffset values (8, then 0-7) across the
// 9 updates one planting action takes (useTurnipSeeds/etc. increment
// stepIndex each call and stop at 9). Offset 8 is the center tile itself;
// offsets 0-7 are every other direction from that same center, one step
// each - together, the full 3x3 block around it. There's no cheap way to
// show 9 separate tiles with this sprite-based approach (that would need 9
// map-object slots instead of 1, and there are only 6 in the whole game -
// see above), so this just scales the one decal up to approximate the full
// area instead of marking only its center tile.
#define GROUND_DECAL_SEED_SCALE_MULTIPLIER 3.0f

static u8 groundDecalTextureLoaded = FALSE;
static u8 groundDecalWasVisible = FALSE;

static void loadGroundDecalTexture(void) {

    f32 scale;

    dmaSprite(GROUND_DECAL_SPRITE_ID,
        (u32)&_farmPondTextureSegmentRomStart, (u32)&_farmPondTextureSegmentRomEnd,
        (u32)&_farmPondAssetsIndexSegmentRomStart, (u32)&_farmPondAssetsIndexSegmentRomEnd,
        (u32)&_farmPondSpritesheetIndexSegmentRomStart, (u32)&_farmPondSpritesheetIndexSegmentRomEnd,
        (u8*)MAP_OBJECT_BRIDGE_SLOT_4_TEXTURE_1, (u8*)MAP_OBJECT_BRIDGE_SLOT_4_TEXTURE_2,
        (u16*)MAP_OBJECT_METADATA_C_PALETTE, (AnimationFrameMetadata*)MAP_OBJECT_METADATA_C_ANIM_METADATA,
        (u8*)MAP_OBJECT_METADATA_C_SPRITESHEET_INDEX, (u32*)MAP_OBJECT_METADATA_C_TEXTURE_TO_PALETTE_LOOKUP,
        1, 1);

    scale = GROUND_DECAL_SCALE;
    if (isSeedTool(gPlayer.currentTool)) {
        scale *= GROUND_DECAL_SEED_SCALE_MULTIPLIER;
    }
    setSpriteScale(GROUND_DECAL_SPRITE_ID, scale, 1.0f, scale);
    setSpriteAxisMapping(GROUND_DECAL_SPRITE_ID, SPRITE_BILLBOARD_XZ);
    setSpriteTriangleWinding(GROUND_DECAL_SPRITE_ID, SPRITE_RENDERING_REVERSE_WINDING);
    setSpriteBlendMode(GROUND_DECAL_SPRITE_ID, SPRITE_BLEND_ALPHA_MODULATED);
    // Green tint over the water texture: picked in-tool against the real
    // pixel crop to read as "marked tile" rather than "puddle." Alpha 133/255
    // is a bit more opaque than the original half-transparent guess.
    setSpriteBaseRGBA(GROUND_DECAL_SPRITE_ID, 50, 236, 81, 133);
    setSpriteColor(GROUND_DECAL_SPRITE_ID, 255, 255, 255, 255);

    groundDecalTextureLoaded = TRUE;
}

static void hideGroundDecal(void) {
    if (groundDecalWasVisible) {
        deactivateMapObject(MAIN_MAP_INDEX, GROUND_DECAL_OBJECT_INDEX);
        groundDecalWasVisible = FALSE;
    }
}

// Re-loading the texture every time Farm is (re)entered, not just once ever,
// matters here: metadata group C is also used elsewhere (Mountain 2 / Top of
// Mountain 1's bridges), and those maps' own loadLevelMapObjects calls would
// overwrite the same RAM buffer with their own texture while visited. Without
// reloading on return to Farm, the decal could show whatever those other
// maps left behind.
RECOMP_HOOK_RETURN("loadMapAtSpawnPoint") void tool_target_ground_decal_on_map_load(void) {
    if (gBaseMapIndex == FARM) {
        // Force a real reload even if some other map's bridge (Mountain 2,
        // Top of Mountain 1 - also metadata group C) left the sprite
        // SPRITE_ACTIVE with its own texture in it. dmaSprite no-ops if the
        // sprite is already active, so without this, returning to Farm
        // could silently inherit whatever that other map left behind.
        deactivateSprite(GROUND_DECAL_SPRITE_ID);
        loadGroundDecalTexture();
    } else {
        groundDecalTextureLoaded = FALSE;
    }
}

static void updateGroundDecal(void) {

    Vec3f worldPos;

    if (gBaseMapIndex != FARM || !groundDecalTextureLoaded || !computeTargetWorldPos(&worldPos)) {
        hideGroundDecal();
        return;
    }

    // deactivateMapObject (called by hideGroundDecal) also calls
    // deactivateSprite on the underlying sprite, which clears its
    // SPRITE_ACTIVE flag (hm64-decomp/src/system/globalSprites.c). Without
    // this call, the first hide - a tool swing, picking up or dropping an
    // item, anything that makes computeTargetWorldPos return FALSE even
    // once - left the decal permanently dead afterward: setMapObject alone
    // never reactivates the sprite. dmaSprite itself already guards on
    // "already active" and no-ops instantly if so, so calling it every
    // frame here is cheap - it only actually re-DMAs on the frame right
    // after a hide.
    loadGroundDecalTexture();

    // setMapObject is normally called once per decoration at map-load time
    // (every call in loadLevelMapObjects does this); it also increments
    // mainMap[...].mapState.mapObjectCount, which turns out to be write-only
    // dead data (grep the whole decomp: nothing ever reads it), so calling
    // this every frame to reposition the decal is safe, if slightly
    // against the grain of how vanilla code uses it.
    //
    // setMapObject's x/z are NOT the same coordinate space
    // getGroundObjectWorldPosition returns. The per-frame renderer
    // (setupMapObjectSprites in hm64-decomp/src/system/map.c) computes:
    // finalX = (coordinates.x + mapWorldCenterX) - tileSizeX/2, then applies
    // the camera/view offset on top - i.e. it expects a value already
    // offset by half a tile the other way from what we have. Adding
    // tileSizeX/2 (and tileSizeZ/2) here cancels exactly that, which is what
    // was producing the tile-corner offset instead of tile-center.
    setMapObject(MAIN_MAP_INDEX, GROUND_DECAL_OBJECT_INDEX, GROUND_DECAL_SPRITE_ID, 0,
        worldPos.x + (mainMap[MAIN_MAP_INDEX].mapGrid.tileSizeX / 2.0f) + GROUND_DECAL_OFFSET_X,
        worldPos.y + GROUND_DECAL_HEIGHT_OFFSET,
        worldPos.z + (mainMap[MAIN_MAP_INDEX].mapGrid.tileSizeZ / 2.0f) + GROUND_DECAL_OFFSET_Y,
        0, 0xFE, 0, 0);

    groundDecalWasVisible = TRUE;
}

// ============================================================================

RECOMP_HOOK_RETURN("updatePlayerAction") void tool_target_on_update(void) {
    updateGroundDecal();
}

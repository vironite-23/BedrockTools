#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>

// MobSpawnOverlayModule
// ----------------------------------------------------------------------------
// Renders two floating, horizontal ring "shells" centered on the local player:
//   - a spawn ring, at the minimum distance mobs are allowed to spawn from the
//     player (24 blocks, matching vanilla behavior and util_sh's SP_CHECKER
//     in terrain.fsh: `float dist = length(wPos + vec3(0,1,0));`)
//   - a despawn ring, at the distance beyond which mobs are force-despawned.
//     This distance is tied to simulation/render distance rather than being a
//     fixed constant, so it is exposed as a config option (renderDistanceChunks)
//     the user sets to match their actual client render distance.
//
// Both boundaries are true spherical distances from the player (exactly like
// SP_CHECKER's length() check), not flat cylinders. To visualize that in 3D
// without filling the world with a solid sphere, each shell is drawn as a
// stack of flat, horizontal "latitude" circles at different heights relative
// to the player. Each circle's radius is sqrt(R^2 - dy^2), so the rings
// naturally shrink the further above/below the player they are, converging
// toward the top/bottom pole of the sphere - which is what makes it read as
// a floating sphere instead of a single flat disc.
class MobSpawnOverlayModule : public Module {
public:
    MobSpawnOverlayModule();
    ~MobSpawnOverlayModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // --- boundaries ---
    // Blocks from the player at which mobs are permitted to start spawning.
    // Vanilla/SP_CHECKER default is 24.
    float spawnRadius = 24.0f;

    // The client's render distance in chunks (16 blocks/chunk). Despawn
    // distance scales with simulation/render distance, so this is exposed
    // directly rather than assumed - set it to match your in-game video
    // settings for an accurate despawn ring.
    int renderDistanceChunks = 8;

    // Safety floor so the despawn shell never collapses inside the spawn
    // shell if renderDistanceChunks is set very low.
    float minDespawnMargin = 8.0f;

    // --- ring appearance ---
    bool showSpawnRing = true;
    bool showDespawnRing = true;

    // ARGB, alpha in the top byte (0xAARRGGBB) - matches every other
    // color setting in this codebase (e.g. LightOverlayModule::safeColor).
    // Set the alpha byte to control overlay transparency, e.g. 0x8000FF00
    // for a 50%-opaque green.
    uint32_t spawnColor = 0x8000FF00;    // translucent green
    uint32_t despawnColor = 0x80FF0000;  // translucent red

    // Number of segments per ring circle; higher = smoother, more geometry.
    int ringSegments = 48;

    // Vertical spacing (blocks) between stacked latitude rings.
    float ringVerticalSpacing = 4.0f;

    // Hard cap on how many rings (per side, i.e. above + below the
    // equator separately) get drawn for a single shell, regardless of how
    // small ringVerticalSpacing is. Prevents runaway geometry on large
    // despawn radii; spacing is scaled up transparently if the natural
    // count would exceed this.
    int maxRingsPerSide = 20;

    // The equatorial (dy == 0) ring is always drawn at full alpha; the
    // rest fade out toward the poles when this is enabled, which helps
    // the sphere shape read at a glance.
    bool fadeWithHeight = true;
    float minHeightAlphaScale = 0.25f;

private:
    void applyPatch();

    bool m_patched;
    void* m_patchTarget;

    void* m_tessBeginAddr;
    void* m_tessColorAddr;
    void* m_tessVertexAddr;
    void* m_renderMeshAddr;
    void* m_renderMaterialGroupAddr;
};

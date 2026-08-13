#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <string>

class MobSpawnOverlayModule : public Module {
public:
    MobSpawnOverlayModule();
    ~MobSpawnOverlayModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Layout
    bool showHorizontal;      // draw the flat ring(s) at feet level
    bool showVertical;        // draw vertical pillars around the ring
    bool showInnerRing;       // draw the "no spawn" exclusion ring (vanilla: 24 blocks)
    bool showOtherPlayers;    // also draw the overlay around other visible players

    float innerRadius;        // blocks, exclusion radius around a player
    int renderDistanceChunks; // outer boundary = renderDistanceChunks * 16 blocks

    // Appearance
    uint32_t innerColor;   // color for the exclusion ring / pillars
    uint32_t outerColor;   // color for the outer boundary ring / pillars
    float opacity;         // 0..1, multiplies the alpha of both colors

private:
    bool m_patched;
    void* m_patchTarget;

    void* m_tessBeginAddr;
    void* m_tessColorAddr;
    void* m_tessVertexAddr;
    void* m_renderMaterialGroupAddr;

    void applyPatch();
};

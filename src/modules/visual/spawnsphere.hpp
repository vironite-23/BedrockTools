#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <string>

class SpawnSphereModule : public Module {
public:
    SpawnSphereModule();
    ~SpawnSphereModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Layout
    float innerRadius;         // blocks, vanilla no-spawn exclusion sphere (24)
    int renderDistanceChunks;  // outer sphere radius = renderDistanceChunks * 16 blocks
    float bandSpacing;         // vertical distance (blocks) between latitude rings
    int meridianCount;         // number of vertical "longitude" lines connecting the rings
    bool blockyEdges;          // false = smooth polygon circle, true = follows block outlines

    // Appearance
    uint32_t innerColor;
    uint32_t outerColor;
    float opacity;              // 0..1, multiplies alpha of both colors

private:
    bool m_patched;
    void* m_patchTarget;

    void* m_tessBeginAddr;
    void* m_tessColorAddr;
    void* m_tessVertexAddr;
    void* m_renderMaterialGroupAddr;

    void applyPatch();
};

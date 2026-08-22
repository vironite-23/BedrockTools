#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <cstdint>

class MobSpawnPointModule : public Module {
public:
    MobSpawnPointModule();
    ~MobSpawnPointModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Outer radius of the spawn-distance overlay in blocks.
    int renderDistance = 32;

    // Alpha multiplier for the translucent overlay.
    float transparency = 0.25f;

    // RGB color in 0xAARRGGBB form. Alpha is ignored and controlled by transparency.
    uint32_t overlayColor = 0xFF55FF55;

private:
    void applyPatch();

    bool m_patched = false;
    void* m_patchTarget = nullptr;

    void* m_tessBeginAddr = nullptr;
    void* m_tessColorAddr = nullptr;
    void* m_tessVertexAddr = nullptr;
    void* m_renderMaterialGroupAddr = nullptr;
};

#pragma once

#include <imgui.h>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

struct Vec3 {
    float x, y, z;
};

struct Vec2 {
    float x, y;
};

// Konfigurasi Fitur Mob Spawn Overlay
struct MobSpawnConfig {
    bool enabled = false;
    ImVec4 overlayColor = ImVec4(1.0f, 0.2f, 0.2f, 0.4f); // Warna RGBA
    float transparency = 0.4f;                           // Opasitas (0.0 - 1.0)
    bool showOtherPlayers = true;                        // Tampilkan overlay player lain
    int forceRenderDistance = 0;                         // 0 = Otomatis ikuti game
    bool showInnerRadius = true;                         // Tampilkan batas aman 24 block
    bool showVerticalBounds = true;                      // Tampilkan garis batas vertikal
};

class MobSpawnOverlay {
public:
    static MobSpawnOverlay& getInstance();

    // Getter untuk konfigurasi (diakses oleh Mod Menu ImGui)
    MobSpawnConfig& getConfig() { return m_config; }

    // Dipanggil setiap frame rendering ImGui
    void onRender(ImDrawList* drawList);

    // Fungsi pembantu matematika World to Screen
    static bool worldToScreen(const Vec3& worldPos, Vec2& screenPos, const float* viewMatrix, const float* projMatrix, float screenWidth, float screenHeight);

private:
    MobSpawnOverlay() = default;
    MobSpawnConfig m_config;

    void renderPlayerOverlay(ImDrawList* drawList, const Vec3& playerPos, int renderDistance, const float* viewProj);
    void drawCircle3D(ImDrawList* drawList, const Vec3& center, float radius, float yOffset, ImU32 color, const float* viewProj, int segments = 48);
    void drawBoundingCylinder(ImDrawList* drawList, const Vec3& center, float minRadius, float maxRadius, float heightRange, ImU32 color, const float* viewProj);
};

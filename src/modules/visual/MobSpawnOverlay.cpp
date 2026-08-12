#include "MobSpawnOverlay.hpp"
#include <cmath>

// Implementasi Singleton
MobSpawnOverlay& MobSpawnOverlay::getInstance() {
    static MobSpawnOverlay instance;
    return instance;
}

// Konversi Koordinat 3D World ke 2D Screen Space
bool MobSpawnOverlay::worldToScreen(const Vec3& worldPos, Vec2& screenPos, const float* viewMatrix, const float* projMatrix, float screenWidth, float screenHeight) {
    // Gabungkan View * Proj Matrix (simulasi)
    float clipX = worldPos.x * viewMatrix[0] + worldPos.y * viewMatrix[4] + worldPos.z * viewMatrix[8] + viewMatrix[12];
    float clipY = worldPos.x * viewMatrix[1] + worldPos.y * viewMatrix[5] + worldPos.z * viewMatrix[9] + viewMatrix[13];
    float clipZ = worldPos.x * viewMatrix[2] + worldPos.y * viewMatrix[6] + worldPos.z * viewMatrix[10] + viewMatrix[14];
    float clipW = worldPos.x * viewMatrix[3] + worldPos.y * viewMatrix[7] + worldPos.z * viewMatrix[11] + viewMatrix[15];

    if (clipW < 0.1f) return false; // Dibelakang kamera

    Vec3 ndc;
    ndc.x = clipX / clipW;
    ndc.y = clipY / clipW;

    screenPos.x = (screenWidth / 2.0f) * (ndc.x + 1.0f);
    screenPos.y = (screenHeight / 2.0f) * (1.0f - ndc.y);

    return true;
}

void MobSpawnOverlay::drawCircle3D(ImDrawList* drawList, const Vec3& center, float radius, float yOffset, ImU32 color, const float* viewProj, int segments) {
    ImVec2 screenPoints[64];
    int validPoints = 0;
    
    ImGuiIO& io = ImGui::GetIO();
    float sw = io.DisplaySize.x;
    float sh = io.DisplaySize.y;

    for (int i = 0; i < segments; ++i) {
        float angle = (2.0f * 3.14159265f * i) / segments;
        Vec3 worldPoint = {
            center.x + radius * cosf(angle),
            center.y + yOffset,
            center.z + radius * sinf(angle)
        };

        Vec2 screenPoint;
        if (worldToScreen(worldPoint, screenPoint, viewProj, viewProj + 16, sw, sh)) {
            screenPoints[validPoints++] = ImVec2(screenPoint.x, screenPoint.y);
        }
    }

    // Gambar garis melingkar 3D di layar 2D ImGui
    for (int i = 0; i < validPoints - 1; ++i) {
        drawList->AddLine(screenPoints[i], screenPoints[i + 1], color, 2.0f);
    }
    if (validPoints > 2) {
        drawList->AddLine(screenPoints[validPoints - 1], screenPoints[0], color, 2.0f);
    }
}

void MobSpawnOverlay::drawBoundingCylinder(ImDrawList* drawList, const Vec3& center, float minRadius, float maxRadius, float heightRange, ImU32 color, const float* viewProj) {
    // Batas Atas dan Batas Bawah Vertikal
    float topY = heightRange;
    float bottomY = -heightRange;

    // 1. Gambar Lingkaran Outer (Maksimal Spawn Area)
    drawCircle3D(drawList, center, maxRadius, topY, color, viewProj);
    drawCircle3D(drawList, center, maxRadius, bottomY, color, viewProj);

    // 2. Gambar Lingkaran Inner (24 Block No-Spawn Area)
    if (m_config.showInnerRadius) {
        ImU32 innerColor = (color & 0x00FFFFFF) | 0x80000000; // Opasitas lebih rendah
        drawCircle3D(drawList, center, minRadius, topY, innerColor, viewProj);
        drawCircle3D(drawList, center, minRadius, bottomY, innerColor, viewProj);
    }

    // 3. Garis Pilar Vertikal
    if (m_config.showVerticalBounds) {
        ImGuiIO& io = ImGui::GetIO();
        float sw = io.DisplaySize.x;
        float sh = io.DisplaySize.y;

        for (int i = 0; i < 4; ++i) {
            float angle = (2.0f * 3.14159265f * i) / 4.0f;
            Vec3 pTop = { center.x + maxRadius * cosf(angle), center.y + topY, center.z + maxRadius * sinf(angle) };
            Vec3 pBottom = { center.x + maxRadius * cosf(angle), center.y + bottomY, center.z + maxRadius * sinf(angle) };

            Vec2 sTop, sBottom;
            if (worldToScreen(pTop, sTop, viewProj, viewProj + 16, sw, sh) &&
                worldToScreen(pBottom, sBottom, viewProj, viewProj + 16, sw, sh)) {
                drawList->AddLine(ImVec2(sTop.x, sTop.y), ImVec2(sBottom.x, sBottom.y), color, 1.5f);
            }
        }
    }
}

void MobSpawnOverlay::renderPlayerOverlay(ImDrawList* drawList, const Vec3& playerPos, int renderDistance, const float* viewProj) {
    // Hitung Radius Spawn berdasarkan Render Distance
    // Minimal = 24 Block
    // Maksimal = Sim/Render Distance * 16 (chunk size), dicap pada batasan MCBE (44 - 128 block)
    float minRadius = 24.0f;
    float maxRadius = std::min(128.0f, std::max(44.0f, static_cast<float>(renderDistance * 16)));
    float verticalRange = 24.0f; // Batas vertikal default MCBE

    // Tentukan Warna ImGui RGBA dengan Alpha
    ImVec4 col = m_config.overlayColor;
    ImU32 colorU32 = ImGui::ColorConvertFloat4ToU32(ImVec4(col.x, col.y, col.z, m_config.transparency));

    drawBoundingCylinder(drawList, playerPos, minRadius, maxRadius, verticalRange, colorU32, viewProj);
}

void MobSpawnOverlay::onRender(ImDrawList* drawList) {
    if (!m_config.enabled) return;

    // Catatan: Di dalam SDK BedrockTools, kita mengambil pointer LocalPlayer & Level
    // Contoh pengambilan data instance dari BedrockTools SDK:
    /*
    auto localPlayer = SDK::GetLocalPlayer();
    if (!localPlayer) return;

    Vec3 localPos = localPlayer->getPosition();
    int currentRenderDistance = m_config.forceRenderDistance > 0 ? 
                                m_config.forceRenderDistance : SDK::GetRenderDistance();
    float matrixHolder[32]; // View & Proj Matrices
    SDK::GetCameraMatrix(matrixHolder);

    // 1. Render Overlay untuk Local Player
    renderPlayerOverlay(drawList, localPos, currentRenderDistance, matrixHolder);

    // 2. Render Overlay untuk Player Lain (jika diaktifkan)
    if (m_config.showOtherPlayers) {
        auto playerList = SDK::GetLevel()->getPlayers();
        for (auto player : playerList) {
            if (player == localPlayer) continue;
            Vec3 otherPos = player->getPosition();
            renderPlayerOverlay(drawList, otherPos, currentRenderDistance, matrixHolder);
        }
    }
    */
}

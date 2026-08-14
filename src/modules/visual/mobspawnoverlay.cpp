#include "mobspawnoverlay.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <cmath>
#include <string>
#include <cstring>
#include <vector>
#include <algorithm>

typedef void (*Tessellator_begin_t)(void* tessellator, void* debugCallback, int primitiveMode, int vertexCount, int noIndices);
typedef void (*Tessellator_color_t)(void* tessellator, float r, float g, float b, float a);
typedef void (*Tessellator_vertex_t)(void* tessellator, float x, float y, float z);
typedef void (*MeshHelpers_renderMeshImmediately_t)(void* screenContext, void* tessellator, void* material, char* pad);

struct HashedString {
    uint64_t mStrHash;
    std::string mStr;
    mutable const HashedString* mLastMatch;

    HashedString() : mStrHash(0), mStr(), mLastMatch(nullptr) {}

    explicit HashedString(const char* str) : mLastMatch(nullptr) {
        mStr = str ? str : "";
        mStrHash = computeHash(mStr);
    }

private:
    static uint64_t computeHash(const std::string& str) {
        if (str.empty()) return 0;
        constexpr uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr uint64_t kPrime = 0x100000001B3ULL;
        uint64_t hash = kOffset;
        for (char ch : str)
            hash = static_cast<uint64_t>(static_cast<unsigned char>(ch)) ^ (kPrime * hash);
        return hash;
    }
};

struct MaterialPtr {
    void* sharedPtrData[2];
};

static uintptr_t resolveADRP(uint32_t* insns, size_t count, uint32_t targetReg) {
    for (size_t i = 0; i < count; i++) {
        uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;

        if ((insn & 0x9F000000) == 0x90000000) {
            uintptr_t page = ((uintptr_t)&insns[i] & ~0xFFFULL)
                           + ((int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29) & 3) << 43) >> 31);

            for (size_t j = i + 1; j < count; j++) {
                uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetReg &&
                    (add & 0x1F) == targetReg) {
                    uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg) break;
            }
        }
        if ((insn & 0x9F000000) == 0x10000000) {
            int64_t imm = (int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29)) << 43) >> 43;
            return (uintptr_t)&insns[i] + imm;
        }
    }
    return 0;
}

static MobSpawnOverlayModule* g_mobSpawnOverlayMod = nullptr;

static Tessellator_begin_t                s_tessBegin = nullptr;
static Tessellator_color_t                s_tessColor = nullptr;
static Tessellator_vertex_t               s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;

static MaterialPtr* s_matSelection = nullptr;
static uintptr_t    s_renderMaterialGroup = 0;

static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3);

static bedrocktools::sdk::Vec3 g_playerPos = {0.f, 0.f, 0.f};
static bool g_havePlayerPos = false;

static void s_mobSpawnOverlayTickCallback(void* _this) {
    if (!g_mobSpawnOverlayMod || !g_mobSpawnOverlayMod->enabled) return;
    uintptr_t svc = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::Actor::mStateVectorComponent);
    if (svc != 0) {
        g_playerPos = *(bedrocktools::sdk::Vec3*)svc;
        g_havePlayerPos = true;
    }
}

static MaterialPtr* getMaterial(const char* name) {
    if (!s_renderMaterialGroup) return nullptr;

    HashedString hs(name);

    void** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    if (!vtable || !vtable[2]) return nullptr;

    using getMat_t = MaterialPtr*(*)(void*, const HashedString*);
    return reinterpret_cast<getMat_t>(vtable[2])((void*)s_renderMaterialGroup, &hs);
}

static void ensureMaterials() {
    if (s_matSelection) return;
    if (!s_renderMaterialGroup) return;

    if (!s_matSelection) s_matSelection = getMaterial("selection_box");
}

// One flat, horizontal "latitude" ring belonging to a spawn/despawn sphere.
struct SpawnRing {
    float dy;          // height offset from the sphere center
    float radius;       // sqrt(R^2 - dy^2)
    float alphaScale;   // 0..1, multiplies the shell's base alpha
};

// Slices a sphere of radius R (centered on the player) into a stack of flat
// horizontal rings, spaced `spacing` blocks apart vertically, always
// including the dy == 0 equator. This is the 3D analogue of SP_CHECKER's
// `length(wPos + vec3(0,1,0))` spherical distance check, decomposed into
// something that can be drawn as line loops instead of evaluated per-pixel.
static std::vector<SpawnRing> buildSphereRings(float radius, float spacing, int maxRingsPerSide, bool fadeWithHeight, float minAlphaScale) {
    std::vector<SpawnRing> rings;
    if (radius <= 0.01f) return rings;

    spacing = std::max(spacing, 0.1f);
    int maxSteps = static_cast<int>(std::floor(radius / spacing));
    if (maxRingsPerSide > 0 && maxSteps > maxRingsPerSide) {
        spacing = radius / static_cast<float>(maxRingsPerSide);
        maxSteps = maxRingsPerSide;
    }

    rings.reserve(static_cast<size_t>(maxSteps) * 2 + 1);
    for (int i = -maxSteps; i <= maxSteps; ++i) {
        float dy = static_cast<float>(i) * spacing;
        float sliceRadiusSq = radius * radius - dy * dy;
        if (sliceRadiusSq <= 0.0001f) continue;

        float sliceRadius = std::sqrt(sliceRadiusSq);
        float alphaScale = 1.0f;
        if (fadeWithHeight) {
            float t = std::min(std::fabs(dy) / radius, 1.0f);
            alphaScale = 1.0f - t * (1.0f - minAlphaScale);
        }
        rings.push_back({dy, sliceRadius, alphaScale});
    }
    return rings;
}

static void drawSphereShell(void* tessellator, void* screenContext, void* material,
                             const bedrocktools::sdk::Vec3& center, float camX, float camY, float camZ,
                             const std::vector<SpawnRing>& rings, uint32_t baseColor, int segments) {
    if (rings.empty() || segments < 3) return;

    int vertexCount = static_cast<int>(rings.size()) * segments * 2;
    s_tessBegin(tessellator, nullptr, 4, vertexCount, 0);

    const float baseR = ((baseColor >> 16) & 0xFF) / 255.0f;
    const float baseG = ((baseColor >>  8) & 0xFF) / 255.0f;
    const float baseB = ((baseColor      ) & 0xFF) / 255.0f;
    const float baseA = ((baseColor >> 24) & 0xFF) / 255.0f;

    const float step = 6.28318530718f / static_cast<float>(segments);

    for (const auto& ring : rings) {
        s_tessColor(tessellator, baseR, baseG, baseB, baseA * ring.alphaScale);

        const float y = center.y + ring.dy - camY;
        for (int i = 0; i < segments; ++i) {
            float a1 = step * static_cast<float>(i);
            float a2 = step * static_cast<float>(i + 1);

            float x1 = center.x + std::cos(a1) * ring.radius - camX;
            float z1 = center.z + std::sin(a1) * ring.radius - camZ;
            float x2 = center.x + std::cos(a2) * ring.radius - camX;
            float z2 = center.z + std::sin(a2) * ring.radius - camZ;

            s_tessVertex(tessellator, x1, y, z1);
            s_tessVertex(tessellator, x2, y, z2);
        }
    }

    char pad[0x58];
    memset(pad, 0, sizeof(pad));
    s_renderMesh(screenContext, tessellator, material, pad);
}

static void _renderLevel_hook(void* _this, void* screenContext, void* a3) {
    if (_renderLevel_orig) {
        _renderLevel_orig(_this, screenContext, a3);
    }

    if (!g_mobSpawnOverlayMod || !g_mobSpawnOverlayMod->enabled) return;
    if (!g_havePlayerPos) return;
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;
    if (!screenContext || (uintptr_t)screenContext < 0x1000) return;

    uintptr_t tessellatorPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    if (!tessellatorPtr || tessellatorPtr < 0x1000) return;
    void* tessellator = (void*)tessellatorPtr;

    uintptr_t lrpPtr = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (!lrpPtr || lrpPtr < 0x1000) return;

    float camX = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);
    float camY = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 4);
    float camZ = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 8);

    ensureMaterials();

    void* matOverlay = s_matSelection ? (void*)s_matSelection
                                       : (void*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);

    uintptr_t colorHolderPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (!colorHolderPtr || colorHolderPtr < 0x1000) return;
    float* colorHolder = (float*)colorHolderPtr;

    float savedColor[4] = { colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3] };
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    // Sphere center matches SP_CHECKER's convention in terrain.fsh:
    // `length(wPos + vec3(0,1,0))`, i.e. one block above the tracked
    // player position, rather than the raw feet position.
    bedrocktools::sdk::Vec3 center = { g_playerPos.x, g_playerPos.y + 1.0f, g_playerPos.z };

    const int segments = std::max(g_mobSpawnOverlayMod->ringSegments, 3);
    const float spacing = g_mobSpawnOverlayMod->ringVerticalSpacing;
    const int maxRingsPerSide = g_mobSpawnOverlayMod->maxRingsPerSide;
    const bool fade = g_mobSpawnOverlayMod->fadeWithHeight;
    const float minAlphaScale = g_mobSpawnOverlayMod->minHeightAlphaScale;

    const float spawnRadius = std::max(g_mobSpawnOverlayMod->spawnRadius, 1.0f);
    const float despawnRadius = std::max(
        static_cast<float>(g_mobSpawnOverlayMod->renderDistanceChunks) * 16.0f,
        spawnRadius + g_mobSpawnOverlayMod->minDespawnMargin
    );

    if (g_mobSpawnOverlayMod->showDespawnRing) {
        auto rings = buildSphereRings(despawnRadius, spacing, maxRingsPerSide, fade, minAlphaScale);
        drawSphereShell(tessellator, screenContext, matOverlay, center, camX, camY, camZ,
                         rings, g_mobSpawnOverlayMod->despawnColor, segments);
    }

    if (g_mobSpawnOverlayMod->showSpawnRing) {
        auto rings = buildSphereRings(spawnRadius, spacing, maxRingsPerSide, fade, minAlphaScale);
        drawSphereShell(tessellator, screenContext, matOverlay, center, camX, camY, camZ,
                         rings, g_mobSpawnOverlayMod->spawnColor, segments);
    }

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

MobSpawnOverlayModule::MobSpawnOverlayModule()
    : Module("Mob Spawn Overlay", "Floating spawn/despawn sphere rings around the player.") {

    showInMenu = true;
    m_patched = false;
    m_patchTarget = nullptr;
    m_tessBeginAddr = nullptr;
    m_tessColorAddr = nullptr;
    m_tessVertexAddr = nullptr;
    m_renderMeshAddr = nullptr;
    m_renderMaterialGroupAddr = nullptr;
    g_mobSpawnOverlayMod = this;
}

MobSpawnOverlayModule::~MobSpawnOverlayModule() {
    if (g_mobSpawnOverlayMod == this) g_mobSpawnOverlayMod = nullptr;
}

void MobSpawnOverlayModule::onInit() {
    uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (addr != 0) {
        m_patchTarget = (void*)addr;
    }

    uintptr_t tb = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin);
    if (tb) { m_tessBeginAddr = (void*)tb; s_tessBegin = (Tessellator_begin_t)tb; }

    uintptr_t tc = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor);
    if (tc) { m_tessColorAddr = (void*)tc; s_tessColor = (Tessellator_color_t)tc; }

    uintptr_t tv = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex);
    if (tv) { m_tessVertexAddr = (void*)tv; s_tessVertex = (Tessellator_vertex_t)tv; }

    uintptr_t rm = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (rm) {
        m_renderMeshAddr = (void*)rm;
        s_renderMesh = (MeshHelpers_renderMeshImmediately_t)rm;
    } else {
        uintptr_t rm5 = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
        if (rm5) s_renderMesh = (MeshHelpers_renderMeshImmediately_t)rm5;
    }

    uintptr_t rmg = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
    if (rmg) {
        m_renderMaterialGroupAddr = (void*)rmg;
        uintptr_t groupAddr = resolveADRP(reinterpret_cast<uint32_t*>(rmg), 2, 0);
        if (groupAddr) {
            s_renderMaterialGroup = groupAddr + bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
        }
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) { s_mobSpawnOverlayTickCallback(event.player); });
}

void MobSpawnOverlayModule::applyPatch() {
    if (m_patched) return;
    if (!m_patchTarget) {
        return;
    }
    bedrocktools::hooks::install(m_patchTarget, (void*)_renderLevel_hook, (void**)&_renderLevel_orig);
    m_patched = true;
}

void MobSpawnOverlayModule::onEnable() {
    applyPatch();
}

void MobSpawnOverlayModule::onDisable() {
}

void MobSpawnOverlayModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    spawnRadius = j.value("spawnRadius", spawnRadius);
    renderDistanceChunks = j.value("renderDistanceChunks", renderDistanceChunks);
    minDespawnMargin = j.value("minDespawnMargin", minDespawnMargin);

    showSpawnRing = j.value("showSpawnRing", showSpawnRing);
    showDespawnRing = j.value("showDespawnRing", showDespawnRing);

    ringSegments = j.value("ringSegments", ringSegments);
    ringVerticalSpacing = j.value("ringVerticalSpacing", ringVerticalSpacing);
    maxRingsPerSide = j.value("maxRingsPerSide", maxRingsPerSide);

    fadeWithHeight = j.value("fadeWithHeight", fadeWithHeight);
    minHeightAlphaScale = j.value("minHeightAlphaScale", minHeightAlphaScale);

    auto parseColor = [&](const std::string& key, uint32_t& outColor) {
        if (j.contains(key)) {
            std::string hexStr = j[key].get<std::string>();
            if (!hexStr.empty() && hexStr[0] == '#') {
                try { outColor = std::stoul(hexStr.substr(1), nullptr, 16); } catch (...) {}
            }
        }
    };

    parseColor("spawnColor", spawnColor);
    parseColor("despawnColor", despawnColor);
}

void MobSpawnOverlayModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["spawnRadius"] = spawnRadius;
    j["renderDistanceChunks"] = renderDistanceChunks;
    j["minDespawnMargin"] = minDespawnMargin;

    j["showSpawnRing"] = showSpawnRing;
    j["showDespawnRing"] = showDespawnRing;

    j["ringSegments"] = ringSegments;
    j["ringVerticalSpacing"] = ringVerticalSpacing;
    j["maxRingsPerSide"] = maxRingsPerSide;

    j["fadeWithHeight"] = fadeWithHeight;
    j["minHeightAlphaScale"] = minHeightAlphaScale;

    char hexS[12], hexD[12];
    snprintf(hexS, sizeof(hexS), "#%08X", spawnColor);
    snprintf(hexD, sizeof(hexD), "#%08X", despawnColor);

    j["spawnColor"] = std::string(hexS);
    j["despawnColor"] = std::string(hexD);
}

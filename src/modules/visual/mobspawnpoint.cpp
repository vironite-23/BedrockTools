#include "mobspawnpoint.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/events/EventBus.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

struct BlockPos {
    int x, y, z;
};

typedef void (*Tessellator_begin_t)(
    void* tessellator, void* debugCallback, int primitiveMode,
    int vertexCount, int noIndices);
typedef void (*Tessellator_color_t)(
    void* tessellator, float r, float g, float b, float a);
typedef void (*Tessellator_vertex_t)(
    void* tessellator, float x, float y, float z);
typedef void (*MeshHelpers_renderMeshImmediately_t)(
    void* screenContext, void* tessellator, void* material, char* pad);

typedef bool (*BlockSource_isSolidBlockingBlock_t)(
    void* region, const BlockPos& pos);

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
        for (char ch : str) {
            hash = static_cast<uint64_t>(
                static_cast<unsigned char>(ch)) ^ (kPrime * hash);
        }
        return hash;
    }
};

struct MaterialPtr {
    void* sharedPtrData[2];
};

static uintptr_t resolveADRP(uint32_t* insns, size_t count, uint32_t targetReg) {
    for (size_t i = 0; i < count; ++i) {
        uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;

        if ((insn & 0x9F000000) == 0x90000000) {
            uintptr_t page =
                ((uintptr_t)&insns[i] & ~0xFFFULL) +
                ((int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC |
                                      (insn >> 29) & 3) << 43) >> 31);

            for (size_t j = i + 1; j < count; ++j) {
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
            int64_t imm =
                (int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC |
                                     (insn >> 29)) << 43) >> 43;
            return (uintptr_t)&insns[i] + imm;
        }
    }
    return 0;
}

static MobSpawnPointModule* g_mobSpawnPointMod = nullptr;

static Tessellator_begin_t s_tessBegin = nullptr;
static Tessellator_color_t s_tessColor = nullptr;
static Tessellator_vertex_t s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;
static BlockSource_isSolidBlockingBlock_t s_isSolidBlockingBlock = nullptr;

static MaterialPtr* s_matSelection = nullptr;
static uintptr_t s_renderMaterialGroup = 0;

static void (*s_renderLevelOrig)(void*, void*, void*) = nullptr;
static bedrocktools::sdk::Vec3 g_playerPos = {0.f, 0.f, 0.f};
static void* g_localPlayer = nullptr;

static void tickCallback(void* player) {
    if (!g_mobSpawnPointMod || !g_mobSpawnPointMod->enabled || !player) return;

    g_localPlayer = player;

    uintptr_t svc = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uintptr_t>(player) +
        bedrocktools::sdk::offsets::Actor::mStateVectorComponent);

    if (svc) {
        g_playerPos = *reinterpret_cast<bedrocktools::sdk::Vec3*>(svc);
    }
}

static MaterialPtr* getMaterial(const char* name) {
    if (!s_renderMaterialGroup) return nullptr;

    HashedString hs(name);
    void** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);

    const auto getMaterialIndex =
        bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial;
    if (!vtable || !vtable[getMaterialIndex]) return nullptr;

    using getMat_t = MaterialPtr*(*)(void*, const HashedString*);
    return reinterpret_cast<getMat_t>(vtable[getMaterialIndex])(
        reinterpret_cast<void*>(s_renderMaterialGroup), &hs);
}

static void ensureMaterial() {
    if (s_matSelection || !s_renderMaterialGroup) return;
    s_matSelection = getMaterial("selection_box");
}

static void drawRingOverlay(
    void* screenContext,
    void* tessellator,
    void* material,
    float camX, float camY, float camZ,
    void* region,
    int blockY,
    int outerRadius,
    float transparency,
    uint32_t color) {

    // Hostile mobs cannot normally spawn immediately around the player.
    // Keep the conventional 24-block inner exclusion radius and use the
    // configured render distance as the outer edge.
    constexpr int kSpawnMinDistance = 24;

    if (outerRadius <= kSpawnMinDistance) return;

    const int r = std::min(std::max(outerRadius, kSpawnMinDistance + 1), 128);
    const int inner = kSpawnMinDistance;

    std::vector<bedrocktools::sdk::Vec3> vertices;
    vertices.reserve(static_cast<size_t>(r * r * 8));

    const int minX = static_cast<int>(std::floor(g_playerPos.x)) - r;
    const int maxX = static_cast<int>(std::floor(g_playerPos.x)) + r;
    const int minZ = static_cast<int>(std::floor(g_playerPos.z)) - r;
    const int maxZ = static_cast<int>(std::floor(g_playerPos.z)) + r;

    // Draw only the top face of solid blocks. This keeps the overlay attached
    // to the terrain rather than floating as a flat screen-space circle.
    for (int x = minX; x <= maxX; ++x) {
        for (int z = minZ; z <= maxZ; ++z) {
            const float dx = (static_cast<float>(x) + 0.5f) - g_playerPos.x;
            const float dz = (static_cast<float>(z) + 0.5f) - g_playerPos.z;
            const float distSq = dx * dx + dz * dz;

            if (distSq < static_cast<float>(inner * inner) ||
                distSq > static_cast<float>(r * r)) {
                continue;
            }

            if (s_isSolidBlockingBlock) {
                BlockPos bp = {x, blockY, z};
                if (!s_isSolidBlockingBlock(region, bp)) continue;

                // Do not paint the top of a block if another solid block is
                // directly above it.
                BlockPos above = {x, blockY + 1, z};
                if (s_isSolidBlockingBlock(region, above)) continue;
            }

            constexpr float yOffset = 0.0125f;
            const float y = static_cast<float>(blockY + 1) + yOffset;

            vertices.push_back({static_cast<float>(x) - camX, y - camY,
                                static_cast<float>(z) - camZ});
            vertices.push_back({static_cast<float>(x + 1) - camX, y - camY,
                                static_cast<float>(z) - camZ});
            vertices.push_back({static_cast<float>(x + 1) - camX, y - camY,
                                static_cast<float>(z + 1) - camZ});
            vertices.push_back({static_cast<float>(x) - camX, y - camY,
                                static_cast<float>(z + 1) - camZ});
        }
    }

    if (vertices.empty()) return;

    const float red = ((color >> 16) & 0xFF) / 255.0f;
    const float green = ((color >> 8) & 0xFF) / 255.0f;
    const float blue = (color & 0xFF) / 255.0f;
    const float colorAlpha = ((color >> 24) & 0xFF) / 255.0f;
    const float alpha = std::clamp(transparency, 0.0f, 1.0f) * colorAlpha;

    // Primitive mode 1 is the quad-list mode already used by the project's
    // terrain overlays (for example Breadcrumbs).
    s_tessBegin(tessellator, nullptr, 1,
                 static_cast<int>(vertices.size()), 0);
    s_tessColor(tessellator, red, green, blue, alpha);

    for (const auto& v : vertices) {
        s_tessVertex(tessellator, v.x, v.y, v.z);
    }

    char pad[0x58];
    std::memset(pad, 0, sizeof(pad));
    s_renderMesh(screenContext, tessellator, material, pad);
}

static void renderLevelHook(void* self, void* screenContext, void* a3) {
    if (s_renderLevelOrig) {
        s_renderLevelOrig(self, screenContext, a3);
    }

    if (!g_mobSpawnPointMod || !g_mobSpawnPointMod->enabled) return;
    if (!g_localPlayer || !screenContext) return;
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;
    if (!s_isSolidBlockingBlock) return;

    const uintptr_t sc = reinterpret_cast<uintptr_t>(screenContext);
    if (sc < 0x1000) return;

    uintptr_t tessellatorPtr = *reinterpret_cast<uintptr_t*>(
        sc + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    if (!tessellatorPtr || tessellatorPtr < 0x1000) return;

    uintptr_t lrpPtr = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uintptr_t>(self) +
        bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (!lrpPtr || lrpPtr < 0x1000) return;

    const float camX = *reinterpret_cast<float*>(
        lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);
    const float camY = *reinterpret_cast<float*>(
        lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 4);
    const float camZ = *reinterpret_cast<float*>(
        lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 8);

    uintptr_t dimension = *reinterpret_cast<uintptr_t*>(
        reinterpret_cast<uintptr_t>(g_localPlayer) +
        bedrocktools::sdk::offsets::Actor::mDimension);
    if (!dimension) return;

    uintptr_t blockSource = *reinterpret_cast<uintptr_t*>(
        dimension + bedrocktools::sdk::offsets::Dimension::mBlockSource);
    if (!blockSource) return;

    ensureMaterial();

    void* material = s_matSelection
        ? reinterpret_cast<void*>(s_matSelection)
        : reinterpret_cast<void*>(
            lrpPtr +
            bedrocktools::sdk::offsets::LevelRendererPlayer::
                mSelectionOverlayMaterial);

    uintptr_t colorHolderPtr = *reinterpret_cast<uintptr_t*>(
        sc + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (!colorHolderPtr || colorHolderPtr < 0x1000) return;

    float* colorHolder = reinterpret_cast<float*>(colorHolderPtr);
    float savedColor[4] = {
        colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3]
    };

    // Prevent the current scene color from multiplying the configured color.
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    const int playerBlockY = static_cast<int>(std::floor(g_playerPos.y)) - 1;

    drawRingOverlay(
        screenContext,
        reinterpret_cast<void*>(tessellatorPtr),
        material,
        camX, camY, camZ,
        reinterpret_cast<void*>(blockSource),
        playerBlockY,
        g_mobSpawnPointMod->renderDistance,
        g_mobSpawnPointMod->transparency,
        g_mobSpawnPointMod->overlayColor);

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

MobSpawnPointModule::MobSpawnPointModule()
    : Module("Mob spawn point",
             "Shows the mob spawn-distance area around the player.") {
    showInMenu = true;
    renderDistance = 6;
    transparency = 0.25f;
    overlayColor = 0xFF55FF55;
    g_mobSpawnPointMod = this;
}

MobSpawnPointModule::~MobSpawnPointModule() {
    if (g_mobSpawnPointMod == this) {
        g_mobSpawnPointMod = nullptr;
    }
}

void MobSpawnPointModule::onInit() {
    uintptr_t renderLevel =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::RenderLevel);
    if (renderLevel) {
        m_patchTarget = reinterpret_cast<void*>(renderLevel);
    }

    uintptr_t tb =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::TessellatorBegin);
    if (tb) {
        m_tessBeginAddr = reinterpret_cast<void*>(tb);
        s_tessBegin = reinterpret_cast<Tessellator_begin_t>(tb);
    }

    uintptr_t tc =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::TessellatorColor);
    if (tc) {
        m_tessColorAddr = reinterpret_cast<void*>(tc);
        s_tessColor = reinterpret_cast<Tessellator_color_t>(tc);
    }

    uintptr_t tv =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::TessellatorVertex);
    if (tv) {
        m_tessVertexAddr = reinterpret_cast<void*>(tv);
        s_tessVertex = reinterpret_cast<Tessellator_vertex_t>(tv);
    }

    uintptr_t rm =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (rm) {
        s_renderMesh = reinterpret_cast<MeshHelpers_renderMeshImmediately_t>(rm);
    } else {
        uintptr_t rmFallback =
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
        if (rmFallback) {
            s_renderMesh =
                reinterpret_cast<MeshHelpers_renderMeshImmediately_t>(rmFallback);
        }
    }

    uintptr_t rmg =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
    if (rmg) {
        m_renderMaterialGroupAddr = reinterpret_cast<void*>(rmg);
        uintptr_t groupAddr =
            resolveADRP(reinterpret_cast<uint32_t*>(rmg), 2, 0);
        if (groupAddr) {
            s_renderMaterialGroup =
                groupAddr +
                bedrocktools::sdk::offsets::MaterialGroup::
                    mRenderMaterialGroupOffset;
        }
    }

    uintptr_t solid =
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock);
    if (solid) {
        s_isSolidBlockingBlock =
            reinterpret_cast<BlockSource_isSolidBlockingBlock_t>(solid);
    }

    bedrocktools::events::bus().subscribe<
        bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { tickCallback(event.player); });
}

void MobSpawnPointModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;

    bedrocktools::hooks::install(
        m_patchTarget,
        reinterpret_cast<void*>(renderLevelHook),
        reinterpret_cast<void**>(&s_renderLevelOrig));

    m_patched = true;
}

void MobSpawnPointModule::onEnable() {
    applyPatch();
}

void MobSpawnPointModule::onDisable() {
    // The hook stays installed, like the other render modules in this project.
}

void MobSpawnPointModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    renderDistance = j.value("renderDistance", renderDistance);
    transparency = j.value("transparency", transparency);

    renderDistance = std::clamp(renderDistance, 1, 128);
    transparency = std::clamp(transparency, 0.0f, 1.0f);

    if (j.contains("overlayColor")) {
        const std::string hex = j["overlayColor"].get<std::string>();
        if (!hex.empty() && hex[0] == '#') {
            try {
                std::string value = hex.substr(1);
                uint32_t parsed = static_cast<uint32_t>(
                    std::stoul(value, nullptr, 16));

                // Accept both #RRGGBB and #AARRGGBB.
                if (value.size() <= 6) {
                    overlayColor = 0xFF000000u | parsed;
                } else {
                    overlayColor = parsed;
                }
            } catch (...) {
                // Keep the previous valid color.
            }
        }
    }
}

void MobSpawnPointModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["renderDistance"] = renderDistance;
    j["transparency"] = transparency;

    char hex[12];
    std::snprintf(hex, sizeof(hex), "#%08X", overlayColor);
    j["overlayColor"] = std::string(hex);
}

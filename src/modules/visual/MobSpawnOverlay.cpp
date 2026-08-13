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
#include <utility>

typedef void (*Tessellator_begin_t)(void* tessellator, void* debugCallback, int primitiveMode, int vertexCount, int noIndices);
typedef void (*Tessellator_color_t)(void* tessellator, float r, float g, float b, float a);
typedef void (*Tessellator_vertex_t)(void* tessellator, float x, float y, float z);
typedef void (*MeshHelpers_renderMeshImmediately_t)(void* screenContext, void* tessellator, void* material, char* pad);

typedef bool (*Actor_isPlayer_t)(void* actor);
typedef bool (*Actor_isInvisible_t)(void* actor);

struct DistanceSortedActor {
    void* mActor;
    float mDistance;
    float _pad;
};

struct ActorVec {
    DistanceSortedActor* begin;
    DistanceSortedActor* end;
    DistanceSortedActor* cap;
};

typedef ActorVec (*Actor_fetchNearbyActorsSorted_t)(void* actor, void* extent, int actorType);

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

static MobSpawnOverlayModule* g_mobSpawnMod = nullptr;

static Tessellator_begin_t                s_tessBegin = nullptr;
static Tessellator_color_t                s_tessColor = nullptr;
static Tessellator_vertex_t               s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;

static Actor_isPlayer_t                   s_actorIsPlayer = nullptr;
static Actor_isInvisible_t                s_actorIsInvisible = nullptr;
static Actor_fetchNearbyActorsSorted_t    s_actorFetchNearby = nullptr;

static MaterialPtr* s_matSelection = nullptr;
static uintptr_t    s_renderMaterialGroup = 0;

static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3);

static bedrocktools::sdk::Vec3 g_playerPos = {0.f, 0.f, 0.f};
static void* g_localPlayerPtr = nullptr;

constexpr int kRingSegments = 32;
constexpr float kWorldBottom = -64.0f;
constexpr float kWorldTop = 320.0f;

static void s_mobSpawnTickCallback(void* _this) {
    if (!g_mobSpawnMod || !g_mobSpawnMod->enabled) return;
    g_localPlayerPtr = _this;
    uintptr_t svc = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::Actor::mStateVectorComponent);
    if (svc != 0) {
        g_playerPos = *(bedrocktools::sdk::Vec3*)svc;
    }
}

static bedrocktools::sdk::Vec3 getActorPos(void* actor) {
    bedrocktools::sdk::Vec3 pos = {0.f, 0.f, 0.f};
    uintptr_t svc = *(uintptr_t*)((uintptr_t)actor + bedrocktools::sdk::offsets::Actor::mStateVectorComponent);
    if (svc != 0) {
        pos = *(bedrocktools::sdk::Vec3*)svc;
    }
    return pos;
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

static void _renderLevel_hook(void* _this, void* screenContext, void* a3) {
    if (_renderLevel_orig) {
        _renderLevel_orig(_this, screenContext, a3);
    }

    if (!g_mobSpawnMod || !g_mobSpawnMod->enabled) return;
    if (!g_localPlayerPtr) return;
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

    void* matInner = s_matSelection ? (void*)s_matSelection
                                    : (void*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);

    uintptr_t colorHolderPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (!colorHolderPtr || colorHolderPtr < 0x1000) return;
    float* colorHolder = (float*)colorHolderPtr;

    float savedColor[4] = { colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3] };
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    auto drawBatchedLines = [&](const std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>>& lines, uint32_t color, float opacityMul) {
        if (lines.empty()) return;
        float r = ((color >> 16) & 0xFF) / 255.0f;
        float g = ((color >>  8) & 0xFF) / 255.0f;
        float b = ((color      ) & 0xFF) / 255.0f;
        float a = (((color >> 24) & 0xFF) / 255.0f) * opacityMul;

        s_tessBegin(tessellator, nullptr, 4, static_cast<int>(lines.size() * 2), 0);
        s_tessColor(tessellator, r, g, b, a);

        for (const auto& line : lines) {
            bedrocktools::sdk::Vec3 p1 = line.first;
            bedrocktools::sdk::Vec3 p2 = line.second;
            p1.x -= camX; p1.y -= camY; p1.z -= camZ;
            p2.x -= camX; p2.y -= camY; p2.z -= camZ;
            s_tessVertex(tessellator, p1.x, p1.y, p1.z);
            s_tessVertex(tessellator, p2.x, p2.y, p2.z);
        }

        char pad[0x58];
        memset(pad, 0, sizeof(pad));
        s_renderMesh(screenContext, tessellator, matInner, pad);
    };

    // Builds a horizontal ring outline (kRingSegments-gon approximating a circle) at height y.
    auto buildRing = [&](const bedrocktools::sdk::Vec3& center, float radius, float y,
                          std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>>& out) {
        bedrocktools::sdk::Vec3 prev{
            center.x + radius,
            y,
            center.z
        };
        for (int i = 1; i <= kRingSegments; ++i) {
            float theta = (2.0f * 3.14159265f * static_cast<float>(i)) / static_cast<float>(kRingSegments);
            bedrocktools::sdk::Vec3 cur{
                center.x + radius * std::cos(theta),
                y,
                center.z + radius * std::sin(theta)
            };
            out.push_back({prev, cur});
            prev = cur;
        }
    };

    // Builds vertical pillars sampled around the ring circumference, from bottom to top of the world.
    auto buildPillars = [&](const bedrocktools::sdk::Vec3& center, float radius,
                             std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>>& out) {
        constexpr int pillarCount = 8;
        for (int i = 0; i < pillarCount; ++i) {
            float theta = (2.0f * 3.14159265f * static_cast<float>(i)) / static_cast<float>(pillarCount);
            float px = center.x + radius * std::cos(theta);
            float pz = center.z + radius * std::sin(theta);
            out.push_back({
                bedrocktools::sdk::Vec3{px, kWorldBottom, pz},
                bedrocktools::sdk::Vec3{px, kWorldTop, pz}
            });
        }
    };

    auto renderAround = [&](const bedrocktools::sdk::Vec3& center) {
        std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> innerLines;
        std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> outerLines;

        const float innerR = g_mobSpawnMod->innerRadius;
        const float outerR = static_cast<float>(g_mobSpawnMod->renderDistanceChunks) * 16.0f;

        if (g_mobSpawnMod->showHorizontal) {
            if (g_mobSpawnMod->showInnerRing && innerR > 0.0f) buildRing(center, innerR, center.y, innerLines);
            if (outerR > 0.0f) buildRing(center, outerR, center.y, outerLines);
        }

        if (g_mobSpawnMod->showVertical) {
            if (g_mobSpawnMod->showInnerRing && innerR > 0.0f) buildPillars(center, innerR, innerLines);
            if (outerR > 0.0f) buildPillars(center, outerR, outerLines);
        }

        drawBatchedLines(innerLines, g_mobSpawnMod->innerColor, g_mobSpawnMod->opacity);
        drawBatchedLines(outerLines, g_mobSpawnMod->outerColor, g_mobSpawnMod->opacity);
    };

    renderAround(g_playerPos);

    if (g_mobSpawnMod->showOtherPlayers && s_actorFetchNearby) {
        const float outerR = static_cast<float>(g_mobSpawnMod->renderDistanceChunks) * 16.0f;
        bedrocktools::sdk::Vec3 extent = {outerR, 64.0f, outerR};
        ActorVec actors = s_actorFetchNearby(g_localPlayerPtr, &extent, 1);

        if (actors.begin && actors.end) {
            for (DistanceSortedActor* it = actors.begin; it < actors.end; ++it) {
                void* ent = it->mActor;
                if (!ent || ent == g_localPlayerPtr) continue;
                if (!s_actorIsPlayer || !s_actorIsPlayer(ent)) continue;
                if (s_actorIsInvisible && s_actorIsInvisible(ent)) continue;

                renderAround(getActorPos(ent));
            }
        }
    }

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

MobSpawnOverlayModule::MobSpawnOverlayModule()
    : Module("Mob Spawn Overlay", "Shows where hostile mobs can spawn around players.") {

    showInMenu = true;

    showHorizontal = true;
    showVertical = false;
    showInnerRing = true;
    showOtherPlayers = false;

    innerRadius = 24.0f;        // vanilla: mobs won't spawn within 24 blocks of any player
    renderDistanceChunks = 8;   // outer boundary in chunks (x16 blocks)

    innerColor = 0xFFFF3B30;    // red-ish: no-spawn zone
    outerColor = 0xFF34C759;    // green-ish: outer spawn boundary
    opacity = 0.6f;

    m_patched = false;
    m_patchTarget = nullptr;
    m_tessBeginAddr = nullptr;
    m_tessColorAddr = nullptr;
    m_tessVertexAddr = nullptr;
    m_renderMaterialGroupAddr = nullptr;
    g_mobSpawnMod = this;
}

MobSpawnOverlayModule::~MobSpawnOverlayModule() {
    if (g_mobSpawnMod == this) g_mobSpawnMod = nullptr;
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

    uintptr_t aip = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer);
    if (aip) s_actorIsPlayer = (Actor_isPlayer_t)aip;

    uintptr_t aii = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsInvisible);
    if (aii) s_actorIsInvisible = (Actor_isInvisible_t)aii;

    uintptr_t afn = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted);
    if (afn) s_actorFetchNearby = (Actor_fetchNearbyActorsSorted_t)afn;

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) { s_mobSpawnTickCallback(event.player); });
}

void MobSpawnOverlayModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
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
    showHorizontal = j.value("showHorizontal", showHorizontal);
    showVertical = j.value("showVertical", showVertical);
    showInnerRing = j.value("showInnerRing", showInnerRing);
    showOtherPlayers = j.value("showOtherPlayers", showOtherPlayers);
    innerRadius = j.value("innerRadius", innerRadius);
    renderDistanceChunks = j.value("renderDistanceChunks", renderDistanceChunks);
    opacity = j.value("opacity", opacity);

    auto parseColor = [&](const std::string& key, uint32_t& outColor) {
        if (j.contains(key)) {
            std::string hexStr = j[key].get<std::string>();
            if (!hexStr.empty() && hexStr[0] == '#') {
                try { outColor = std::stoul(hexStr.substr(1), nullptr, 16); } catch (...) {}
            }
        }
    };

    parseColor("innerColor", innerColor);
    parseColor("outerColor", outerColor);
}

void MobSpawnOverlayModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["showHorizontal"] = showHorizontal;
    j["showVertical"] = showVertical;
    j["showInnerRing"] = showInnerRing;
    j["showOtherPlayers"] = showOtherPlayers;
    j["innerRadius"] = innerRadius;
    j["renderDistanceChunks"] = renderDistanceChunks;
    j["opacity"] = opacity;

    char hexI[12], hexO[12];
    snprintf(hexI, sizeof(hexI), "#%08X", innerColor);
    snprintf(hexO, sizeof(hexO), "#%08X", outerColor);

    j["innerColor"] = std::string(hexI);
    j["outerColor"] = std::string(hexO);
}
#include "spawnsphere.hpp"
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

static SpawnSphereModule* g_spawnSphereMod = nullptr;

static Tessellator_begin_t                s_tessBegin = nullptr;
static Tessellator_color_t                s_tessColor = nullptr;
static Tessellator_vertex_t               s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;

static MaterialPtr* s_matSelection = nullptr;
static uintptr_t    s_renderMaterialGroup = 0;

static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3);

static bedrocktools::sdk::Vec3 g_playerPos = {0.f, 0.f, 0.f};

constexpr int   kRingSegments = 32;   // smooth-mode polygon resolution per ring
constexpr float kPi = 3.14159265f;

using LineList = std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>>;

static void s_spawnSphereTickCallback(void* _this) {
    if (!g_spawnSphereMod || !g_spawnSphereMod->enabled) return;
    uintptr_t svc = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::Actor::mStateVectorComponent);
    if (svc != 0) {
        g_playerPos = *(bedrocktools::sdk::Vec3*)svc;
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

// Smooth circular ring (kRingSegments-gon) at height y, radius r, centered on `center`.
static void buildSmoothRing(const bedrocktools::sdk::Vec3& center, float r, float y, LineList& out) {
    if (r <= 0.01f) return;
    bedrocktools::sdk::Vec3 prev{center.x + r, y, center.z};
    for (int i = 1; i <= kRingSegments; ++i) {
        float theta = (2.0f * kPi * static_cast<float>(i)) / static_cast<float>(kRingSegments);
        bedrocktools::sdk::Vec3 cur{
            center.x + r * std::cos(theta),
            y,
            center.z + r * std::sin(theta)
        };
        out.push_back({prev, cur});
        prev = cur;
    }
}

// Stepped ring that traces the outer edge of every block column whose center falls
// within radius r (a "voxelized" circle, like the world border's blocky look).
static void buildBlockyRing(const bedrocktools::sdk::Vec3& center, float r, float y, LineList& out) {
    if (r < 1.0f) return;
    int radius = static_cast<int>(std::floor(r));
    if (radius < 1) return;

    auto zTop = [&](int x) -> int {
        float val = r * r - static_cast<float>(x) * static_cast<float>(x);
        return val > 0.f ? static_cast<int>(std::floor(std::sqrt(val))) : 0;
    };

    // Top half: staircase from x=-radius to x=+radius, mirrored for the bottom half (z < 0).
    for (int sign = 1; sign >= -1; sign -= 2) {
        for (int x = -radius; x <= radius; ++x) {
            int z = zTop(x);
            bedrocktools::sdk::Vec3 a{center.x + x,     y, center.z + sign * static_cast<float>(z)};
            bedrocktools::sdk::Vec3 b{center.x + x + 1, y, center.z + sign * static_cast<float>(z)};
            out.push_back({a, b});
        }
        for (int x = -radius; x < radius; ++x) {
            int z1 = zTop(x);
            int z2 = zTop(x + 1);
            bedrocktools::sdk::Vec3 a{center.x + x + 1, y, center.z + sign * static_cast<float>(z1)};
            bedrocktools::sdk::Vec3 b{center.x + x + 1, y, center.z + sign * static_cast<float>(z2)};
            out.push_back({a, b});
        }
    }
}

static void buildRing(const bedrocktools::sdk::Vec3& center, float r, float y, bool blocky, LineList& out) {
    if (blocky) buildBlockyRing(center, r, y, out);
    else        buildSmoothRing(center, r, y, out);
}

// Stacks latitude rings from -R to +R, shrinking radius with |dy| per sphere.jw
// equation (r(dy) = sqrt(R^2 - dy^2)) -- this mirrors the actual 3D distance
// check the game uses, not a cylinder. Also draws meridian lines connecting
// the rings so the whole thing reads as a polygonal globe rather than a stack
// of disconnected circles.
static void buildSphere(const bedrocktools::sdk::Vec3& center, float R, float spacing,
                         int meridianCount, bool blocky, LineList& out) {
    if (R <= 0.01f) return;
    if (spacing < 0.5f) spacing = 0.5f;

    std::vector<float> dys;
    dys.push_back(0.0f);
    for (float dy = spacing; dy < R; dy += spacing) {
        dys.push_back(dy);
        dys.push_back(-dy);
    }
    dys.push_back(R);
    dys.push_back(-R);

    for (float dy : dys) {
        float rem = R * R - dy * dy;
        float ringR = rem > 0.f ? std::sqrt(rem) : 0.f;
        buildRing(center, ringR, center.y + dy, blocky, out);
    }

    if (meridianCount > 0) {
        constexpr int kMeridianSamples = 16; // vertical resolution per meridian arc
        for (int m = 0; m < meridianCount; ++m) {
            float theta = (2.0f * kPi * static_cast<float>(m)) / static_cast<float>(meridianCount);
            float cosT = std::cos(theta);
            float sinT = std::sin(theta);

            bedrocktools::sdk::Vec3 prev{center.x + R * cosT, center.y - R, center.z + R * sinT};
            for (int i = 1; i <= kMeridianSamples; ++i) {
                float dy = -R + (2.0f * R * static_cast<float>(i)) / static_cast<float>(kMeridianSamples);
                float rem = R * R - dy * dy;
                float ringR = rem > 0.f ? std::sqrt(rem) : 0.f;
                bedrocktools::sdk::Vec3 cur{center.x + ringR * cosT, center.y + dy, center.z + ringR * sinT};
                out.push_back({prev, cur});
                prev = cur;
            }
        }
    }
}

static void _renderLevel_hook(void* _this, void* screenContext, void* a3) {
    if (_renderLevel_orig) {
        _renderLevel_orig(_this, screenContext, a3);
    }

    if (!g_spawnSphereMod || !g_spawnSphereMod->enabled) return;
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

    auto drawBatchedLines = [&](const LineList& lines, uint32_t color, float opacityMul) {
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

    const float innerR = g_spawnSphereMod->innerRadius;
    const float outerR = static_cast<float>(g_spawnSphereMod->renderDistanceChunks) * 16.0f;
    const float spacing = g_spawnSphereMod->bandSpacing;
    const int meridians = g_spawnSphereMod->meridianCount;
    const bool blocky = g_spawnSphereMod->blockyEdges;

    LineList innerLines;
    LineList outerLines;

    buildSphere(g_playerPos, innerR, spacing, meridians, blocky, innerLines);
    buildSphere(g_playerPos, outerR, spacing, meridians, blocky, outerLines);

    drawBatchedLines(innerLines, g_spawnSphereMod->innerColor, g_spawnSphereMod->opacity);
    drawBatchedLines(outerLines, g_spawnSphereMod->outerColor, g_spawnSphereMod->opacity);

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

SpawnSphereModule::SpawnSphereModule()
    : Module("Spawn Sphere", "Shows the 3D spawn-distance sphere around the player.") {

    showInMenu = true;

    innerRadius = 24.0f;        // vanilla: mobs won't spawn within 24 blocks (3D) of any player
    renderDistanceChunks = 8;   // outer boundary in chunks (x16 blocks)
    bandSpacing = 4.0f;         // blocks between latitude rings
    meridianCount = 8;          // vertical polygon lines around the sphere
    blockyEdges = false;        // smooth polygon circle by default

    innerColor = 0xFFFF3B30;    // red-ish: no-spawn sphere
    outerColor = 0xFF34C759;    // green-ish: outer spawn boundary sphere
    opacity = 0.6f;

    m_patched = false;
    m_patchTarget = nullptr;
    m_tessBeginAddr = nullptr;
    m_tessColorAddr = nullptr;
    m_tessVertexAddr = nullptr;
    m_renderMaterialGroupAddr = nullptr;
    g_spawnSphereMod = this;
}

SpawnSphereModule::~SpawnSphereModule() {
    if (g_spawnSphereMod == this) g_spawnSphereMod = nullptr;
}

void SpawnSphereModule::onInit() {
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

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) { s_spawnSphereTickCallback(event.player); });
}

void SpawnSphereModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    bedrocktools::hooks::install(m_patchTarget, (void*)_renderLevel_hook, (void**)&_renderLevel_orig);
    m_patched = true;
}

void SpawnSphereModule::onEnable() {
    applyPatch();
}

void SpawnSphereModule::onDisable() {
}

void SpawnSphereModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    innerRadius = j.value("innerRadius", innerRadius);
    renderDistanceChunks = j.value("renderDistanceChunks", renderDistanceChunks);
    bandSpacing = j.value("bandSpacing", bandSpacing);
    meridianCount = j.value("meridianCount", meridianCount);
    blockyEdges = j.value("blockyEdges", blockyEdges);
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

void SpawnSphereModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["innerRadius"] = innerRadius;
    j["renderDistanceChunks"] = renderDistanceChunks;
    j["bandSpacing"] = bandSpacing;
    j["meridianCount"] = meridianCount;
    j["blockyEdges"] = blockyEdges;
    j["opacity"] = opacity;

    char hexI[12], hexO[12];
    snprintf(hexI, sizeof(hexI), "#%08X", innerColor);
    snprintf(hexO, sizeof(hexO), "#%08X", outerColor);

    j["innerColor"] = std::string(hexI);
    j["outerColor"] = std::string(hexO);
}

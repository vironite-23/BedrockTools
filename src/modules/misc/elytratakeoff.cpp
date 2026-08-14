#include "elytratakeoff.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <entt/entt.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>

// --- Same MoveInputComponent layout keystrokes.cpp reads jump/space from ---
// (src/modules/hud/keystrokes.cpp). Duplicated here rather than shared via a
// header because it isn't exposed anywhere public yet; kept byte-for-byte
// identical so we are writing to the exact same field that module reads.
namespace {

using uint = uint32_t;
using ushort = uint16_t;
using uchar = unsigned char;

enum class EntityId : uint32_t {};

template <size_t N, typename T>
struct bitset {
    T value;
    void set(size_t index, bool v) {
        if (v) value |= (1ULL << index);
        else value &= ~(1ULL << index);
    }
    bool test(size_t index) const {
        return (value & (1ULL << index)) != 0;
    }
};

struct EntityIdTraits {
    using value_type = EntityId;
    using entity_type = uint32_t;
    using version_type = uint16_t;
    static constexpr uint32_t entity_mask = 0x3FFFF;
    static constexpr uint32_t version_mask = 0x3FFF;
};

}

template<>
struct entt::entt_traits<EntityId> : entt::basic_entt_traits<EntityIdTraits> {
    static constexpr std::size_t page_size = ENTT_SPARSE_PAGE;
};

namespace {

struct MoveInputState {
    bitset<27, uint> mFlagValues;
    bedrocktools::sdk::Vec2 mAnalogMoveVector;
    uchar mLookSlightDirField;
    uchar mLookNormalDirField;
    uchar mLookSmoothDirField;
    uchar pad[1];
};

struct MoveInputComponent {
    MoveInputState mInputState;
    MoveInputState mRawInputState;
    uchar mHoldAutoJumpInWaterTicks;
    uchar pad[3];
    bedrocktools::sdk::Vec2 mMove;
    bedrocktools::sdk::Vec2 mLookDelta;
    bedrocktools::sdk::Vec2 mInteractDir;
    bedrocktools::sdk::Vec3 mDisplacement;
    bedrocktools::sdk::Vec3 mDisplacementDelta;
    bedrocktools::sdk::Vec3 mCameraOrientation;
    bitset<11, ushort> mFlagValues;
    std::array<bool, 2> mIsPaddling;
};

class EntityRegistry;

class EntityContext {
public:
    inline entt::basic_registry<EntityId>& getRegistry() { return mEnTTRegistry; }

    template <class T>
    inline T* tryGetComponent() {
        return getRegistry().try_get<T>(mEntity);
    }

    EntityRegistry& mRegistry;
    entt::basic_registry<EntityId>& mEnTTRegistry;
    EntityId const mEntity;
};

// Jump/space is bit index 7 in mRawInputState.mFlagValues - confirmed by
// KeystrokesModule, which reads this exact bit to display whether jump is
// currently held.
constexpr size_t kJumpBit = 7;

MoveInputComponent* getMoveInput(void* player) {
    if (!player) return nullptr;
    auto* ctx = reinterpret_cast<EntityContext*>(reinterpret_cast<char*>(player) + bedrocktools::sdk::offsets::Actor::mEntityContext);
    if (!ctx) return nullptr;
    return ctx->tryGetComponent<MoveInputComponent>();
}

void setJumpHeld(void* player, bool held) {
    auto* moveInput = getMoveInput(player);
    if (!moveInput) return;
    moveInput->mRawInputState.mFlagValues.set(kJumpBit, held);
}

} // namespace

ElytraTakeoffModule::ElytraTakeoffModule()
    : Module("Elytra Takeoff", "Button that simulates the jump double-tap + hold needed to take off with an elytra.") {
    showInMenu = true;
}

ElytraTakeoffModule::~ElytraTakeoffModule() {
}

void ElytraTakeoffModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([this](auto& event) {
        m_lastPlayer = event.player;
        advance(event.player);
    });
}

void ElytraTakeoffModule::onEnable() {
}

void ElytraTakeoffModule::onDisable() {
    // Don't leave the jump input forced down if the module gets disabled
    // mid-sequence.
    cancel();
}

bool ElytraTakeoffModule::hasElytraAndFireworkEquipped() {
    // See the header comment: no verified way to read the local player's
    // armor/hotbar item stacks exists in this SDK snapshot yet. Always
    // "pass" the check until this is implemented against real offsets;
    // set requireEquipmentCheck = false to make that explicit instead of
    // silently relying on this stub.
    return true;
}

void ElytraTakeoffModule::startTakeoff() {
    if (!enabled) return;
    if (m_state != State::Idle) return; // already running, ignore re-press

    if (requireEquipmentCheck && !hasElytraAndFireworkEquipped()) {
        return;
    }

    m_state = State::FirstPressDown;
    m_stateTicks = 0;
}

void ElytraTakeoffModule::cancel() {
    if (m_state != State::Idle && m_lastPlayer) {
        setJumpHeld(m_lastPlayer, false);
    }
    m_state = State::Idle;
    m_stateTicks = 0;
}

void ElytraTakeoffModule::advance(void* player) {
    if (m_state == State::Idle) return;
    if (!enabled || !player) {
        cancel();
        return;
    }

    ++m_stateTicks;

    switch (m_state) {
        case State::Idle:
            break;

        case State::FirstPressDown:
            setJumpHeld(player, true);
            if (m_stateTicks >= std::max(pressTicks, 1)) {
                m_state = State::FirstPressUp;
                m_stateTicks = 0;
            }
            break;

        case State::FirstPressUp:
            setJumpHeld(player, false);
            if (m_stateTicks >= 1) {
                m_state = State::WaitingToFall;
                m_stateTicks = 0;
            }
            break;

        case State::WaitingToFall:
            // jump stays released while the player starts falling
            if (m_stateTicks >= std::max(fallWaitTicks, 0)) {
                m_state = State::SecondPressDown;
                m_stateTicks = 0;
            }
            break;

        case State::SecondPressDown:
            // this is the press that actually deploys the glider
            setJumpHeld(player, true);
            if (m_stateTicks >= std::max(pressTicks, 1)) {
                m_state = State::Holding;
                m_stateTicks = 0;
            }
            break;

        case State::Holding:
            // "hold the screen" - keep jump held to pitch the glide up
            setJumpHeld(player, true);
            if (m_stateTicks >= std::max(holdTicks, 0)) {
                setJumpHeld(player, false);
                m_state = State::Idle;
                m_stateTicks = 0;
            }
            break;
    }
}

void ElytraTakeoffModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    requireEquipmentCheck = j.value("requireEquipmentCheck", requireEquipmentCheck);
    pressTicks = j.value("pressTicks", pressTicks);
    fallWaitTicks = j.value("fallWaitTicks", fallWaitTicks);
    holdTicks = j.value("holdTicks", holdTicks);

    if (j.contains("takeoffButton") && j["takeoffButton"].is_boolean()) {
        if (j["takeoffButton"].get<bool>()) {
            startTakeoff();
        }
    }
}

void ElytraTakeoffModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["requireEquipmentCheck"] = requireEquipmentCheck;
    j["pressTicks"] = pressTicks;
    j["fallWaitTicks"] = fallWaitTicks;
    j["holdTicks"] = holdTicks;

    // Momentary button - never persisted as pressed.
    j["takeoffButton"] = false;
}

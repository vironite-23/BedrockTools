#include "frontviewcontrols.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include "core/memory/Hooks.hpp"
#include <entt/entt.hpp>

#include <array>
#include <cstdint>
#include <atomic>

namespace {

enum class EntityId : std::uint32_t {};

template <size_t N, typename T>
struct bitset {
    T value;
};

struct EntityIdTraits {
    using value_type = EntityId;
    using entity_type = std::uint32_t;
    using version_type = std::uint16_t;
    static constexpr std::uint32_t entity_mask = 0x3FFFF;
    static constexpr std::uint32_t version_mask = 0x3FFF;
};

namespace entt {
template<>
struct entt_traits<::EntityId> : basic_entt_traits<::EntityIdTraits> {
    static constexpr std::size_t page_size = ENTT_SPARSE_PAGE;
};
} // namespace entt

struct MoveInputState {
    bitset<27, std::uint32_t> mFlagValues;
    bedrocktools::sdk::Vec2 mAnalogMoveVector;
    std::uint8_t mLookSlightDirField;
    std::uint8_t mLookNormalDirField;
    std::uint8_t mLookSmoothDirField;
    std::uint8_t pad[1];
};

struct MoveInputComponent {
    MoveInputState mInputState;
    MoveInputState mRawInputState;
    std::uint8_t mHoldAutoJumpInWaterTicks;
    std::uint8_t pad[3];
    bedrocktools::sdk::Vec2 mMove;
    bedrocktools::sdk::Vec2 mLookDelta;
    bedrocktools::sdk::Vec2 mInteractDir;
    bedrocktools::sdk::Vec3 mDisplacement;
    bedrocktools::sdk::Vec3 mDisplacementDelta;
    bedrocktools::sdk::Vec3 mCameraOrientation;
    bitset<11, std::uint16_t> mFlagValues;
    std::array<bool, 2> mIsPaddling;
};

class EntityContext {
public:
    inline entt::basic_registry<EntityId>& getRegistry() { return mEnTTRegistry; }

    template <class T>
    inline T* tryGetComponent() {
        return getRegistry().try_get<T>(mEntity);
    }

    void* mRegistry;
    entt::basic_registry<EntityId>& mEnTTRegistry;
    EntityId const mEntity;
};

FrontViewControlsModule* g_module = nullptr;
std::atomic_bool g_frontPerspective{false};

int (*g_getPerspectiveOriginal)(void*) = nullptr;

int getPerspectiveHook(void* self) {
    const int result = g_getPerspectiveOriginal ? g_getPerspectiveOriginal(self) : 0;
    // Bedrock perspective values: 0 = first person, 1 = third person back,
    // 2 = third person front.
    g_frontPerspective.store(result == 2, std::memory_order_relaxed);
    return result;
}

void onLocalPlayerTick(void* player) {
    if (!g_module || !g_module->enabled ||
        !g_frontPerspective.load(std::memory_order_relaxed) || !player) {
        return;
    }

    auto* context = reinterpret_cast<EntityContext*>(
        reinterpret_cast<std::uintptr_t>(player) +
        bedrocktools::sdk::offsets::Actor::mEntityContext
    );
    if (!context) return;

    auto* moveInput = context->tryGetComponent<MoveInputComponent>();
    if (!moveInput) return;

    // In front-facing view, the player's horizontal and depth directions are
    // visually mirrored. Negating both axes makes the touch controls behave
    // naturally from the camera's point of view.
    moveInput->mMove.x = -moveInput->mMove.x;
    moveInput->mMove.y = -moveInput->mMove.y;
    moveInput->mInputState.mAnalogMoveVector.x = -moveInput->mInputState.mAnalogMoveVector.x;
    moveInput->mInputState.mAnalogMoveVector.y = -moveInput->mInputState.mAnalogMoveVector.y;
    moveInput->mRawInputState.mAnalogMoveVector.x = -moveInput->mRawInputState.mAnalogMoveVector.x;
    moveInput->mRawInputState.mAnalogMoveVector.y = -moveInput->mRawInputState.mAnalogMoveVector.y;
}

} // namespace

FrontViewControlsModule::FrontViewControlsModule()
    : Module("Front View Controls", "Reverses movement controls while using the front-facing camera view.") {
    g_module = this;
}

FrontViewControlsModule::~FrontViewControlsModule() {
    if (g_module == this) g_module = nullptr;
}

void FrontViewControlsModule::onInit() {
    const auto perspectiveAddress = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::GetPerspective
    );

    if (perspectiveAddress != 0 && !m_perspectiveHooked) {
        bedrocktools::hooks::install(
            reinterpret_cast<void*>(perspectiveAddress),
            reinterpret_cast<void*>(getPerspectiveHook),
            reinterpret_cast<void**>(&g_getPerspectiveOriginal)
        );
        m_perspectiveHooked = true;
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { onLocalPlayerTick(event.player); }
    );
}

void FrontViewControlsModule::onDisable() {
    g_frontPerspective.store(false, std::memory_order_relaxed);
}

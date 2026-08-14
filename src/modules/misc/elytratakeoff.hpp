#pragma once

#include "../Module.hpp"

// ElytraTakeoffModule
// ----------------------------------------------------------------------------
// Adds a menu Button ("takeoffButton") that, when pressed, simulates the
// jump input sequence normally needed to take off with an elytra:
//   1. a short jump press+release (gets the player airborne),
//   2. a short pause while the player starts falling,
//   3. a second jump press ("double-tap") which is what actually deploys
//      the glider while the player is in the air,
//   4. holding jump afterward for a bit (the "hold the screen" ascend
//      input players normally keep pressed right after opening the
//      glider, so the player pulls its nose up instead of immediately
//      diving into the ground).
//
// The jump input is simulated by writing directly to the same
// MoveInputComponent bit (index 7, mRawInputState.mFlagValues) that
// KeystrokesModule already reads to show whether jump/space is held -
// see src/modules/hud/keystrokes.cpp. Re-using that exact, already-verified
// bit avoids guessing new offsets for this.
//
// NOTE ON THE EQUIPMENT CHECK:
// This module is only meant to fire the takeoff sequence when the player
// actually has an elytra in the chest armor slot and is holding a firework
// rocket. However, the SDK snapshot this module was written against does
// not currently expose a way to read the *local player's own* armor/hotbar
// item stacks (the only ItemStack reading available, in shulkerpreview.cpp,
// is scoped to an open shulker-box container UI, not the player's persistent
// inventory/armor slots). Implementing hasElytraAndFireworkEquipped() for
// real requires the layout of the player's inventory/armor container
// component (offsets this project would normally get via signature
// scanning/reverse engineering, the same way MoveInputComponent's layout
// was obtained). Rather than guess those offsets - which, for memory reads,
// risks silently reading garbage and misbehaving instead of just failing to
// compile - hasElytraAndFireworkEquipped() is left as a clearly marked stub
// that currently always returns true, and requireEquipmentCheck can be used
// to skip calling it entirely. Fill it in once you have the real offsets.
class ElytraTakeoffModule : public Module {
public:
    ElytraTakeoffModule();
    ~ElytraTakeoffModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Menu button. Auto-detected as a pressable Button (not a persistent
    // Toggle) by ModuleMenu.cpp because its key name contains "button" -
    // same convention BreadcrumbsModule::clearTrailButton already uses.
    // Always written back out as false in saveConfig() so it never stays
    // "stuck on" in the saved config; a press is a momentary event.
    bool takeoffButton = false;

    // Skips hasElytraAndFireworkEquipped() when false, always attempting
    // the takeoff sequence. See the equipment-check note above.
    bool requireEquipmentCheck = true;

    // Timing, in game ticks (20 ticks/sec).
    int pressTicks = 3;     // how long each simulated jump press is held
    int fallWaitTicks = 6;  // gap between the two jumps, to start falling
    int holdTicks = 20;     // how long jump is held after the 2nd press

private:
    enum class State {
        Idle,
        FirstPressDown,
        FirstPressUp,
        WaitingToFall,
        SecondPressDown,
        Holding
    };

    bool hasElytraAndFireworkEquipped();
    void startTakeoff();
    void cancel();
    void advance(void* player);

    State m_state = State::Idle;
    int m_stateTicks = 0;
    void* m_lastPlayer = nullptr;
};

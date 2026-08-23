#pragma once

#include "../Module.hpp"

class FrontViewControlsModule : public Module {
public:
    FrontViewControlsModule();
    ~FrontViewControlsModule() override;

    void onInit() override;
    void onDisable() override;

private:
    bool m_perspectiveHooked = false;
};

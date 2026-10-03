#pragma once
// Portable frontend input boundary. No controller/time/gameplay implementation.
struct VitaInputSample {
    bool menuAcceptPressed=false,menuBackPressed=false,menuDownPressed=false,menuUpPressed=false;
    bool menuLeftPressed=false,menuRightPressed=false,startPressed=false,performanceTogglePressed=false;
};
struct VitaInputConfig {
    float lookSensitivity=1;
    bool invertY=false,rearTouchEnabled=true,dpadFallbackProfile=false;
};

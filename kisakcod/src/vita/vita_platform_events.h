#pragma once
#include <cstdint>

struct VitaPlatformEventSample {
    bool suspend = false;
    bool resume = false;
    bool available = false;
};

// Created and polled on the source/main thread. Callbacks record flags only;
// resource transactions happen at the application frame boundary.
class VitaPlatformEvents {
public:
    ~VitaPlatformEvents();
    bool Initialize();
    VitaPlatformEventSample Poll();
private:
    static int PowerCallback(int id, int count, int flags, void* context);
    int callback_ = -1;
    uint32_t pending_ = 0;
    bool overlay_ = false;
};

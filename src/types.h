#pragma once

enum CandleMode {
    CANDLE_MODE,
    COLOR_MODE,
    MAGIC_MODE,
    AUTO_MODE,
    NUM_MODES
};

struct ModeConfig {
    const char* name;
    void (*updateFunction)();
    void (*enterFunction)();
    void (*exitFunction)();
};

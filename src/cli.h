#pragma once
#include <Arduino.h>
#include <FastLED.h>
#include "config.h"
#include "types.h"

// Called once from setup() - prints the normal menu
void cliBegin();

// Called every loop() iteration - reads serial input and dispatches commands
void cliUpdate();

// Returns true while a blocking test is running (loop() should skip normal mode updates)
bool cliTestActive();

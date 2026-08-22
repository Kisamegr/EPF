#ifndef EPF_OLED_STATUS_H
#define EPF_OLED_STATUS_H

#include "hardware_profile.h"

#if EPF_USE_OLED
#include <Arduino.h>
#include <Adafruit_SSD1306.h>

class OledStatus
{
public:
    OledStatus();
    bool begin();
    void show(const String &line1, const String &line2 = String());

private:
    Adafruit_SSD1306 display;
};

#endif

#endif

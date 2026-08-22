#include "oled_status.h"

#include "config.h"
#if EPF_USE_OLED
#include <Wire.h>

OledStatus::OledStatus()
    : display(OLED_SCREEN_WIDTH, OLED_SCREEN_HEIGHT, &Wire, OLED_RESET_PIN)
{
}

bool OledStatus::begin()
{
    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
    return display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS);
}

void OledStatus::show(const String &line1, const String &line2)
{
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setTextWrap(false);
    display.setCursor(0, 0);
    display.println(line1.substring(0, OLED_MAX_CHARS));
    if (line2.length() > 0)
    {
        display.println(line2.substring(0, OLED_MAX_CHARS));
    }
    display.display();
}
#endif

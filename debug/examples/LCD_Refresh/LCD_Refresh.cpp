/*
 * @Description: None
 * @version: V1.0.0
 * @Author: None
 * @Date: 2024-01-10 17:43:15
 * @LastEditors: LILYGO_L
 * @LastEditTime: 2024-01-17 10:49:44
 * @License: GPL 3.0
 */
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "pin_config.h"
#include "Material_16Bit.h"

static uint32_t LCD_Color[] = {RED, GREEN, BLUE, WHITE};
static uint8_t LCD_Color_Count = 0;

static int LCD_Backlight_Count = 0;
static bool LCD_Backlight_Flag = 0;
static bool LCD_Backlight_Flag1 = 0;

static size_t CycleTime = 0;
static size_t CycleTime1 = 0;
static size_t CycleTime2 = 0;
static size_t CycleTime3 = 0;

Arduino_DataBus *bus = new Arduino_XL9535SWSPI(IIC_SDA /* SDA */, IIC_SCL /* SCL */, -1 /* XL PWD */,
                                               XL95X5_CS /* XL CS */, XL95X5_SCLK /* XL SCK */, XL95X5_MOSI /* XL MOSI */);
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    -1 /* DE */, LCD_VSYNC /* VSYNC */, LCD_HSYNC /* HSYNC */, LCD_PCLK /* PCLK */,
    LCD_B0 /* B0 */, LCD_B1 /* B1 */, LCD_B2 /* B2 */, LCD_B3 /* B3 */, LCD_B4 /* B4 */,
    LCD_G0 /* G0 */, LCD_G1 /* G1 */, LCD_G2 /* G2 */, LCD_G3 /* G3 */, LCD_G4 /* G4 */, LCD_G5 /* G5 */,
    LCD_R0 /* R0 */, LCD_R1 /* R1 */, LCD_R2 /* R2 */, LCD_R3 /* R3 */, LCD_R4 /* R4 */,
    1 /* hsync_polarity */, 20 /* hsync_front_porch */, 2 /* hsync_pulse_width */, 0 /* hsync_back_porch */,
    1 /* vsync_polarity */, 30 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 1 /* vsync_back_porch */,
    10 /* pclk_active_neg */, 6000000L /* prefer_speed */, false /* useBigEndian */,
    0 /* de_idle_high*/, 0 /* pclk_idle_high */);
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    LCD_WIDTH /* width */, LCD_HEIGHT /* height */, rgbpanel, 0 /* rotation */, true /* auto_flush */,
    bus, -1 /* RST */, st7701_type9_init_operations, sizeof(st7701_type9_init_operations));

void setup()
{
    Serial.begin(115200);
    Serial.println("Ciallo");

    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);

    ledcAttachPin(LCD_BL, 1);
    ledcSetup(1, 1000, 8);
    ledcWrite(1, 255);

    Wire.begin(IIC_SDA, IIC_SCL);

    gfx->begin();
    gfx->fillScreen(WHITE);

    gfx->fillScreen(WHITE);
    gfx->setCursor(100, 200);
    gfx->setTextSize(3);
    gfx->setTextColor(PURPLE);
    gfx->printf("Backlight_Refresh");

    gfx->drawRect(0, 0, 480, 480, RED);
}

void loop()
{
    if (millis() > CycleTime)
    {
        gfx->fillScreen(LCD_Color[LCD_Color_Count]);
        LCD_Color_Count++;
        if (LCD_Color_Count > 3)
        {
            LCD_Color_Count = 0;
        }
        CycleTime = millis() + 100; // 100ms
    }

    if (millis() > CycleTime1)
    {
        ledcWrite(1, LCD_Backlight_Count);

        if (LCD_Backlight_Flag == 0)
        {
            LCD_Backlight_Count++;
        }
        else
        {
            LCD_Backlight_Count--;
        }

        if (LCD_Backlight_Count >= 255)
        {
            LCD_Backlight_Flag = 1;
        }
        if (LCD_Backlight_Count <= 0)
        {
            LCD_Backlight_Flag = 0;
        }

        CycleTime1 = millis() + 1; // 1ms
    }

    if (millis() > CycleTime2)
    {
        LCD_Backlight_Flag1 = !LCD_Backlight_Flag1;

        if (LCD_Backlight_Flag1 == 0)
        {
            ledcWrite(1, 0);
            delay(10);
        }
        else
        {
            ledcWrite(1, 255);
            delay(10);
        }

        CycleTime2 = millis() + 100; // 100ms
    }

    if (millis() > CycleTime3)
    {
        ledcWrite(1, 255);
        gfx->draw16bitRGBBitmap(0, 0, (uint16_t *)gImage_1, 480, 480); // RGB
        delay(5000);
        CycleTime3 = millis() + 15000; // 15000ms
    }
}
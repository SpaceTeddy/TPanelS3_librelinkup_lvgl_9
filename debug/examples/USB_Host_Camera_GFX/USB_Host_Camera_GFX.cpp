/*
 * @Description: None
 * @version: V1.0.0
 * @Author: None
 * @Date: 2024-04-12 13:15:30
 * @LastEditors: LILYGO_L
 * @LastEditTime: 2024-05-28 13:34:59
 * @License: GPL 3.0
 */
#include <Arduino.h>
#include "USB_STREAM.h"
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include "JPEGDEC.h"

JPEGDEC jpeg;

Arduino_DataBus *bus = new Arduino_XL9535SWSPI(IIC_SDA /* SDA */, IIC_SCL /* SCL */, -1 /* XL PWD */,
                                               XL95X5_CS /* XL CS */, XL95X5_SCLK /* XL SCK */, XL95X5_MOSI /* XL MOSI */);
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    -1 /* DE */, LCD_VSYNC /* VSYNC */, LCD_HSYNC /* HSYNC */, LCD_PCLK /* PCLK */,
    LCD_B0 /* B0 */, LCD_B1 /* B1 */, LCD_B2 /* B2 */, LCD_B3 /* B3 */, LCD_B4 /* B4 */,
    LCD_G0 /* G0 */, LCD_G1 /* G1 */, LCD_G2 /* G2 */, LCD_G3 /* G3 */, LCD_G4 /* G4 */, LCD_G5 /* G5 */,
    LCD_R0 /* R0 */, LCD_R1 /* R1 */, LCD_R2 /* R2 */, LCD_R3 /* R3 */, LCD_R4 /* R4 */,
    1 /* hsync_polarity */, 20 /* hsync_front_porch */, 2 /* hsync_pulse_width */, 0 /* hsync_back_porch */,
    1 /* vsync_polarity */, 30 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 1 /* vsync_back_porch */,
    10 /* pclk_active_neg */, 7500000L /* prefer_speed */, false /* useBigEndian */,
    0 /* de_idle_high*/, 0 /* pclk_idle_high */);
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    LCD_WIDTH /* width */, LCD_HEIGHT /* height */, rgbpanel, 0 /* rotation */, true /* auto_flush */,
    bus, -1 /* RST */, st7701_type9_init_operations, sizeof(st7701_type9_init_operations));

extern Arduino_RGB_Display *gfx;

// pixel drawing callback
static int drawMCU(JPEGDRAW *pDraw)
{
    // Serial.printf("Draw pos = (%d, %d), size = %d x %d\n", pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight);

    gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
    return 1;
} /* drawMCU() */

/* Define the camera frame callback function implementation */
static void onCameraFrameCallback(uvc_frame *frame, void *user_ptr)
{
    // Serial.printf("uvc callback! frame_format = %d, seq = %" PRIu32 ", width = %" PRIu32", height = %" PRIu32 ", length = %u, ptr = %d\n",
    //          frame->frame_format, frame->sequence, frame->width, frame->height, frame->data_bytes, (int)user_ptr);

    if (jpeg.openRAM((uint8_t *)frame->data, frame->data_bytes, drawMCU))
    {
        Serial.println("Successfully opened JPEG image");
        Serial.printf("Image size: %d x %d, orientation: %d, bpp: %d\n", jpeg.getWidth(),
                      jpeg.getHeight(), jpeg.getOrientation(), jpeg.getBpp());
        if (jpeg.hasThumb())
        {
            Serial.printf("Thumbnail present: %d x %d\n", jpeg.getThumbWidth(), jpeg.getThumbHeight());
        }

        jpeg.setMaxOutputSize(MAX_BUFFERED_PIXELS / 16 / 16);
        jpeg.setPixelType(RGB565_BIG_ENDIAN); // The SPI LCD wants the 16-bit pixels in big-endian order

        // Draw the thumbnail image in the middle of the display (upper left corner = 120,100) at 1/4 scale
        if (jpeg.decode(40, 90, JPEG_SCALE_QUARTER))
        {
            Serial.println("Image decoding successful");
        }
        jpeg.close();
        vTaskDelay(1); // 看门狗喂狗
    }

    // gfx->draw16bitBeRGBBitmap(0, 0, (uint16_t *)frame->data, 200, 200);
}

void setup()
{
    Serial.begin(115200);

    pinMode(LCD_BL, OUTPUT);
    digitalWrite(LCD_BL, HIGH);

    Wire.begin(IIC_SDA, IIC_SCL);

    gfx->begin();
    gfx->fillScreen(WHITE);

    // Instantiate an object
    USB_STREAM *usb = new USB_STREAM();

    // allocate memory
    uint8_t *_xferBufferA = (uint8_t *)malloc(55 * 1024);
    assert(_xferBufferA != NULL);
    uint8_t *_xferBufferB = (uint8_t *)malloc(55 * 1024);
    assert(_xferBufferB != NULL);
    uint8_t *_frameBuffer = (uint8_t *)malloc(55 * 1024);
    assert(_frameBuffer != NULL);

    // Config the parameter
    usb->uvcConfiguration(1280, 720, FRAME_INTERVAL_FPS_20, 55 * 1024, _xferBufferA, _xferBufferB, 55 * 1024, _frameBuffer);

    // Register the camera frame callback function
    usb->uvcCamRegisterCb(&onCameraFrameCallback, NULL);

    usb->start();

    usb->connectWait(1000);
    delay(5000);

    usb->uvcCamSuspend(NULL);
    delay(5000);

    usb->uvcCamResume(NULL);

    /*Dont forget to free the allocated memory*/
    // free(_xferBufferA);
    // free(_xferBufferB);
    // free(_frameBuffer);
}

void loop()
{
    vTaskDelay(1);
}

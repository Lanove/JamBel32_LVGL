#include <lgfx_config.h>
#include <lvgl.h>

LV_FONT_DECLARE(Montserrat_SemiBold91)
LV_FONT_DECLARE(Montserrat_SemiBold48)

/*Change to your screen resolution*/
static const uint16_t screenWidth = 480;
static const uint16_t screenHeight = 320;

void my_touchpad_read(lv_indev_drv_t* indev_driver, lv_indev_data_t* data);
void my_disp_flush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p);
void printLVGLDebug(const char* buf);
void lgfx_init();

const size_t pxBufCnt = screenWidth * 20;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[pxBufCnt];
lv_disp_drv_t disp_drv;
lv_indev_drv_t indev_drv;

void lgfx_init() {
    tft.init();
    tft.fillScreen((0x0000));
    uint16_t params[8] = { 3782,3928,231,3932,3816,305,284,248 };
    // tft.calibrateTouch(params, TFT_WHITE, TFT_BLACK);
    tft.setTouchCalibrate(params);
}

void lvgl_esp32_init() {
#if LV_USE_LOG != 0
    lv_log_register_print_cb(printLVGLDebug); /* register print function for debugging */
#endif

    lv_disp_draw_buf_init(&draw_buf, buf, NULL, pxBufCnt);

    /*Initialize the display*/
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    /*Change the following line to your display resolution*/
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /*Initialize the (dummy) input device driver*/
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);
}

#if LV_USE_LOG != 0
/* Serial debugging */
void printLVGLDebug(const char* buf)
{
    Serial.printf(buf);
    Serial.flush();
}
#endif

void my_disp_flush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    //tft.pushColors( ( uint16_t * )&color_p->full, w * h, true );
    tft.writePixels((lgfx::rgb565_t*)&color_p->full, w * h);
    tft.endWrite();

    lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t* indev_driver, lv_indev_data_t* data)
{
    uint16_t touchX, touchY;
    bool touched = tft.getTouch(&touchX, &touchY);
    if (!touched)
        data->state = LV_INDEV_STATE_REL;
    else
    {
        data->state = LV_INDEV_STATE_PR;
        /*Set the coordinates*/
        data->point.x = touchX;
        data->point.y = touchY;
    }
}
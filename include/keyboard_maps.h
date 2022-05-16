#include "lvgl.h"

#define LV_KB_BTN(width) LV_BTNMATRIX_CTRL_POPOVER | width // Need to define here, because LV_KB_BTN is on lv_keyboard.c, and there is no way we can access lv_keyboard.c

static const char* const regularKeyboard_map[] = { "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
                                                 "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
                                                 "Copy", "z", "x", "c", "v", "b", "n", "m", ":","Paste", "\n",
                                                 LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t regularKeyboard_controlMap[] = {
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5, LV_KB_BTN(4), LV_KB_BTN(4), LV_KB_BTN(4), LV_KB_BTN(4), LV_KB_BTN(4), LV_KB_BTN(4), LV_KB_BTN(4), LV_KB_BTN(4), LV_KB_BTN(4), LV_KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED | 7,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 6, LV_KB_BTN(3), LV_KB_BTN(3), LV_KB_BTN(3), LV_KB_BTN(3), LV_KB_BTN(3), LV_KB_BTN(3), LV_KB_BTN(3), LV_KB_BTN(3), LV_KB_BTN(3), LV_BTNMATRIX_CTRL_CHECKED | 7,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | LV_KB_BTN(1),  LV_KEYBOARD_CTRL_BTN_FLAGS | 2,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, LV_BTNMATRIX_CTRL_CHECKED | 2, 6, LV_BTNMATRIX_CTRL_CHECKED | 2, LV_KEYBOARD_CTRL_BTN_FLAGS | 2
};

static const char* const numericKeyboard_map[] = { "1", "2", "3", LV_SYMBOL_KEYBOARD, "\n",
                                                  "4", "5", "6", LV_SYMBOL_OK, "\n",
                                                  "7", "8", "9", LV_SYMBOL_BACKSPACE, "\n",
                                                  LV_SYMBOL_LEFT, "0", LV_SYMBOL_RIGHT, LV_SYMBOL_NEW_LINE, ""
};

static const lv_btnmatrix_ctrl_t numericKeyboard_controlMap[] = {
    1, 1, 1, LV_KEYBOARD_CTRL_BTN_FLAGS | 1,
    1, 1, 1, LV_KEYBOARD_CTRL_BTN_FLAGS | 1,
    1, 1, 1, 1,
    1, 1, 1, LV_BTNMATRIX_CTRL_DISABLED | 1
};

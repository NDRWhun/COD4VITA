#include "vita_input.h"

#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/touch.h>
#include <psp2/common_dialog.h>
#include <string.h>

// engine key codes (src/ui/keycodes.h); the engine has no pad codes of its own
#define K_TAB        0x09
#define K_ENTER      0x0D
#define K_ESCAPE     0x1B
#define K_SPACE      0x20
#define K_BACKSPACE  0x7F
#define K_UPARROW    0x9A
#define K_DOWNARROW  0x9B
#define K_LEFTARROW  0x9C
#define K_RIGHTARROW 0x9D
#define K_MOUSE1     0xC8
#define K_MOUSE2     0xC9

#define VITA_SCREEN_WIDTH   960
#define VITA_SCREEN_HEIGHT  544

#define TOUCH_PANEL_WIDTH   1920
#define TOUCH_PANEL_HEIGHT  1088

#define KEYBOARD_MAX_LENGTH 255

struct VitaButtonMapping
{
    uint32_t button;
    int key;
};

// R fires and L aims, as the shoulder buttons do on the console builds
static const VitaButtonMapping s_buttonMap[] =
{
    { SCE_CTRL_RTRIGGER, K_MOUSE1 },
    { SCE_CTRL_LTRIGGER, K_MOUSE2 },
    { SCE_CTRL_CROSS,    K_SPACE },
    { SCE_CTRL_CIRCLE,   'c' },
    { SCE_CTRL_SQUARE,   'r' },
    { SCE_CTRL_TRIANGLE, 'f' },
    { SCE_CTRL_START,    K_ESCAPE },
    { SCE_CTRL_SELECT,   K_TAB },
    { SCE_CTRL_UP,       K_UPARROW },
    { SCE_CTRL_DOWN,     K_DOWNARROW },
    { SCE_CTRL_LEFT,     K_LEFTARROW },
    { SCE_CTRL_RIGHT,    K_RIGHTARROW },
};

static VitaInputKeyFn s_keyFn;
static VitaInputCharFn s_charFn;
static VitaInputState s_state;
static uint32_t s_previousButtons;
static float s_lookSensitivity = 1.0f;
static float s_deadZone = 0.2f;

static struct
{
    VitaKeyboardStatus status;
    SceWChar16 title[64];
    SceWChar16 initial[KEYBOARD_MAX_LENGTH + 1];
    SceWChar16 buffer[KEYBOARD_MAX_LENGTH + 1];
    char result[KEYBOARD_MAX_LENGTH + 1];
} s_keyboard;

bool VitaInput_Init(VitaInputKeyFn keyFn, VitaInputCharFn charFn)
{
    memset(&s_state, 0, sizeof(s_state));
    memset(&s_keyboard, 0, sizeof(s_keyboard));
    s_keyFn = keyFn;
    s_charFn = charFn;
    s_previousButtons = 0;

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    return true;
}

void VitaInput_Shutdown(void)
{
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP);
    s_keyFn = NULL;
    s_charFn = NULL;
}

// sticks report 0..255 with 128 at rest; below the dead zone counts as centred
static float VitaInput_Axis(uint8_t raw)
{
    const float value = ((float)raw - 128.0f) / 127.0f;
    if (value > -s_deadZone && value < s_deadZone)
        return 0.0f;
    return value;
}

static void VitaInput_PollTouch(void)
{
    SceTouchData touch;
    if (sceTouchRead(SCE_TOUCH_PORT_FRONT, &touch, 1) < 0 || !touch.reportNum)
    {
        s_state.touchActive = false;
        return;
    }

    s_state.touchActive = true;
    s_state.touchX = touch.report[0].x * VITA_SCREEN_WIDTH / TOUCH_PANEL_WIDTH;
    s_state.touchY = touch.report[0].y * VITA_SCREEN_HEIGHT / TOUCH_PANEL_HEIGHT;
}

void VitaInput_Frame(void)
{
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0)
        return;

    const uint32_t pressed = pad.buttons & ~s_previousButtons;
    const uint32_t released = ~pad.buttons & s_previousButtons;

    if (s_keyFn)
    {
        for (uint32_t i = 0; i < sizeof(s_buttonMap) / sizeof(s_buttonMap[0]); ++i)
        {
            if (pressed & s_buttonMap[i].button)
                s_keyFn(s_buttonMap[i].key, true);
            else if (released & s_buttonMap[i].button)
                s_keyFn(s_buttonMap[i].key, false);
        }
    }
    s_previousButtons = pad.buttons;

    s_state.moveSide = VitaInput_Axis(pad.lx);
    s_state.moveForward = -VitaInput_Axis(pad.ly);
    s_state.lookYaw = VitaInput_Axis(pad.rx) * s_lookSensitivity;
    s_state.lookPitch = VitaInput_Axis(pad.ry) * s_lookSensitivity;

    VitaInput_PollTouch();
}

const VitaInputState *VitaInput_State(void)
{
    return &s_state;
}

void VitaInput_SetLookSensitivity(float sensitivity)
{
    s_lookSensitivity = sensitivity;
}

void VitaInput_SetDeadZone(float deadZone)
{
    s_deadZone = deadZone;
}

// --- on-screen keyboard ---

static void VitaKeyboard_ToWide(const char *src, SceWChar16 *dst, uint32_t maxChars)
{
    uint32_t i = 0;
    if (src)
    {
        for (; src[i] && i < maxChars; ++i)
            dst[i] = (SceWChar16)(unsigned char)src[i];
    }
    dst[i] = 0;
}

static void VitaKeyboard_ToAscii(const SceWChar16 *src, char *dst, uint32_t maxChars)
{
    uint32_t i = 0;
    for (; src[i] && i < maxChars; ++i)
        dst[i] = src[i] < 0x80 ? (char)src[i] : '?';
    dst[i] = 0;
}

bool VitaKeyboard_Open(const char *title, const char *initialText, uint32_t maxLength)
{
    if (s_keyboard.status == VITA_KEYBOARD_RUNNING)
        return false;
    if (!maxLength || maxLength > KEYBOARD_MAX_LENGTH)
        maxLength = KEYBOARD_MAX_LENGTH;

    memset(&s_keyboard, 0, sizeof(s_keyboard));
    VitaKeyboard_ToWide(title, s_keyboard.title,
                        sizeof(s_keyboard.title) / sizeof(s_keyboard.title[0]) - 1);
    VitaKeyboard_ToWide(initialText, s_keyboard.initial, maxLength);

    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.supportedLanguages = 0;
    param.languagesForced = SCE_TRUE;
    param.type = SCE_IME_TYPE_DEFAULT;
    param.title = s_keyboard.title;
    param.maxTextLength = maxLength;
    param.initialText = s_keyboard.initial;
    param.inputTextBuffer = s_keyboard.buffer;

    if (sceImeDialogInit(&param) < 0)
        return false;

    s_keyboard.status = VITA_KEYBOARD_RUNNING;
    return true;
}

VitaKeyboardStatus VitaKeyboard_Update(void)
{
    if (s_keyboard.status != VITA_KEYBOARD_RUNNING)
        return s_keyboard.status;

    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return VITA_KEYBOARD_RUNNING;

    SceImeDialogResult result;
    memset(&result, 0, sizeof(result));
    sceImeDialogGetResult(&result);

    if (result.button == SCE_IME_DIALOG_BUTTON_ENTER)
    {
        VitaKeyboard_ToAscii(s_keyboard.buffer, s_keyboard.result, KEYBOARD_MAX_LENGTH);
        s_keyboard.status = VITA_KEYBOARD_DONE;
    }
    else
    {
        s_keyboard.result[0] = 0;
        s_keyboard.status = VITA_KEYBOARD_CANCELLED;
    }

    sceImeDialogTerm();
    return s_keyboard.status;
}

const char *VitaKeyboard_Result(void)
{
    return s_keyboard.result;
}

void VitaKeyboard_Close(void)
{
    if (s_keyboard.status == VITA_KEYBOARD_RUNNING)
        sceImeDialogTerm();
    memset(&s_keyboard, 0, sizeof(s_keyboard));
}

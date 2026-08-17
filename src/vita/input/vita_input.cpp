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

// each control gets its own bindable identity ("bind AUX1 ..."), nothing pretends to be a key
#define K_AUX1       0xCF
#define K_AUX2       0xD0
#define K_AUX3       0xD1
#define K_AUX4       0xD2
#define K_AUX5       0xD3
#define K_AUX6       0xD4
#define K_AUX7       0xD5
#define K_AUX8       0xD6
#define K_AUX9       0xD7
#define K_AUX10      0xD8
#define K_AUX11      0xD9
#define K_AUX12      0xDA
#define K_AUX13      0xDB

#define VITA_SCREEN_WIDTH   960
#define VITA_SCREEN_HEIGHT  544

// used only until sceTouchGetPanelInfo reports the real front panel range
#define TOUCH_PANEL_WIDTH   1920
#define TOUCH_PANEL_HEIGHT  1088

#define KEYBOARD_MAX_LENGTH 255

#define STICK_DIR_COUNT 4

struct VitaButtonMapping
{
    uint32_t button;
    int key;
};

// every control raises its own AUX key; what it does is decided by binds, not by this table
static const VitaButtonMapping s_gameMap[] =
{
    { SCE_CTRL_CROSS,    K_AUX1 },
    { SCE_CTRL_CIRCLE,   K_AUX2 },
    { SCE_CTRL_SQUARE,   K_AUX3 },
    { SCE_CTRL_TRIANGLE, K_AUX4 },
    { SCE_CTRL_LTRIGGER, K_AUX5 },
    { SCE_CTRL_RTRIGGER, K_AUX6 },
    { SCE_CTRL_UP,       K_AUX7 },
    { SCE_CTRL_DOWN,     K_AUX8 },
    { SCE_CTRL_LEFT,     K_AUX9 },
    { SCE_CTRL_RIGHT,    K_AUX10 },
    { SCE_CTRL_SELECT,   K_AUX11 },
    { SCE_CTRL_START,    K_ESCAPE },
};

// cross must be ENTER here: Menu_HandleKey activates an item on 13, not on space
static const VitaButtonMapping s_menuMap[] =
{
    { SCE_CTRL_CROSS,    K_ENTER },
    { SCE_CTRL_CIRCLE,   K_ESCAPE },
    { SCE_CTRL_START,    K_ESCAPE },
    { SCE_CTRL_SELECT,   K_TAB },
    { SCE_CTRL_SQUARE,   K_BACKSPACE },
    { SCE_CTRL_UP,       K_UPARROW },
    { SCE_CTRL_DOWN,     K_DOWNARROW },
    { SCE_CTRL_LEFT,     K_LEFTARROW },
    { SCE_CTRL_RIGHT,    K_RIGHTARROW },
};

// in game the sticks feed CL_GamepadMove as analog axes; only the menus want key repeats
static const int s_menuStickKeys[STICK_DIR_COUNT] = { K_UPARROW, K_DOWNARROW, K_LEFTARROW, K_RIGHTARROW };

static VitaInputKeyFn s_keyFn;
static VitaInputCharFn s_charFn;
static VitaInputState s_state;
static uint32_t s_previousButtons;
static uint32_t s_stickKeys;
static VitaInputContext s_context;
static bool s_resyncButtons;
static float s_lookSensitivity = 1.0f;
static float s_deadZone = 0.2f;
static float s_moveThreshold = 0.35f;

static bool s_rearHeld[2];

static int s_touchOriginX = 0;
static int s_touchOriginY = 0;
static int s_touchSpanX = TOUCH_PANEL_WIDTH;
static int s_touchSpanY = TOUCH_PANEL_HEIGHT;

static struct
{
    VitaKeyboardStatus status;
    SceWChar16 title[64];
    SceWChar16 initial[KEYBOARD_MAX_LENGTH + 1];
    SceWChar16 buffer[KEYBOARD_MAX_LENGTH + 1];
    char result[KEYBOARD_MAX_LENGTH + 1];
} s_keyboard;

static const VitaButtonMapping *VitaInput_Map(uint32_t *count)
{
    if (s_context == VITA_INPUT_MENU)
    {
        *count = sizeof(s_menuMap) / sizeof(s_menuMap[0]);
        return s_menuMap;
    }
    *count = sizeof(s_gameMap) / sizeof(s_gameMap[0]);
    return s_gameMap;
}

static void VitaInput_QueryTouchPanel(void)
{
    SceTouchPanelInfo info;
    if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &info) < 0)
        return;
    if (info.maxDispX <= info.minDispX || info.maxDispY <= info.minDispY)
        return;

    s_touchOriginX = info.minDispX;
    s_touchOriginY = info.minDispY;
    s_touchSpanX = info.maxDispX - info.minDispX + 1;
    s_touchSpanY = info.maxDispY - info.minDispY + 1;
}

bool VitaInput_Init(VitaInputKeyFn keyFn, VitaInputCharFn charFn)
{
    memset(&s_state, 0, sizeof(s_state));
    memset(&s_keyboard, 0, sizeof(s_keyboard));
    s_keyFn = keyFn;
    s_charFn = charFn;
    s_previousButtons = 0;
    s_stickKeys = 0;
    s_context = VITA_INPUT_GAME;
    s_resyncButtons = false;

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    VitaInput_QueryTouchPanel();
    return true;
}

void VitaInput_Shutdown(void)
{
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_STOP);
    s_keyFn = NULL;
    s_charFn = NULL;
}

// sticks report 0..255 with 128 at rest; the range past the dead zone is rescaled to 0..1
static float VitaInput_Axis(uint8_t raw)
{
    float value = ((float)raw - 128.0f) / 127.0f;
    if (value < 0.0f)
    {
        if (value > -s_deadZone)
            return 0.0f;
        return (value + s_deadZone) / (1.0f - s_deadZone);
    }
    if (value < s_deadZone)
        return 0.0f;
    return (value - s_deadZone) / (1.0f - s_deadZone);
}

static void VitaInput_PollTouch(void)
{
    SceTouchData touch;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) < 0 || !touch.reportNum)
    {
        s_state.touchActive = false;
        return;
    }

    int x = ((int)touch.report[0].x - s_touchOriginX) * VITA_SCREEN_WIDTH / s_touchSpanX;
    int y = ((int)touch.report[0].y - s_touchOriginY) * VITA_SCREEN_HEIGHT / s_touchSpanY;
    if (x < 0)
        x = 0;
    else if (x >= VITA_SCREEN_WIDTH)
        x = VITA_SCREEN_WIDTH - 1;
    if (y < 0)
        y = 0;
    else if (y >= VITA_SCREEN_HEIGHT)
        y = VITA_SCREEN_HEIGHT - 1;

    s_state.touchActive = true;
    s_state.touchX = x;
    s_state.touchY = y;
}

// the release threshold sits below the press threshold so a stick held near it cannot chatter
static void VitaInput_EmitStickKeys(void)
{
    if (s_context != VITA_INPUT_MENU)
        return;
    const int *keys = s_menuStickKeys;
    const float deflection[STICK_DIR_COUNT] =
    {
        s_state.moveForward, -s_state.moveForward, -s_state.moveSide, s_state.moveSide
    };

    for (uint32_t i = 0; i < STICK_DIR_COUNT; ++i)
    {
        const bool held = (s_stickKeys & (1u << i)) != 0;
        const bool down = deflection[i] > (held ? s_moveThreshold * 0.7f : s_moveThreshold);
        if (down == held)
            continue;

        s_stickKeys ^= 1u << i;
        if (s_keyFn)
            s_keyFn(keys[i], down);
    }
}

static void VitaInput_ReleaseHeld(void)
{
    uint32_t count;
    const VitaButtonMapping *map = VitaInput_Map(&count);

    if (s_keyFn)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            if (s_previousButtons & map[i].button)
                s_keyFn(map[i].key, false);
        }
        for (uint32_t i = 0; i < STICK_DIR_COUNT; ++i)
        {
            if (s_stickKeys & (1u << i))
                s_keyFn(s_menuStickKeys[i], false);
        }
        for (uint32_t i = 0; i < 2; ++i)
        {
            if (s_rearHeld[i])
            {
                s_keyFn(i ? K_AUX13 : K_AUX12, false);
                s_rearHeld[i] = false;
            }
        }
    }
    s_stickKeys = 0;
}

void VitaInput_SetContext(VitaInputContext context)
{
    if (context == s_context)
        return;

    VitaInput_ReleaseHeld();
    s_context = context;
    s_resyncButtons = true;
}

void VitaInput_Frame(void)
{
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0)
        return;

    // a button already down when the map changed must not fire a press under the new map
    if (s_resyncButtons)
    {
        s_previousButtons = pad.buttons;
        s_resyncButtons = false;
    }

    const uint32_t pressed = pad.buttons & ~s_previousButtons;
    const uint32_t released = ~pad.buttons & s_previousButtons;

    uint32_t count;
    const VitaButtonMapping *map = VitaInput_Map(&count);

    if (s_keyFn)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            if (pressed & map[i].button)
                s_keyFn(map[i].key, true);
            else if (released & map[i].button)
                s_keyFn(map[i].key, false);
        }
    }
    s_previousButtons = pad.buttons;

    s_state.moveSide = VitaInput_Axis(pad.lx);
    s_state.moveForward = -VitaInput_Axis(pad.ly);
    s_state.lookYaw = VitaInput_Axis(pad.rx) * s_lookSensitivity;
    s_state.lookPitch = VitaInput_Axis(pad.ry) * s_lookSensitivity;

    VitaInput_EmitStickKeys();
    VitaInput_PollTouch();

    // the back panel is two bindable halves, live only while the game has the controls
    bool rear[2] = { false, false };
    SceTouchData back;
    if (s_context == VITA_INPUT_GAME &&
        sceTouchPeek(SCE_TOUCH_PORT_BACK, &back, 1) >= 0)
    {
        for (uint32_t i = 0; i < back.reportNum && i < SCE_TOUCH_MAX_REPORT; ++i)
            rear[back.report[i].x >= TOUCH_PANEL_WIDTH / 2] = true;
    }
    for (uint32_t i = 0; i < 2; ++i)
    {
        if (rear[i] != s_rearHeld[i])
        {
            s_rearHeld[i] = rear[i];
            if (s_keyFn)
                s_keyFn(i ? K_AUX13 : K_AUX12, rear[i]);
        }
    }
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

void VitaInput_SetMoveThreshold(float threshold)
{
    s_moveThreshold = threshold;
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

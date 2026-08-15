// Pad, sticks, touch and the on-screen keyboard.
//
// The engine has no gamepad key codes -- CoD4 on PC is keyboard and mouse -- so pad
// buttons emit the keyboard and mouse codes the binds already use, and the sticks
// drive movement and look separately.
#pragma once

#include <stdint.h>

// key and character events, mirroring the engine's SE_KEY / SE_CHAR
typedef void (*VitaInputKeyFn)(int key, bool down);
typedef void (*VitaInputCharFn)(int character);

struct VitaInputState
{
    float moveForward;      // -1 to 1, from the left stick
    float moveSide;
    float lookYaw;          // right stick, already scaled by sensitivity
    float lookPitch;
    bool touchActive;
    int touchX;             // in screen pixels
    int touchY;
};

bool VitaInput_Init(VitaInputKeyFn keyFn, VitaInputCharFn charFn);
void VitaInput_Shutdown(void);

// polls the pad and touch panel, emitting events for anything that changed
void VitaInput_Frame(void);

const VitaInputState *VitaInput_State(void);

void VitaInput_SetLookSensitivity(float sensitivity);
void VitaInput_SetDeadZone(float deadZone);

// --- on-screen keyboard, for text fields ---

enum VitaKeyboardStatus
{
    VITA_KEYBOARD_IDLE,
    VITA_KEYBOARD_RUNNING,
    VITA_KEYBOARD_DONE,
    VITA_KEYBOARD_CANCELLED
};

// title and initial text are ASCII; maxLength counts characters, not bytes
bool VitaKeyboard_Open(const char *title, const char *initialText, uint32_t maxLength);

// must be called every frame while running; drives the dialog's own update
VitaKeyboardStatus VitaKeyboard_Update(void);

// valid once the status reads DONE; empty on cancel
const char *VitaKeyboard_Result(void);

void VitaKeyboard_Close(void);

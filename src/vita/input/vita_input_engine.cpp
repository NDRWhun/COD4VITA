#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <client/client.h>
#include <client/cl_input.h>
#include <ui/ui_shared.h>
#include <win32/win_local.h>
#include <vita/input/vita_input.h>

#include <math.h>
#include <stdio.h>
#include <sys/stat.h>

#define KEYCATCH_CONSOLE 0x1
#define KEYCATCH_UI      0x10

#define K_MOUSE1 0xC8

#define VITA_SCREEN_WIDTH  960
#define VITA_SCREEN_HEIGHT 544

static const dvar_s *vita_lookSpeed;
static const dvar_s *vita_lookPower;
static const dvar_s *vita_cursorSpeed;
static const dvar_s *vita_moveThreshold;
static const dvar_s *vita_touchCursor;

static uint32_t s_lastFrameTime;
static float s_lookRemainderX;
static float s_lookRemainderY;
static int s_cursorX = VITA_SCREEN_WIDTH / 2;
static int s_cursorY = VITA_SCREEN_HEIGHT / 2;
static bool s_touchHeld;
static bool s_dvarsRegistered;

// nothing calls IN_Init in this build, so the dvars are claimed on the first frame instead
static void IN_VitaRegisterDvars()
{
    if (s_dvarsRegistered)
        return;
    s_dvarsRegistered = true;
    s_lastFrameTime = Sys_Milliseconds();

    vita_lookSpeed = Dvar_RegisterFloat(
        "vita_lookSpeed", 1200.0f, 0.0f, 20000.0f, DVAR_ARCHIVE,
        "Right stick look speed, in mouse counts per second at full deflection");
    vita_lookPower = Dvar_RegisterFloat(
        "vita_lookPower", 2.0f, 1.0f, 4.0f, DVAR_ARCHIVE,
        "Right stick response curve exponent; 1 is linear");
    vita_cursorSpeed = Dvar_RegisterFloat(
        "vita_cursorSpeed", 700.0f, 0.0f, 5000.0f, DVAR_ARCHIVE,
        "Right stick menu cursor speed, in pixels per second at full deflection");
    vita_moveThreshold = Dvar_RegisterFloat(
        "vita_moveThreshold", 0.35f, 0.05f, 0.95f, DVAR_ARCHIVE,
        "Left stick deflection that counts as a movement key press");
    vita_touchCursor = Dvar_RegisterBool(
        "vita_touchCursor", 1, DVAR_ARCHIVE,
        "Drive the menu cursor from the front touch screen");

    // written once; the player edits it freely
    mkdir("ux0:data/kisakcod/raw", 0777);
    FILE *binds = fopen("ux0:data/kisakcod/raw/vita_controls.cfg", "r");
    if (binds)
        fclose(binds);
    else
    {
        binds = fopen("ux0:data/kisakcod/raw/vita_controls.cfg", "w");
        if (binds)
        {
            // a semicolon separates commands even inside a comment, so the text carries none
            fputs("// vita controls: AUX1-4 cross/circle/square/triangle, AUX5-6 L/R,\n"
                  "// AUX7-10 dpad up/down/left/right, AUX11 select, AUX12-13 rear touch left/right\n"
                  "bind AUX1 \"+gostand\"\n"
                  "bind AUX2 \"togglecrouch\"\n"
                  "bind AUX3 \"+usereload\"\n"
                  "bind AUX4 \"weapnext\"\n"
                  "bind AUX5 \"+toggleads_throw\"\n"
                  "bind AUX6 \"+attack\"\n"
                  "bind AUX7 \"+nightvision\"\n"
                  "bind AUX8 \"toggleprone\"\n"
                  "bind AUX9 \"+smoke\"\n"
                  "bind AUX10 \"+frag\"\n"
                  "bind AUX11 \"+breath_sprint\"\n"
                  "bind AUX12 \"+breath_sprint\"\n"
                  "bind AUX13 \"+melee\"\n", binds);
            fclose(binds);
        }
    }
    Cbuf_AddText(0, "exec vita_controls.cfg\n");
}

static float IN_VitaStickResponse(float value)
{
    const float magnitude = fabsf(value);
    if (magnitude == 0.0f)
        return 0.0f;

    const float shaped = powf(magnitude, vita_lookPower->current.value);
    return value < 0.0f ? -shaped : shaped;
}

static int IN_VitaClamp(int value, int last)
{
    if (value < 0)
        return 0;
    if (value > last)
        return last;
    return value;
}

// the menu's text fields are dvar-backed, so the system keyboard edits the dvar directly
static void IN_VitaPumpKeyboard(void)
{
    extern int g_editingField;
    extern itemDef_s *g_editItem;
    static bool wasEditing;

    if (g_editingField && g_editItem && g_editItem->dvar && !wasEditing)
    {
        wasEditing = true;
        VitaKeyboard_Open(g_editItem->dvar, Dvar_GetVariantString(g_editItem->dvar), 64);
    }
    if (!g_editingField)
        wasEditing = false;

    const VitaKeyboardStatus status = VitaKeyboard_Update();
    if (status != VITA_KEYBOARD_DONE && status != VITA_KEYBOARD_CANCELLED)
        return;

    if (status == VITA_KEYBOARD_DONE && g_editingField && g_editItem && g_editItem->dvar)
        Dvar_SetStringByName(g_editItem->dvar, VitaKeyboard_Result());

    // same teardown as a rejected key
    VitaKeyboard_Close();
    g_editingField = 0;
    g_editItem = NULL;
    wasEditing = false;
}

// called several times per engine frame, so every delta here is measured against the wall clock
void __cdecl IN_Frame()
{
    IN_VitaRegisterDvars();
    IN_VitaPumpKeyboard();

    const bool uiActive = Key_IsCatcherActive(0, KEYCATCH_UI);
    const bool consoleActive = Key_IsCatcherActive(0, KEYCATCH_CONSOLE);

    VitaInput_SetMoveThreshold(vita_moveThreshold->current.value);
    VitaInput_SetContext(uiActive || consoleActive ? VITA_INPUT_MENU : VITA_INPUT_GAME);
    VitaInput_Frame();

    const VitaInputState *state = VitaInput_State();

    const uint32_t now = Sys_Milliseconds();
    uint32_t msec = now - s_lastFrameTime;
    s_lastFrameTime = now;
    if (msec > 250)
        msec = 250;

    const dvar_s *speedDvar = uiActive ? vita_cursorSpeed : vita_lookSpeed;
    const float speed = speedDvar->current.value * (float)msec * 0.001f;
    s_lookRemainderX += IN_VitaStickResponse(state->lookYaw) * speed;
    s_lookRemainderY += IN_VitaStickResponse(state->lookPitch) * speed;

    int dx = (int)s_lookRemainderX;
    int dy = (int)s_lookRemainderY;
    s_lookRemainderX -= (float)dx;
    s_lookRemainderY -= (float)dy;

    const bool touchCursor = uiActive && vita_touchCursor->current.enabled && state->touchActive;
    const bool touchEdge = touchCursor != s_touchHeld;
    if (touchEdge)
    {
        s_touchHeld = touchCursor;
        Sys_QueEvent(0, SE_KEY, K_MOUSE1, touchCursor, 0, NULL);
    }

    // CL_MouseEvent's console-only branch reads r_fullscreen, and there is no cursor to place there
    if (consoleActive && !uiActive)
        return;

    bool pointerMoved = false;
    if (uiActive)
    {
        if (touchCursor)
        {
            // the press edge counts, so a touch landing on the cursor still shows and places it
            pointerMoved = touchEdge || state->touchX != s_cursorX || state->touchY != s_cursorY;
            s_cursorX = state->touchX;
            s_cursorY = state->touchY;
        }
        else if (dx || dy)
        {
            s_cursorX = IN_VitaClamp(s_cursorX + dx, VITA_SCREEN_WIDTH - 1);
            s_cursorY = IN_VitaClamp(s_cursorY + dy, VITA_SCREEN_HEIGHT - 1);
            pointerMoved = true;
        }
        // the menus take an absolute cursor; a delta would move the view behind them
        dx = 0;
        dy = 0;
    }

    if (uiActive && (dx || dy || pointerMoved))
        CL_MouseEvent(s_cursorX, s_cursorY, dx, dy);
}

// the Vita has no pointing device to capture
void __cdecl IN_ActivateMouse(int force)
{
}

// nor a system cursor to hide; the menus draw their own
void __cdecl IN_ShowSystemCursor(int show)
{
}

bool __cdecl IN_IsForegroundWindow()
{
    return true;
}

void __cdecl IN_SetForegroundWindow()
{
}

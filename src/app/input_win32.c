/*
 * input_win32.c — translate Windows keyboard/mouse input to RFB events and
 * forward them to the worker over the IPC channel.
 *
 * Keyboard: Windows virtual-key codes are mapped to X11 keysyms (RFB uses X11
 * keysyms). Printable ASCII maps to itself; the special keys are tabled below.
 * This baseline (VK-based) handling is refined in M4, which adds the QEMU
 * Extended Key Event path (keysym + XT scancode) for correct layout handling.
 */
#include "app/app.h"

#include <windowsx.h>

/* X11 keysyms we need (from X11/keysymdef.h; values are stable). */
#define XK_BackSpace 0xFF08
#define XK_Tab       0xFF09
#define XK_Return    0xFF0D
#define XK_Escape    0xFF1B
#define XK_Home      0xFF50
#define XK_Left      0xFF51
#define XK_Up        0xFF52
#define XK_Right     0xFF53
#define XK_Down      0xFF54
#define XK_Prior     0xFF55
#define XK_Next      0xFF56
#define XK_End       0xFF57
#define XK_Insert    0xFF63
#define XK_Delete    0xFFFF
#define XK_F1        0xFFBE
#define XK_Shift_L   0xFFE1
#define XK_Shift_R   0xFFE2
#define XK_Control_L 0xFFE3
#define XK_Control_R 0xFFE4
#define XK_Alt_L     0xFFE9
#define XK_Alt_R     0xFFEA
#define XK_Super_L   0xFFEB

static uint32_t vk_to_keysym(WPARAM vk, LPARAM lparam)
{
    BOOL extended = (lparam & (1 << 24)) != 0;
    switch (vk) {
    case VK_BACK:   return XK_BackSpace;
    case VK_TAB:    return XK_Tab;
    case VK_RETURN: return XK_Return;
    case VK_ESCAPE: return XK_Escape;
    case VK_HOME:   return XK_Home;
    case VK_LEFT:   return XK_Left;
    case VK_UP:     return XK_Up;
    case VK_RIGHT:  return XK_Right;
    case VK_DOWN:   return XK_Down;
    case VK_PRIOR:  return XK_Prior;
    case VK_NEXT:   return XK_Next;
    case VK_END:    return XK_End;
    case VK_INSERT: return XK_Insert;
    case VK_DELETE: return XK_Delete;
    case VK_SHIFT: case VK_LSHIFT:   return XK_Shift_L;
    case VK_RSHIFT:                  return XK_Shift_R;
    case VK_CONTROL: return extended ? XK_Control_R : XK_Control_L;
    case VK_LCONTROL: return XK_Control_L;
    case VK_RCONTROL: return XK_Control_R;
    case VK_MENU:    return extended ? XK_Alt_R : XK_Alt_L;
    case VK_LMENU:   return XK_Alt_L;
    case VK_RMENU:   return XK_Alt_R;
    case VK_LWIN: case VK_RWIN: return XK_Super_L;
    case VK_SPACE:  return ' ';
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12)
        return XK_F1 + (uint32_t)(vk - VK_F1);

    /* Printable keys: resolve to a character honoring current layout + shift. */
    BYTE ks[256];
    if (GetKeyboardState(ks)) {
        WCHAR chars[4];
        int n = ToUnicode((UINT)vk, (UINT)((lparam >> 16) & 0xFF), ks, chars, 4, 0);
        if (n == 1 && chars[0] >= 0x20)
            return (uint32_t)chars[0]; /* Latin-1 / Unicode == keysym below 0x100 */
    }
    return 0;
}

void input_key(ViewerApp *app, WPARAM vk, LPARAM lparam, BOOL down)
{
    if (app->view_only)
        return;
    uint32_t keysym = vk_to_keysym(vk, lparam);
    if (!keysym)
        return;
    vnc_ipc_key k = { keysym, (uint8_t)(down ? 1 : 0) };
    vnc_channel_send(&app->ch, VNC_CMD_KEY, &k, sizeof(k));
}

void input_pointer(ViewerApp *app, int x, int y, UINT msg, WPARAM wparam)
{
    if (app->view_only)
        return;

    /* Track button state across events into the RFB button mask (bit0=left,
     * bit1=middle, bit2=right, bit3=wheel-up, bit4=wheel-down). */
    static int mask = 0;
    switch (msg) {
    case WM_LBUTTONDOWN: mask |= 1; break;
    case WM_LBUTTONUP:   mask &= ~1; break;
    case WM_MBUTTONDOWN: mask |= 2; break;
    case WM_MBUTTONUP:   mask &= ~2; break;
    case WM_RBUTTONDOWN: mask |= 4; break;
    case WM_RBUTTONUP:   mask &= ~4; break;
    default: break;
    }

    if (x < 0) x = 0;
    if (y < 0) y = 0;

    if (msg == WM_MOUSEWHEEL) {
        int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        int wheel_bit = delta > 0 ? 8 : 16;
        vnc_ipc_pointer down = { (uint16_t)x, (uint16_t)y, (uint8_t)(mask | wheel_bit) };
        vnc_ipc_pointer up   = { (uint16_t)x, (uint16_t)y, (uint8_t)mask };
        vnc_channel_send(&app->ch, VNC_CMD_POINTER, &down, sizeof(down));
        vnc_channel_send(&app->ch, VNC_CMD_POINTER, &up, sizeof(up));
        return;
    }

    vnc_ipc_pointer p = { (uint16_t)x, (uint16_t)y, (uint8_t)mask };
    vnc_channel_send(&app->ch, VNC_CMD_POINTER, &p, sizeof(p));
}

#define _INC_OLE
#include <windows.h>
#undef _INC_OLE
#include <tchar.h>
#include "General.h"
#include "DarkMode.h"
#include "ClipHandler.h"

#define WINDOW_TITLE TEXT("ClipMaster")

#define ID_HANDLER_FAVORITE   (CLIP_HANDLER_FAVORITE)
#define ID_HANDLER_REMOVE     (CLIP_HANDLER_REMOVE)
#define ID_HANDLER_SAVE       (CLIP_HANDLER_SAVE)
#define ID_HANDLER_MARK       10
#define ID_HANDLER_TITLE      11

typedef struct {
    int choice;
    BOOL done;
    HFONT font, mark_font, title_font;
} CLIP_HANDLER;

static int handler_scale(HWND window, int value)
{
    return MulDiv(value, GetDpiForWindow(window), 96);
}

static HWND handler_control(HWND owner, const TCHAR *type, const TCHAR *text, DWORD style, int id, HFONT font)
{
    HWND control = CreateWindowEx(0, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, owner,
        (HMENU)(INT_PTR)id, (HINSTANCE)GetWindowLongPtr(owner, GWLP_HINSTANCE), NULL);
    SendMessage(control, WM_SETFONT, (WPARAM)font, TRUE);
    return control;
}

static HFONT handler_font(HWND window, const TCHAR *face, int points)
{
    return CreateFont(-MulDiv(points, GetDpiForWindow(window), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

static void handler_layout(HWND window)
{
    int margin = handler_scale(window, 24), gap = handler_scale(window, 12);
    int button_height = handler_scale(window, 40);
    RECT client;
    int width, top;
    GetClientRect(window, &client);
    width = (client.right - margin * 2 - gap * 2) / 3;
    top = client.bottom - margin - button_height;
    MoveWindow(GetDlgItem(window, ID_HANDLER_MARK), margin, handler_scale(window, 20), handler_scale(window, 40),
        handler_scale(window, 44), TRUE);
    MoveWindow(GetDlgItem(window, ID_HANDLER_TITLE), margin + handler_scale(window, 38), handler_scale(window, 28),
        client.right - margin * 2 - handler_scale(window, 38), handler_scale(window, 32), TRUE);
    MoveWindow(GetDlgItem(window, ID_HANDLER_FAVORITE), margin, top, width, button_height, TRUE);
    MoveWindow(GetDlgItem(window, ID_HANDLER_REMOVE), margin + width + gap, top, width, button_height, TRUE);
    MoveWindow(GetDlgItem(window, ID_HANDLER_SAVE), margin + (width + gap) * 2, top, width, button_height, TRUE);
}

static LRESULT CALLBACK handler_proc(HWND window, UINT message, WPARAM parameter, LPARAM data)
{
    CLIP_HANDLER *handler = (CLIP_HANDLER *)GetWindowLongPtr(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        SetWindowLongPtr(window, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCT *)data)->lpCreateParams);
        return DefWindowProc(window, message, parameter, data);
    }
    if (handler == NULL) return DefWindowProc(window, message, parameter, data);
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(parameter) == ID_HANDLER_FAVORITE || LOWORD(parameter) == ID_HANDLER_REMOVE ||
            LOWORD(parameter) == ID_HANDLER_SAVE) {
            handler->choice = LOWORD(parameter);
            DestroyWindow(window);
        } else if (LOWORD(parameter) == IDCANCEL) {
            DestroyWindow(window);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        handler->done = TRUE;
        PostThreadMessage(GetCurrentThreadId(), WM_NULL, 0, 0);
        return 0;
    }
    return DefWindowProc(window, message, parameter, data);
}

int cliphandler_show(HINSTANCE instance, HWND owner)
{
    CLIP_HANDLER handler;
    WNDCLASS descriptor;
    const DWORD style = WS_CAPTION | WS_SYSMENU, extended = WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME;
    HWND window;
    RECT frame, owner_rect;
    MSG message;
    UINT dpi = GetDpiForWindow(owner);
    ZeroMemory(&handler, sizeof(handler));
    ZeroMemory(&descriptor, sizeof(descriptor));
    descriptor.hInstance = instance;
    descriptor.lpszClassName = TEXT("ClipMasterHandler");
    descriptor.lpfnWndProc = handler_proc;
    descriptor.hCursor = LoadCursor(NULL, IDC_ARROW);
    descriptor.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    RegisterClass(&descriptor);
    SetRect(&frame, 0, 0, MulDiv(408, dpi, 96), MulDiv(148, dpi, 96));
    AdjustWindowRectExForDpi(&frame, style, FALSE, extended, dpi);
    GetWindowRect(owner, &owner_rect);
    window = CreateWindowEx(extended, descriptor.lpszClassName, WINDOW_TITLE, style,
        (owner_rect.left + owner_rect.right - (frame.right - frame.left)) / 2,
        (owner_rect.top + owner_rect.bottom - (frame.bottom - frame.top)) / 2,
        frame.right - frame.left, frame.bottom - frame.top, owner, NULL, instance, &handler);
    if (window == NULL) return CLIP_HANDLER_CANCEL;
    handler.font = handler_font(window, TEXT("Malgun Gothic"), 9);
    handler.mark_font = handler_font(window, TEXT("Segoe UI Symbol"), 22);
    handler.title_font = handler_font(window, TEXT("Malgun Gothic"), 13);
    handler_control(window, TEXT("STATIC"), TEXT("\x2713"), SS_CENTER, ID_HANDLER_MARK, handler.mark_font);
    handler_control(window, TEXT("STATIC"), TEXT("Clip Handler"), SS_LEFT, ID_HANDLER_TITLE, handler.title_font);
    handler_control(window, TEXT("BUTTON"), TEXT("add favorite"), BS_PUSHBUTTON | WS_TABSTOP, ID_HANDLER_FAVORITE, handler.font);
    handler_control(window, TEXT("BUTTON"), TEXT("remove clip"), BS_PUSHBUTTON | WS_TABSTOP, ID_HANDLER_REMOVE, handler.font);
    handler_control(window, TEXT("BUTTON"), TEXT("save as file"), BS_PUSHBUTTON | WS_TABSTOP, ID_HANDLER_SAVE, handler.font);
    handler_layout(window);
    dark_mode_set_window(window);
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    SetFocus(GetDlgItem(window, ID_HANDLER_FAVORITE));
    while (!handler.done) {
        int result = GetMessage(&message, NULL, 0, 0);
        if (result <= 0) { if (result == 0) PostQuitMessage((int)message.wParam); break; }
        if (!IsDialogMessage(window, &message)) { TranslateMessage(&message); DispatchMessage(&message); }
    }
    if (IsWindow(window)) DestroyWindow(window);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    DeleteObject(handler.font);
    DeleteObject(handler.mark_font);
    DeleteObject(handler.title_font);
    return handler.choice;
}

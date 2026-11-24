#ifndef _INC_PASTEPLUS_H
#define _INC_PASTEPLUS_H
#include "Data.h"
#define WM_PASTEPLUS_PASTE (WM_APP + 1150)
#define WM_PASTEPLUS_COPY (WM_APP + 1151)
#define WM_PASTEPLUS_OCR_DONE (WM_APP + 1152)
/* lParam: const OPTION_INFO * holding the edited paste handlers; the viewer applies and saves it, returns TRUE on success. */
#define WM_PASTEPLUS_APPLY_HANDLERS (WM_APP + 1153)
#define PASTEPLUS_BUTTON_PRIMARY 0
#define PASTEPLUS_BUTTON_NORMAL 1
#define PASTEPLUS_BUTTON_DANGER 2
#ifdef __cplusplus
extern "C" {
#endif
HWND pasteplus_create(HINSTANCE instance, HWND owner, int identifier);
HWND pasteplus_symbols_create(HINSTANCE instance, HWND owner, int identifier);
void pasteplus_refresh(HWND window, DATA_INFO *item);
/* Flat colored push button. The button needs BS_OWNERDRAW; its parent forwards WM_DRAWITEM to pasteplus_button_draw. */
void pasteplus_button_style(HWND button, int kind);
BOOL pasteplus_button_draw(const DRAWITEMSTRUCT *draw);
#ifdef __cplusplus
}
#endif
#endif
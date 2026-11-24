#ifndef _INC_CLIPHANDLER_H
#define _INC_CLIPHANDLER_H
#include <windows.h>

#define CLIP_HANDLER_CANCEL   0
#define CLIP_HANDLER_FAVORITE 21
#define CLIP_HANDLER_REMOVE   22
#define CLIP_HANDLER_SAVE     23

/* Right-click menu of a history clip: add favorite / remove clip / save as file. Returns one of CLIP_HANDLER_*. */
int cliphandler_show(HINSTANCE instance, HWND owner);
#endif

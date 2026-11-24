/*
 * CPCP
 *
 * gdip.h
 *
 * Copyright (C) 1996-2019 by Ohno Tomoaki. All rights reserved.
 *		https://www.nakka.com/
 *		nakka@nakka.com
 */

#pragma once

 /* Include Files */
#define _INC_OLE
#include <windows.h>
#undef  _INC_OLE
#include <tchar.h>

#ifdef __cplusplus
extern "C" {
#endif

	/* Include Files */

	/* Define */

	/* Struct */

	/* Function Prototypes */
	void init_gdip();
	void shutdown_gdip();

	int save_jpeg(HBITMAP hBmp, LPCWSTR lpszFilename, ULONG uQuality);
	int save_png(HBITMAP hBmp, LPCWSTR lpszFilename);
	int save_png_hbitmap(HBITMAP hBmp, LPCWSTR lpszFilename);
	int save_png_encoded(const BYTE *data, DWORD size, LPCWSTR lpszFilename);
	int save_png_dib(const BYTE *dib, DWORD size, LPCWSTR lpszFilename);

	HBITMAP image_to_bitmap(HDC hdc, LPCWSTR lpszFilename);
	HBITMAP image_bytes_to_bitmap(const BYTE *data, DWORD size, int *width, int *height);
	int draw_circle_smooth(HDC hdc, int left, int top, int right, int bottom, COLORREF color);
#ifdef __cplusplus
}
#endif
/* End of source */

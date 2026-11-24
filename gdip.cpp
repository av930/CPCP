/*
 * CPCP
 *
 * gdip.cpp
 *
 * Copyright (C) 1996-2019 by Ohno Tomoaki. All rights reserved.
 *		https://www.nakka.com/
 *		nakka@nakka.com
 */

 /* Include Files */

#define _INC_OLE
#include <windows.h>
#undef  _INC_OLE
#include <objidl.h>
#include <objbase.h>
#include <vector>
#include <gdiplus.h>

using namespace Gdiplus;

#include "gdip.h"

extern "C" {
#include "dpi.h"
}

#pragma comment(lib, "gdiplus.lib")

/* Define */

/* Global Variables */
static HMODULE hModuleThread;
static ULONG_PTR gdiplusToken;

typedef Status(WINAPI* pGdipCreateBitmapFromHBITMAP)(HBITMAP, HPALETTE, GpBitmap**);
typedef Status(WINAPI* pGdipSaveImageToFile)(GpBitmap*, const WCHAR*, const CLSID*, const Gdiplus::EncoderParameters*);
typedef Status(WINAPI* pGdipDisposeImage)(GpBitmap*);

static pGdipCreateBitmapFromHBITMAP _GdipCreateBitmapFromHBITMAP;
static pGdipSaveImageToFile _GdipSaveImageToFile;
static pGdipDisposeImage _GdipDisposeImage;

/* Local Function Prototypes */

/*
 * init_gdip - GDI+の初期化
 */
void init_gdip()
{
	hModuleThread = LoadLibrary(TEXT("gdiplus.dll"));
	_GdipCreateBitmapFromHBITMAP = (pGdipCreateBitmapFromHBITMAP)GetProcAddress(hModuleThread, "GdipCreateBitmapFromHBITMAP");
	_GdipSaveImageToFile = (pGdipSaveImageToFile)GetProcAddress(hModuleThread, "GdipSaveImageToFile");
	_GdipDisposeImage = (pGdipDisposeImage)GetProcAddress(hModuleThread, "GdipDisposeImage");

	GdiplusStartupInput gdiplusStartupInput;
	GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, 0);
}

/*
 * shutdown_gdip - GDI+の終了処理
 */
void shutdown_gdip()
{
	GdiplusShutdown(gdiplusToken);
	FreeLibrary(hModuleThread);
}

/*
 * save_jpeg - HBITMAPをJPEGで保存
 */
int save_jpeg(HBITMAP hBmp, LPCWSTR lpszFilename, ULONG uQuality)
{
	GpBitmap* pBitmap = NULL;
	CLSID imageCLSID;
	Gdiplus::EncoderParameters encoderParams;

	if (_GdipCreateBitmapFromHBITMAP == NULL || _GdipSaveImageToFile == NULL || _GdipDisposeImage == NULL) {
		return 0;
	}
	_GdipCreateBitmapFromHBITMAP(hBmp, NULL, &pBitmap);
	CLSIDFromString(L"{557CF401-1A04-11D3-9A73-0000F81EF32E}", &imageCLSID);	//jpeg
	encoderParams.Count = 1;
	encoderParams.Parameter[0].NumberOfValues = 1;
	encoderParams.Parameter[0].Guid = Gdiplus::EncoderQuality;
	encoderParams.Parameter[0].Type = EncoderParameterValueTypeLong;
	encoderParams.Parameter[0].Value = &uQuality;
	_GdipSaveImageToFile(pBitmap, lpszFilename, &imageCLSID, &encoderParams);
	_GdipDisposeImage(pBitmap);
	return 1;
}

/*
 * save_png - HBITMAPをPNGで保存
 */
int save_png(HBITMAP hBmp, LPCWSTR lpszFilename)
{
	GpBitmap* pBitmap = NULL;
	CLSID imageCLSID;

	if (_GdipCreateBitmapFromHBITMAP == NULL || _GdipSaveImageToFile == NULL || _GdipDisposeImage == NULL) {
		return 0;
	}
	_GdipCreateBitmapFromHBITMAP(hBmp, NULL, &pBitmap);
	CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &imageCLSID);	//png
	_GdipSaveImageToFile(pBitmap, lpszFilename, &imageCLSID, NULL);
	_GdipDisposeImage(pBitmap);
	return 1;
}

/*
 * gdip_start - GDI+ が未初期化なら一時的に初期化する (FALSE: 初期化に失敗)
 */
static BOOL gdip_start(ULONG_PTR *local_token)
{
	*local_token = 0;
	if (gdiplusToken) return TRUE;
	GdiplusStartupInput startup;
	return GdiplusStartup(local_token, &startup, NULL) == Ok;
}

static int gdip_save_png(Gdiplus::Bitmap *image, LPCWSTR lpszFilename)
{
	CLSID imageCLSID;
	if (image == NULL || image->GetLastStatus() != Ok) return 0;
	CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &imageCLSID);	//png
	return image->Save(lpszFilename, &imageCLSID, NULL) == Ok;
}

/*
 * save_png_hbitmap - HBITMAPをPNGで保存 (失敗時は 0)
 */
int save_png_hbitmap(HBITMAP hBmp, LPCWSTR lpszFilename)
{
	ULONG_PTR local_token;
	int result = 0;
	if (!gdip_start(&local_token)) return 0;
	{
		Gdiplus::Bitmap *image = Gdiplus::Bitmap::FromHBITMAP(hBmp, NULL);
		result = gdip_save_png(image, lpszFilename);
		delete image;
	}
	if (local_token) GdiplusShutdown(local_token);
	return result;
}

/*
 * save_png_encoded - エンコード済み画像 (JPEG/GIF/TIFF など) をPNGで保存 (失敗時は 0)
 */
int save_png_encoded(const BYTE *data, DWORD size, LPCWSTR lpszFilename)
{
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
	void *buffer = memory ? GlobalLock(memory) : NULL;
	IStream *stream = NULL;
	ULONG_PTR local_token;
	int result = 0;
	if (buffer == NULL) { if (memory) GlobalFree(memory); return 0; }
	CopyMemory(buffer, data, size);
	GlobalUnlock(memory);
	if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) { GlobalFree(memory); return 0; }
	if (gdip_start(&local_token)) {
		Gdiplus::Bitmap *image = Gdiplus::Bitmap::FromStream(stream, FALSE);
		result = gdip_save_png(image, lpszFilename);
		delete image;
		if (local_token) GdiplusShutdown(local_token);
	}
	stream->Release();
	return result;
}

/*
 * save_png_dib - パックDIB (CF_DIB / CF_DIBV5) をPNGで保存 (失敗時は 0)
 */
int save_png_dib(const BYTE *dib, DWORD size, LPCWSTR lpszFilename)
{
	const BITMAPINFOHEADER *header = (const BITMAPINFOHEADER *)dib;
	ULONGLONG image_bytes, offset;
	DWORD colors;
	HDC context;
	HBITMAP bitmap;
	int result;
	if (size < sizeof(BITMAPINFOHEADER) || header->biSize < sizeof(BITMAPINFOHEADER) || header->biSize > size ||
		header->biWidth <= 0 || header->biHeight == 0 || header->biHeight == LONG_MIN || header->biPlanes != 1 ||
		(header->biCompression != BI_RGB && header->biCompression != BI_BITFIELDS)) {
		return 0;
	}
	colors = header->biClrUsed;
	if (colors == 0 && header->biBitCount <= 8) colors = 1U << header->biBitCount;
	offset = header->biSize + (ULONGLONG)colors * sizeof(RGBQUAD);
	if (header->biSize == sizeof(BITMAPINFOHEADER) && header->biCompression == BI_BITFIELDS) offset += 12;
	image_bytes = ((ULONGLONG)header->biWidth * header->biBitCount + 31) / 32 * 4;
	image_bytes *= (header->biHeight < 0 ? -header->biHeight : header->biHeight);
	if (offset > size || image_bytes > size - offset) return 0;
	/* A DIBV5 that Windows synthesizes from a DIB keeps the 12 byte color masks after the header: the pixels end the data. */
	if (header->biCompression == BI_BITFIELDS && size - image_bytes > offset && size - image_bytes <= offset + 12)
		offset = size - image_bytes;
	context = GetDC(NULL);
	bitmap = CreateDIBitmap(context, header, CBM_INIT, dib + offset, (const BITMAPINFO *)dib, DIB_RGB_COLORS);
	ReleaseDC(NULL, context);
	if (bitmap == NULL) return 0;
	result = save_png_hbitmap(bitmap, lpszFilename);
	DeleteObject(bitmap);
	return result;
}

/*
 * draw_image_file - 画像ファイルをHBITMAPに変換
 */
HBITMAP image_bytes_to_bitmap(const BYTE *data, DWORD size, int *width, int *height)
{
	HBITMAP result = NULL;
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
	void *buffer = memory ? GlobalLock(memory) : NULL;
	IStream *stream = NULL;
	ULONG_PTR local_token = 0;
	if (buffer == NULL) { if (memory) GlobalFree(memory); return NULL; }
	CopyMemory(buffer, data, size);
	GlobalUnlock(memory);
	if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) { GlobalFree(memory); return NULL; }
	if (!gdiplusToken) {
		GdiplusStartupInput startup;
		if (GdiplusStartup(&local_token, &startup, NULL) != Ok) { stream->Release(); return NULL; }
	}
	{
		Gdiplus::Bitmap *image = Gdiplus::Bitmap::FromStream(stream, FALSE);
		if (image && image->GetLastStatus() == Ok && image->GetWidth() && image->GetHeight() &&
			(ULONGLONG)image->GetWidth() * image->GetHeight() <= 67108864) {
			*width = image->GetWidth();
			*height = image->GetHeight();
			int target_width = *width >= *height ? min(*width, 384) : max(1, MulDiv(*width, min(*height, 384), *height));
			int target_height = *height >= *width ? min(*height, 384) : max(1, MulDiv(*height, min(*width, 384), *width));
			Gdiplus::Bitmap thumbnail(target_width, target_height, PixelFormat32bppARGB);
			Gdiplus::Graphics drawing(&thumbnail);
			drawing.Clear(Gdiplus::Color::White);
			drawing.SetInterpolationMode(InterpolationModeHighQualityBicubic);
			if (drawing.DrawImage(image, 0, 0, target_width, target_height) == Ok)
				thumbnail.GetHBITMAP(Gdiplus::Color::White, &result);
		}
		delete image;
	}
	stream->Release();
	if (local_token) GdiplusShutdown(local_token);
	return result;
}

HBITMAP image_to_bitmap(HDC hdc, LPCWSTR lpszFilename)
{
	Gdiplus::Image* image = new Gdiplus::Image(lpszFilename);

	HDC mdc = ::CreateCompatibleDC(hdc);
	HBITMAP hbmp = ::CreateCompatibleBitmap(hdc, image->GetWidth(), image->GetHeight());
	HBITMAP hOldBmp = (HBITMAP)::SelectObject(mdc, hbmp);

	Gdiplus::Graphics* graphics = new Gdiplus::Graphics(mdc);
	graphics->DrawImage(image, 0, 0, image->GetWidth(), image->GetHeight());
	delete(image);
	delete(graphics);

	::SelectObject(mdc, hOldBmp);
	::DeleteDC(mdc);
	return hbmp;
}

/*
 * draw_circle_smooth - アンチエイリアス付きで円を塗りつぶす (GDI+ 未初期化なら 0 を返す)
 */
int draw_circle_smooth(HDC hdc, int left, int top, int right, int bottom, COLORREF color)
{
	if (!gdiplusToken) return 0;
	Gdiplus::Graphics drawing(hdc);
	Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
	drawing.SetSmoothingMode(SmoothingModeAntiAlias);
	drawing.SetPixelOffsetMode(PixelOffsetModeHalf);
	return drawing.FillEllipse(&brush, (Gdiplus::REAL)left, (Gdiplus::REAL)top,
		(Gdiplus::REAL)(right - left), (Gdiplus::REAL)(bottom - top)) == Ok;
}
/* End of source */

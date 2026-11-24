#define _INC_OLE
#include <windows.h>
#undef _INC_OLE
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <tchar.h>
#include <wincodec.h>
#include <wincrypt.h>
#include <string>
#include <thread>
#include <vector>
#include <new>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>
extern "C" {
#include "General.h"
#include "Memory.h"
#include "String.h"
#include "Data.h"
#include "Ini.h"
#include "Format.h"
#include "ClipBoard.h"
#include "DarkMode.h"
extern OPTION_INFO option;
}
#include "PastePlus.h"
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "delayimp.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "windowscodecs.lib")

struct FormatTile {
    DATA_INFO *format;
    int handler;
    HWND window;
    HWND preview;
    HBITMAP image;
    DWORD byte_size;
    ULONGLONG byte_hash;
};

struct PastePane {
    DATA_INFO *snapshot = nullptr;
    std::vector<DATA_INFO *> image_clips;   /* clips holding an image, newest first; valid until the next refresh */
    size_t next_image = 0;                  /* index in image_clips read by the next "Extract text" */
    FILETIME ocr_clip_time = {};
    size_t ocr_clip_number = 0;
    HWND extracted = nullptr;               /* shows the extracted text */
    std::vector<FormatTile> tiles;
    HWND information = nullptr;
    HWND status = nullptr;
    HWND extract_text = nullptr;
    int offset = 0;
    int height = 0;
    bool layout = false;
    bool ocr_running = false;
    int handler_count = 3;                  /* visible rows of the paste handler editor */
    HWND handler_title = nullptr;
    HWND handler_header[3] = {};
    HWND handler_label[16] = {};
    HWND handler_edit[48] = {};             /* row * 3 + column: target, condition, action */
    HWND handler_add = nullptr;
    HWND handler_remove = nullptr;
    HWND handler_save = nullptr;
    HWND handler_status = nullptr;
};

struct OcrCompletion {
    std::wstring text;
    std::wstring error;
};

struct SymbolButton {
    HWND window;
    wchar_t symbol;
};

struct SymbolsPane {
    std::vector<HWND> headings;
    std::vector<SymbolButton> symbols;
    HWND charmap_button = nullptr;
    HFONT font = nullptr;
    int offset = 0;
    int height = 0;
    bool layout = false;
};

struct CharacterMapWindow {
    HWND window = nullptr;
    DWORD process = 0;
};

struct CharacterMapFontSearch {
    HWND combo = nullptr;
    LPVOID font_name = nullptr;
    int index = CB_ERR;
};

static bool format_is_ocr_image(DATA_INFO *format);
static void extract_text_worker(HWND window, DATA_INFO *image);
static HBITMAP create_image_preview(DATA_INFO *format, UINT max_width, UINT max_height);

static int pane_scale(HWND window, int value) { return MulDiv(value, GetDpiForWindow(window), 96); }

/* Moves a child without repainting it; the pane redraws everything once after a layout pass. */
static void pane_place(HWND window, int x, int y, int width, int height)
{
    SetWindowPos(window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
}

static ULONGLONG bytes_hash(const std::vector<BYTE> &bytes)
{
    ULONGLONG hash = 14695981039346656037ULL;
    for (BYTE value : bytes) { hash ^= value; hash *= 1099511628211ULL; }
    return hash;
}

static bool decode_multibyte(const BYTE *bytes, DWORD size, UINT codepage, DWORD flags, std::wstring &text)
{
    int characters = MultiByteToWideChar(codepage, flags, reinterpret_cast<const char *>(bytes), static_cast<int>(size), nullptr, 0);
    if (!characters) return false;
    text.resize(characters);
    return MultiByteToWideChar(codepage, flags, reinterpret_cast<const char *>(bytes), static_cast<int>(size), &text[0], characters) != 0;
}

static bool decode_utf16(const BYTE *bytes, DWORD size, std::wstring &text)
{
    if (size < 4) return false;
    DWORD offset = 0;
    bool big_endian = false;
    if (bytes[0] == 0xFF && bytes[1] == 0xFE) offset = 2;
    else if (bytes[0] == 0xFE && bytes[1] == 0xFF) { offset = 2; big_endian = true; }
    else {
        DWORD pairs = size / 2, even_zeroes = 0, odd_zeroes = 0;
        for (DWORD index = 0; index < pairs; index++) {
            even_zeroes += bytes[index * 2] == 0;
            odd_zeroes += bytes[index * 2 + 1] == 0;
        }
        if (max(even_zeroes, odd_zeroes) * 5 < pairs * 3) return false;
        big_endian = even_zeroes > odd_zeroes;
    }
    DWORD characters = min((size - offset) / 2, 32768UL);
    text.clear();
    text.reserve(characters);
    for (DWORD index = 0; index < characters; index++) {
        BYTE first = bytes[offset + index * 2], second = bytes[offset + index * 2 + 1];
        wchar_t character = static_cast<wchar_t>(big_endian ? (first << 8) | second : first | (second << 8));
        if (!character) break;
        text.push_back(character);
    }
    return !text.empty();
}

static bool readable_text(const std::wstring &text)
{
    if (text.empty()) return false;
    size_t printable = 0;
    for (wchar_t character : text) {
        if (character == L'\r' || character == L'\n' || character == L'\t' ||
            (character >= 0x20 && (character < 0x7F || character > 0x9F))) printable++;
    }
    return printable * 5 >= text.size() * 4;
}

static std::wstring escape_controls(const std::wstring &text)
{
    std::wstring result;
    wchar_t value[8];
    for (wchar_t character : text) {
        if (character == L'\r' || character == L'\n' || character == L'\t' ||
            (character >= 0x20 && (character < 0x7F || character > 0x9F))) result += character;
        else {
            wsprintf(value, character <= 0xFF ? L"\\x%02X" : L"\\u%04X", static_cast<unsigned int>(character));
            result += value;
        }
    }
    return result;
}

static bool format_is_text(DATA_INFO *format)
{
    return format->format == CF_TEXT || format->format == CF_UNICODETEXT ||
        format->format == CF_OEMTEXT || format->format == CF_HDROP ||
        (format->format_name && (StrStrI(format->format_name, L"text") ||
         StrStrI(format->format_name, L"html") || StrStrI(format->format_name, L"rtf") ||
         StrStrI(format->format_name, L"rich") || StrStrI(format->format_name, L"csv") ||
         StrStrI(format->format_name, L"json") || StrStrI(format->format_name, L"xml") ||
         StrStrI(format->format_name, L"url") || StrStrI(format->format_name, L"uri") ||
         StrStrI(format->format_name, L"svg") || StrStrI(format->format_name, L"markdown")));
}

static bool format_is_unavailable(DATA_INFO *format)
{
    return !format->data || format->format == CF_LOCALE || format->format == CF_PALETTE ||
        format->format == CF_OWNERDISPLAY ||
        (format->format >= CF_DSPTEXT && format->format <= CF_DSPENHMETAFILE) ||
        (format->format >= CF_PRIVATEFIRST && format->format <= CF_PRIVATELAST) ||
        (format->format_name && (StrStrI(format->format_name, L"Ole Private") ||
         StrStrI(format->format_name, L"DataObject") || StrStrI(format->format_name, L"DropEffect") ||
         !lstrcmpi(format->format_name, L"OleClipboardPersistOnFlush") ||
         !lstrcmpi(format->format_name, L"OneNote 2010 Internal")));
}

static bool target_matches(const wchar_t *targets, DATA_INFO *format);

static bool format_has_handler(DATA_INFO *format)
{
    for (int index = 0; index < option.config_handler_count; index++)
        if (*option.config_handlers[index].formats && *option.config_handlers[index].script &&
            target_matches(option.config_handlers[index].formats, format)) return true;
    return false;
}

static std::wstring format_text(DATA_INFO *format, const std::vector<BYTE> *cached_bytes = nullptr,
    bool raw_binary = false)
{
    wchar_t image[128];
    if (!raw_binary && format->format == CF_BITMAP && format->data) {
        BITMAP bitmap;
        if (GetObject(format->data, sizeof(bitmap), &bitmap)) {
            wsprintf(image, L"Bitmap: %ld x %ld, %u bpp", bitmap.bmWidth, bitmap.bmHeight, bitmap.bmBitsPixel);
            return image;
        }
    }
    if (!raw_binary && (format->format == CF_DIB || format->format == CF_DIBV5) && format->data) {
        BYTE *dib = static_cast<BYTE *>(GlobalLock(format->data));
        if (dib) {
            DWORD available = min(GlobalSize(format->data), format->size);
            if (available >= sizeof(BITMAPINFOHEADER)) {
                BITMAPINFOHEADER *header = reinterpret_cast<BITMAPINFOHEADER *>(dib);
                if (header->biSize >= sizeof(BITMAPINFOHEADER) && header->biSize <= available) {
                    wsprintf(image, L"DIB: %ld x %ld, %u bpp", header->biWidth,
                        header->biHeight < 0 ? -header->biHeight : header->biHeight, header->biBitCount);
                    GlobalUnlock(format->data);
                    return image;
                }
            }
            GlobalUnlock(format->data);
        }
    }
    if (!raw_binary && format->format == CF_ENHMETAFILE && format->data) {
        ENHMETAHEADER header = {};
        if (GetEnhMetaFileHeader(static_cast<HENHMETAFILE>(format->data), sizeof(header), &header)) {
            wsprintf(image, L"Enhanced metafile: %ld x %ld, %u bytes",
                header.rclBounds.right - header.rclBounds.left, header.rclBounds.bottom - header.rclBounds.top,
                GetEnhMetaFileBits(static_cast<HENHMETAFILE>(format->data), 0, nullptr));
            return image;
        }
    }
    if (!raw_binary && format->format == CF_METAFILEPICT && format->data) {
        METAFILEPICT *picture = static_cast<METAFILEPICT *>(GlobalLock(format->data));
        if (picture) {
            wsprintf(image, L"Metafile picture: %ld x %ld, %u bytes", picture->xExt, picture->yExt,
                GetMetaFileBitsEx(picture->hMF, 0, nullptr));
            GlobalUnlock(format->data);
            return image;
        }
    }
    if (!raw_binary && !format_is_text(format)) {
        wsprintf(image, L"Binary data: %u bytes", format->size);
        return image;
    }
    if (format->format == CF_UNICODETEXT && format->data) {
        const wchar_t *text = static_cast<const wchar_t *>(GlobalLock(format->data));
        if (text) {
            size_t available = min(GlobalSize(format->data), format->size) / sizeof(wchar_t);
            size_t length = 0;
            while (length < available && length < 65536 && text[length]) length++;
            std::wstring result(text, length);
            GlobalUnlock(format->data);
            return result;
        }
    }
    if (format->format == CF_HDROP && format->data) {
        std::wstring result;
        UINT count = DragQueryFile(static_cast<HDROP>(format->data), 0xFFFFFFFF, nullptr, 0);
        for (UINT index = 0; index < count; index++) {
            wchar_t path[32768];
            if (DragQueryFile(static_cast<HDROP>(format->data), index, path, ARRAYSIZE(path))) result += std::wstring(path) + L"\r\n";
        }
        return result;
    }
    DWORD size = 0;
    BYTE *allocated = nullptr;
    const BYTE *bytes = nullptr;
    if (cached_bytes && !cached_bytes->empty()) {
        bytes = cached_bytes->data();
        size = static_cast<DWORD>(cached_bytes->size());
    } else {
        allocated = format_data_to_bytes(format, &size);
        if (!allocated) allocated = clipboard_data_to_bytes(format, &size);
        bytes = allocated;
    }
    if (!bytes) return L"";
    std::wstring result;
    if (raw_binary && !format_is_text(format)) {
        wchar_t value[8];
        DWORD display_size = min(size, 2048UL);
        for (DWORD index = 0; index < display_size; index++) {
            wsprintf(value, L"%02X%s", bytes[index], (index + 1) % 16 ? L" " : L"\r\n");
            result += value;
        }
        if (size > display_size) result += L"\r\n[truncated]";
        mem_free(reinterpret_cast<void **>(&allocated));
        return result;
    }
    DWORD length = min(size, 65536UL);
    while (length && bytes[length - 1] == 0) length--;
    std::wstring decoded;
    bool named_text = format->format_name && (StrStrI(format->format_name, L"text") ||
        StrStrI(format->format_name, L"html") || StrStrI(format->format_name, L"rtf") ||
        StrStrI(format->format_name, L"rich") || StrStrI(format->format_name, L"csv") ||
        StrStrI(format->format_name, L"json") || StrStrI(format->format_name, L"xml") ||
        StrStrI(format->format_name, L"url") || StrStrI(format->format_name, L"uri") ||
        StrStrI(format->format_name, L"svg") || StrStrI(format->format_name, L"markdown"));
    bool standard_text = format->format == CF_TEXT || format->format == CF_OEMTEXT;
    bool utf16 = decode_utf16(bytes, length, decoded);
    if (!utf16 && length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        decode_multibyte(bytes + 3, length - 3, CP_UTF8, MB_ERR_INVALID_CHARS, decoded);
    if (decoded.empty() && !standard_text && !utf16)
        decode_multibyte(bytes, length, CP_UTF8, MB_ERR_INVALID_CHARS, decoded);
    UINT codepage = format->format == CF_OEMTEXT ? CP_OEMCP : CP_ACP;
    if (decoded.empty() && length) decode_multibyte(bytes, length, codepage, 0, decoded);
    if (named_text || readable_text(decoded)) {
        result = escape_controls(decoded);
        if (size > length && length == 65536) result += L"\r\n[truncated]";
    } else {
        wsprintf(image, L"Binary data: %u bytes", size);
        result = image;
    }
    mem_free(reinterpret_cast<void **>(&allocated));
    return result;
}

static std::wstring quote_ps(const wchar_t *text)
{
    std::wstring result = L"'";
    for (; *text; text++) { result += *text; if (*text == L'\'') result += L'\''; }
    return result + L"'";
}

static bool powershell_transform(const wchar_t *pattern, const wchar_t *script, const std::wstring &input,
    std::wstring &output, std::wstring &error)
{
    SECURITY_ATTRIBUTES security = { sizeof(security), nullptr, TRUE };
    wchar_t temporary[MAX_PATH], input_path[MAX_PATH], output_path[MAX_PATH], executable[MAX_PATH];
    HANDLE source = INVALID_HANDLE_VALUE, destination = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION process = {};
    STARTUPINFO startup = {};
    DWORD written, exit_code = 1;
    bool success = false;
    GetTempPath(ARRAYSIZE(temporary), temporary);
    if (!GetTempFileName(temporary, L"cmi", 0, input_path)) return false;
    if (!GetTempFileName(temporary, L"cmo", 0, output_path)) { DeleteFile(input_path); return false; }
    source = CreateFile(input_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    destination = CreateFile(output_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (source != INVALID_HANDLE_VALUE && destination != INVALID_HANDLE_VALUE) {
        int length = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(length, '\0');
        if (length) WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), &utf8[0], length, nullptr, nullptr);
        if (WriteFile(source, utf8.data(), length, &written, nullptr) && written == static_cast<DWORD>(length)) {
            SetFilePointer(source, 0, nullptr, FILE_BEGIN);
            std::wstring command = L"$ErrorActionPreference='Stop';[Console]::InputEncoding=[Text.UTF8Encoding]::new($false);"
                L"[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);$clip=[Console]::In.ReadToEnd();";
            if (*pattern) command += L"$rx=[regex]::new(" + quote_ps(pattern) + L",[Text.RegularExpressions.RegexOptions]::None,[timespan]::FromMilliseconds(250));if(!$rx.IsMatch($clip)){exit 2};";
            command += L"& { " + std::wstring(script) + L" } | ForEach-Object { [Console]::WriteLine([string]$_) };";
            DWORD encoded_length = 0;
            CryptBinaryToString(reinterpret_cast<const BYTE *>(command.data()), static_cast<DWORD>(command.size() * sizeof(wchar_t)),
                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &encoded_length);
            std::wstring encoded(encoded_length, L'\0');
            CryptBinaryToString(reinterpret_cast<const BYTE *>(command.data()), static_cast<DWORD>(command.size() * sizeof(wchar_t)),
                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &encoded[0], &encoded_length);
            encoded.resize(wcslen(encoded.c_str()));
            GetSystemDirectory(executable, ARRAYSIZE(executable));
            std::wstring executable_path = std::wstring(executable) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
            std::wstring arguments = L"\"" + executable_path + L"\" -NoLogo -NoProfile -NonInteractive -EncodedCommand " + encoded;
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_HIDE;
            startup.hStdInput = source;
            startup.hStdOutput = destination;
            startup.hStdError = destination;
            if (CreateProcess(executable_path.c_str(), &arguments[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                nullptr, temporary, &startup, &process)) {
                HANDLE job = CreateJobObject(nullptr, nullptr);
                if (job) {
                    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
                    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
                    AssignProcessToJobObject(job, process.hProcess);
                }
                DWORD wait = WaitForSingleObject(process.hProcess, 15000);
                if (wait != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000); }
                GetExitCodeProcess(process.hProcess, &exit_code);
                LARGE_INTEGER size;
                if (wait == WAIT_OBJECT_0 && exit_code == 0 && GetFileSizeEx(destination, &size) && size.QuadPart <= 8388608) {
                    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
                    SetFilePointer(destination, 0, nullptr, FILE_BEGIN);
                    DWORD read = 0;
                    if (ReadFile(destination, bytes.empty() ? nullptr : &bytes[0], static_cast<DWORD>(bytes.size()), &read, nullptr)) {
                        int characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), read, nullptr, 0);
                        output.resize(characters);
                        if (characters) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), read, &output[0], characters);
                        if (output.size() >= 2 && output.substr(output.size() - 2) == L"\r\n") output.resize(output.size() - 2);
                        success = characters != 0 || read == 0;
                    }
                }
                if (job) CloseHandle(job);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                if (wait != WAIT_OBJECT_0) error = L"Handler timed out.";
                else if (exit_code == 2) error = L"The condition did not match.";
                else if (!success) error = L"Handler failed or returned invalid output.";
            } else error = L"Could not start Windows PowerShell.";
        }
    }
    if (source != INVALID_HANDLE_VALUE) CloseHandle(source);
    if (destination != INVALID_HANDLE_VALUE) CloseHandle(destination);
    DeleteFile(input_path);
    DeleteFile(output_path);
    if (!success && error.empty()) error = L"Could not prepare the handler.";
    return success;
}

static bool target_matches(const wchar_t *targets, DATA_INFO *format)
{
    std::wstring list = targets;
    size_t start = 0;
    for (;;) {
        size_t end = list.find_first_of(L",;|", start);
        std::wstring name = list.substr(start, end == std::wstring::npos ? end : end - start);
        while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
        while (!name.empty() && iswspace(name.back())) name.pop_back();
        if ((!name.empty() && format->format_name && _wcsicmp(name.c_str(), format->format_name) == 0) ||
            (_wcsicmp(name.c_str(), L"CF_TEXT") == 0 && format->format == CF_TEXT) ||
            ((_wcsicmp(name.c_str(), L"CF_OEMTEXT") == 0 || _wcsicmp(name.c_str(), L"CT_OEMTEXT") == 0) && format->format == CF_OEMTEXT) ||
            ((_wcsicmp(name.c_str(), L"CF_UNICODETEXT") == 0 || _wcsicmp(name.c_str(), L"CF_UNICODE") == 0) && format->format == CF_UNICODETEXT) ||
            (_wcsicmp(name.c_str(), L"CF_DIB") == 0 && format->format == CF_DIB) ||
            (_wcsicmp(name.c_str(), L"CF_HDROP") == 0 && format->format == CF_HDROP)) return true;
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return false;
}

static void pane_clear(PastePane *pane)
{
    for (auto &tile : pane->tiles) {
        DestroyWindow(tile.window);
        if (tile.image) DeleteObject(tile.image);
    }
    pane->tiles.clear();
    data_free(pane->snapshot);
    pane->snapshot = nullptr;
    pane->image_clips.clear();
    pane->next_image = 0;
}

static LRESULT CALLBACK format_tile_proc(HWND window, UINT message, WPARAM parameter, LPARAM data,
    UINT_PTR identifier, DWORD_PTR reference)
{
    UNREFERENCED_PARAMETER(identifier);
    if (message == WM_LBUTTONUP) {
        HWND pane = reinterpret_cast<HWND>(reference);
        HWND tile = GetParent(window);
        return SendMessage(pane, WM_COMMAND, GetDlgCtrlID(tile), 0);
    }
    return DefSubclassProc(window, message, parameter, data);
}

static HWND pane_control(HWND owner, const wchar_t *type, const wchar_t *text, DWORD style, int id)
{
    HWND control = CreateWindowEx(lstrcmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0, type, text,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, 0, 0, 0, 0, owner, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        reinterpret_cast<HINSTANCE>(GetWindowLongPtr(owner, GWLP_HINSTANCE)), nullptr);
    SendMessage(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return control;
}

#define BUTTON_STYLE_PROP L"ClipMaster.ButtonStyle"
#define BUTTON_HOVER_PROP L"ClipMaster.ButtonHover"

static COLORREF color_mix(COLORREF base, COLORREF other, int percent)
{
    return RGB((GetRValue(base) * (100 - percent) + GetRValue(other) * percent) / 100,
        (GetGValue(base) * (100 - percent) + GetGValue(other) * percent) / 100,
        (GetBValue(base) * (100 - percent) + GetBValue(other) * percent) / 100);
}

/* Tracks the mouse so the button can draw a hover color. */
static LRESULT CALLBACK button_hover_proc(HWND window, UINT message, WPARAM parameter, LPARAM data,
    UINT_PTR identifier, DWORD_PTR reference)
{
    UNREFERENCED_PARAMETER(reference);
    switch (message) {
    case WM_MOUSEMOVE:
        if (!GetProp(window, BUTTON_HOVER_PROP)) {
            TRACKMOUSEEVENT track = { sizeof(track), TME_LEAVE, window, 0 };
            TrackMouseEvent(&track);
            SetProp(window, BUTTON_HOVER_PROP, reinterpret_cast<HANDLE>(1));
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemoveProp(window, BUTTON_HOVER_PROP);
        InvalidateRect(window, nullptr, FALSE);
        break;
    case WM_NCDESTROY:
        RemoveProp(window, BUTTON_HOVER_PROP);
        RemoveProp(window, BUTTON_STYLE_PROP);
        RemoveWindowSubclass(window, button_hover_proc, identifier);
        break;
    }
    return DefSubclassProc(window, message, parameter, data);
}

extern "C" void pasteplus_button_style(HWND button, int kind)
{
    SetProp(button, BUTTON_STYLE_PROP, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(kind + 1)));
    SetWindowSubclass(button, button_hover_proc, 1, 0);
}

extern "C" BOOL pasteplus_button_draw(const DRAWITEMSTRUCT *draw)
{
    if (!draw || draw->CtlType != ODT_BUTTON) return FALSE;
    INT_PTR kind = reinterpret_cast<INT_PTR>(GetProp(draw->hwndItem, BUTTON_STYLE_PROP)) - 1;
    if (kind < 0) return FALSE;
    bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    bool hover = GetProp(draw->hwndItem, BUTTON_HOVER_PROP) != nullptr;
    COLORREF surface = dark_mode_get_color(COLOR_BTNFACE), ink = dark_mode_get_color(COLOR_WINDOWTEXT);
    COLORREF fill, border, text;
    if (kind == PASTEPLUS_BUTTON_PRIMARY) { fill = RGB(36, 112, 196); border = RGB(22, 79, 151); text = RGB(255, 255, 255); }
    else if (kind == PASTEPLUS_BUTTON_DANGER) { fill = RGB(200, 62, 62); border = RGB(150, 40, 40); text = RGB(255, 255, 255); }
    else { fill = dark_mode_get_color(COLOR_WINDOW); border = color_mix(ink, surface, 60); text = ink; }
    bool colored = kind != PASTEPLUS_BUTTON_NORMAL;
    if (disabled) {
        fill = color_mix(fill, surface, 70);
        border = color_mix(border, surface, 70);
        text = color_mix(text, surface, 60);
    } else if (pressed) {
        fill = colored ? color_mix(fill, RGB(0, 0, 0), 20) : color_mix(fill, ink, 16);
    } else if (hover) {
        fill = colored ? color_mix(fill, RGB(255, 255, 255), 14) : color_mix(fill, ink, 7);
        if (!colored) border = color_mix(ink, surface, 30);
    }
    HFONT font = reinterpret_cast<HFONT>(SendMessage(draw->hwndItem, WM_GETFONT, 0, 0));
    HBRUSH fill_brush = CreateSolidBrush(fill);
    HBRUSH border_brush = CreateSolidBrush(border);
    RECT area = draw->rcItem;
    wchar_t caption[128];
    GetWindowText(draw->hwndItem, caption, ARRAYSIZE(caption));
    int saved = SaveDC(draw->hDC);
    FillRect(draw->hDC, &area, fill_brush);
    FrameRect(draw->hDC, &area, border_brush);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, text);
    SelectObject(draw->hDC, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    if (pressed && !disabled) OffsetRect(&area, 0, 1);
    DrawText(draw->hDC, caption, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if ((draw->itemState & ODS_FOCUS) && !disabled) {
        RECT focus = draw->rcItem;
        InflateRect(&focus, -pane_scale(draw->hwndItem, 3), -pane_scale(draw->hwndItem, 3));
        SetTextColor(draw->hDC, text);
        DrawFocusRect(draw->hDC, &focus);
    }
    RestoreDC(draw->hDC, saved);
    DeleteObject(fill_brush);
    DeleteObject(border_brush);
    return TRUE;
}

static const wchar_t *symbol_names[] = {
    L"Common Symbols", L"Arrows", L"Mathematics", L"Currency", L"Greek", L"Box Drawing"
};

static const wchar_t *symbol_sets[] = {
    L"\x00A9\x00AE\x2122\x2022\x2026\x00B0\x00A7\x00B6\x2713\x2717\x2605\x2606\x2665\x2666\x2663\x2660\x260E\x2709\x2611\x2610\x203B\x2030\x2116\x00B7",
    L"\x2190\x2191\x2192\x2193\x2194\x2195\x2196\x2197\x2198\x2199\x21D0\x21D1\x21D2\x21D3\x21D4\x21B5\x21BA\x21BB",
    L"\x00B1\x00D7\x00F7\x2260\x2248\x2264\x2265\x221E\x221A\x2211\x220F\x222B\x2202\x2206\x2208\x2209\x2205\x2229\x222A\x2282\x2283\x2200\x2203\x2220",
    L"$\x20AC\x00A3\x00A5\x20A9\x20B9\x20BD\x00A2\x20AB\x20BA\x20BF\x20B1",
    L"\x03B1\x03B2\x03B3\x03B4\x03B5\x03B6\x03B7\x03B8\x03B9\x03BA\x03BB\x03BC\x03BD\x03BE\x03BF\x03C0\x03C1\x03C3\x03C4\x03C5\x03C6\x03C7\x03C8\x03C9",
    L"\x2500\x2502\x250C\x2510\x2514\x2518\x251C\x2524\x252C\x2534\x253C\x2550\x2551\x2554\x2557\x255A\x255D\x2560\x2563\x2566\x2569\x256C"
};

static void pane_text_paste(HWND window, const std::wstring &text);

static BOOL CALLBACK charmap_window_proc(HWND window, LPARAM reference)
{
    CharacterMapWindow *search = reinterpret_cast<CharacterMapWindow *>(reference);
    wchar_t module[MAX_PATH];
    if (GetWindowModuleFileName(window, module, ARRAYSIZE(module)) == 0 ||
        lstrcmpi(PathFindFileName(module), L"charmap.exe") != 0) return TRUE;
    search->window = window;
    GetWindowThreadProcessId(window, &search->process);
    return FALSE;
}

static BOOL CALLBACK charmap_font_combo_proc(HWND control, LPARAM reference)
{
    CharacterMapFontSearch *search = reinterpret_cast<CharacterMapFontSearch *>(reference);
    wchar_t class_name[32];
    DWORD_PTR index = CB_ERR;
    if (GetClassName(control, class_name, ARRAYSIZE(class_name)) == 0 ||
        lstrcmpi(class_name, L"ComboBox") != 0 ||
        !SendMessageTimeout(control, CB_FINDSTRINGEXACT, (WPARAM)-1,
            reinterpret_cast<LPARAM>(search->font_name), SMTO_ABORTIFHUNG, 1000, &index) ||
        static_cast<LRESULT>(index) == CB_ERR) return TRUE;
    search->combo = control;
    search->index = static_cast<int>(index);
    return FALSE;
}

static BOOL charmap_select_wingdings(HWND window, const DWORD process_id)
{
    const wchar_t font_name[] = L"Wingdings";
    HANDLE process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE, FALSE, process_id);
    LPVOID remote_name;
    SIZE_T written = 0;
    BOOL selected = FALSE;
    if (!process) return FALSE;
    remote_name = VirtualAllocEx(process, nullptr, sizeof(font_name), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote_name && WriteProcessMemory(process, remote_name, font_name, sizeof(font_name), &written) &&
        written == sizeof(font_name)) {
        CharacterMapFontSearch search = { nullptr, remote_name, CB_ERR };
        EnumChildWindows(window, charmap_font_combo_proc, reinterpret_cast<LPARAM>(&search));
        if (search.combo && SendMessage(search.combo, CB_SETCURSEL, search.index, 0) != CB_ERR) {
            SendMessage(GetParent(search.combo), WM_COMMAND,
                MAKEWPARAM(GetDlgCtrlID(search.combo), CBN_SELCHANGE), reinterpret_cast<LPARAM>(search.combo));
            selected = TRUE;
        }
    }
    if (remote_name) VirtualFreeEx(process, remote_name, 0, MEM_RELEASE);
    CloseHandle(process);
    return selected;
}

static void symbols_open_character_map(HWND owner)
{
    CharacterMapWindow map = {};
    SHELLEXECUTEINFO execute = {};
    wchar_t executable[MAX_PATH];
    UINT length;
    EnumWindows(charmap_window_proc, reinterpret_cast<LPARAM>(&map));
    if (!map.window) {
        length = GetSystemDirectory(executable, ARRAYSIZE(executable));
        if (!length || length >= ARRAYSIZE(executable) || !PathAppend(executable, L"charmap.exe")) return;
        execute.cbSize = sizeof(execute);
        execute.fMask = SEE_MASK_NOCLOSEPROCESS;
        execute.lpVerb = L"open";
        execute.lpFile = executable;
        execute.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteEx(&execute)) {
            MessageBox(owner, L"Windows Character Map could not be opened.", L"ClipMaster", MB_OK | MB_ICONERROR);
            return;
        }
        if (execute.hProcess) {
            WaitForInputIdle(execute.hProcess, 5000);
            CloseHandle(execute.hProcess);
        }
        EnumWindows(charmap_window_proc, reinterpret_cast<LPARAM>(&map));
    }
    if (map.window) {
        charmap_select_wingdings(map.window, map.process);
        ShowWindow(map.window, SW_SHOW);
        SetForegroundWindow(map.window);
    }
}

static LRESULT CALLBACK symbol_button_proc(HWND window, UINT message, WPARAM parameter, LPARAM data,
    UINT_PTR identifier, DWORD_PTR reference)
{
    UNREFERENCED_PARAMETER(identifier);
    UNREFERENCED_PARAMETER(reference);
    if (message == WM_MOUSEWHEEL) return SendMessage(GetParent(window), message, parameter, data);
    return DefSubclassProc(window, message, parameter, data);
}

static void symbols_layout(HWND window, SymbolsPane *pane)
{
    if (pane->layout) return;
    pane->layout = true;
    RECT client;
    GetClientRect(window, &client);
    int margin = pane_scale(window, 16), gap = pane_scale(window, 6), size = pane_scale(window, 53);
    int available = max(1, client.right - margin * 2);
    int columns = max(1, (available + gap) / (size + gap));
    int top = margin;
    size_t symbol_index = 0;
    for (int category = 0; category < ARRAYSIZE(symbol_sets); category++) {
        pane_place(pane->headings[category], margin, top - pane->offset, available, pane_scale(window, 28));
        top += pane_scale(window, 34);
        int count = lstrlen(symbol_sets[category]);
        for (int index = 0; index < count; index++, symbol_index++) {
            int left = margin + index % columns * (size + gap);
            pane_place(pane->symbols[symbol_index].window, left,
                top + index / columns * (size + gap) - pane->offset,
                max(1, min(size, client.right - margin - left)), size);
        }
        top += ((count + columns - 1) / columns) * (size + gap) + margin;
    }
    int charmap_gap = pane_scale(window, 20), charmap_height = pane_scale(window, 40);
    pane_place(pane->charmap_button, margin, top + charmap_gap - pane->offset, available, charmap_height);
    pane->height = top + charmap_gap + charmap_height + margin;
    SCROLLINFO scroll = { sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL };
    scroll.nMax = max(0, pane->height - 1);
    scroll.nPage = client.bottom;
    scroll.nPos = pane->offset;
    SetScrollInfo(window, SB_VERT, &scroll, TRUE);
    int adjusted = GetScrollPos(window, SB_VERT);
    pane->layout = false;
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    if (adjusted != pane->offset) { pane->offset = adjusted; symbols_layout(window, pane); }
}

static void symbols_set_font(HWND window, SymbolsPane *pane)
{
    HFONT font = CreateFont(-MulDiv(24, GetDpiForWindow(window), 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Symbol");
    if (!font) return;
    for (const SymbolButton &button : pane->symbols)
        SendMessage(button.window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (pane->font) DeleteObject(pane->font);
    pane->font = font;
}

static LRESULT CALLBACK symbols_proc(HWND window, UINT message, WPARAM parameter, LPARAM data)
{
    SymbolsPane *pane = reinterpret_cast<SymbolsPane *>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (message == WM_CREATE) {
        pane = new(std::nothrow) SymbolsPane;
        if (!pane) return -1;
        SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pane));
        symbols_set_font(window, pane);
        for (int category = 0; category < ARRAYSIZE(symbol_sets); category++) {
            pane->headings.push_back(pane_control(window, L"STATIC", symbol_names[category], SS_LEFT | SS_CENTERIMAGE,
                3000 + category));
            for (int index = 0; symbol_sets[category][index]; index++) {
                wchar_t label[] = { symbol_sets[category][index], 0 };
                HWND button = pane_control(window, L"BUTTON", label, BS_PUSHBUTTON | WS_TABSTOP,
                    2000 + static_cast<int>(pane->symbols.size()));
                if (pane->font) SendMessage(button, WM_SETFONT, reinterpret_cast<WPARAM>(pane->font), TRUE);
                RECT text_margin = {};
                SendMessage(button, BCM_SETTEXTMARGIN, 0, reinterpret_cast<LPARAM>(&text_margin));
                SetWindowSubclass(button, symbol_button_proc, 1, 0);
                pane->symbols.push_back({ button, symbol_sets[category][index] });
            }
        }
        pane->charmap_button = pane_control(window, L"BUTTON", L"Open Character Map (default Wingdings)",
            BS_OWNERDRAW | WS_TABSTOP, 4000);
        return 0;
    }
    if (!pane) return DefWindowProc(window, message, parameter, data);
    switch (message) {
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *draw = reinterpret_cast<DRAWITEMSTRUCT *>(data);
        if (draw && draw->CtlID == 4000) {
            COLORREF background = (draw->itemState & ODS_SELECTED) ? RGB(190, 83, 25) : RGB(226, 116, 38);
            HBRUSH fill = CreateSolidBrush(background);
            HBRUSH border = CreateSolidBrush(RGB(164, 73, 24));
            RECT text = draw->rcItem;
            int saved = SaveDC(draw->hDC);
            FillRect(draw->hDC, &draw->rcItem, fill);
            FrameRect(draw->hDC, &draw->rcItem, border);
            InflateRect(&text, -pane_scale(window, 8), 0);
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, (draw->itemState & ODS_DISABLED) ? GetSysColor(COLOR_GRAYTEXT) : RGB(255, 255, 255));
            SelectObject(draw->hDC, GetStockObject(DEFAULT_GUI_FONT));
            DrawText(draw->hDC, L"Open Character Map (default Wingdings)", -1, &text,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (draw->itemState & ODS_FOCUS) {
                InflateRect(&text, -pane_scale(window, 2), -pane_scale(window, 2));
                DrawFocusRect(draw->hDC, &text);
            }
            RestoreDC(draw->hDC, saved);
            DeleteObject(fill);
            DeleteObject(border);
            return TRUE;
        }
        break;
    }
    case WM_SIZE:
        symbols_layout(window, pane); return 0;
    case WM_DPICHANGED_AFTERPARENT:
        symbols_set_font(window, pane);
        symbols_layout(window, pane);
        return 0;
    case WM_MOUSEWHEEL:
    case WM_VSCROLL: {
        SCROLLINFO scroll = { sizeof(scroll), SIF_ALL };
        GetScrollInfo(window, SB_VERT, &scroll);
        if (message == WM_MOUSEWHEEL) scroll.nPos -= GET_WHEEL_DELTA_WPARAM(parameter) / WHEEL_DELTA * pane_scale(window, 72);
        else switch (LOWORD(parameter)) {
            case SB_LINEUP: scroll.nPos -= pane_scale(window, 32); break;
            case SB_LINEDOWN: scroll.nPos += pane_scale(window, 32); break;
            case SB_PAGEUP: scroll.nPos -= scroll.nPage; break;
            case SB_PAGEDOWN: scroll.nPos += scroll.nPage; break;
            case SB_THUMBTRACK: scroll.nPos = scroll.nTrackPos; break;
            case SB_TOP: scroll.nPos = 0; break;
            case SB_BOTTOM: scroll.nPos = scroll.nMax; break;
        }
        scroll.fMask = SIF_POS;
        SetScrollInfo(window, SB_VERT, &scroll, TRUE);
        pane->offset = GetScrollPos(window, SB_VERT);
        symbols_layout(window, pane);
        return 0;
    }
    case WM_COMMAND: {
        int identifier = LOWORD(parameter);
        if (identifier == 4000) {
            symbols_open_character_map(window);
        } else if (identifier >= 2000 && identifier < 2000 + static_cast<int>(pane->symbols.size())) {
            wchar_t symbol[] = { pane->symbols[identifier - 2000].symbol, 0 };
            pane_text_paste(window, symbol);
        }
        return 0;
    }
    case WM_NCDESTROY:
        if (pane->font) DeleteObject(pane->font);
        delete pane;
        SetWindowLongPtr(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProc(window, message, parameter, data);
}

static void pane_layout(HWND window, PastePane *pane)
{
    if (pane->layout) return;
    pane->layout = true;
    RECT client;
    GetClientRect(window, &client);
    int margin = pane_scale(window, 12), gap = pane_scale(window, 12), size = pane_scale(window, 256);
    int width = max(1, min(size, client.right - margin * 2));
    int columns = max(1, (client.right - margin * 2 + gap) / (size + gap));
    int available = max(1, client.right - margin * 2);
    int button_width = min(pane_scale(window, 148), available);
    int button_height = pane_scale(window, 36);
    int status_top = margin + button_height + gap;
    int information_top = status_top + pane_scale(window, 32) + gap;
    int tiles_top = information_top + pane_scale(window, 44) + gap;
    pane_place(pane->extract_text, margin, margin, button_width, button_height);
    pane_place(pane->extracted, margin + button_width + gap, margin, max(1, available - button_width - gap), button_height);
    pane_place(pane->status, margin, status_top, available, pane_scale(window, 32));
    pane_place(pane->information, margin, information_top, available, pane_scale(window, 44));
    for (size_t index = 0; index < pane->tiles.size(); index++) {
        int left = margin + static_cast<int>(index % columns) * (size + gap);
        int top = tiles_top + static_cast<int>(index / columns) * (size + gap) - pane->offset;
        HWND tile = pane->tiles[index].window;
        pane_place(tile, left, top, width, size);
        pane_place(GetDlgItem(tile, 1), pane_scale(window, 8), pane_scale(window, 8), width - pane_scale(window, 16), pane_scale(window, 28));
        pane_place(GetDlgItem(tile, 2), pane_scale(window, 8), pane_scale(window, 44), width - pane_scale(window, 16), size - pane_scale(window, 56));
    }
    int rows = static_cast<int>((pane->tiles.size() + columns - 1) / columns);
    int handlers_top = tiles_top + rows * (size + gap);
    int title_height = pane_scale(window, 24), row_height = pane_scale(window, 32);
    int field_gap = pane_scale(window, 8), label_width = pane_scale(window, 44);
    int part = max(1, (available - label_width) / 4);
    int heading_top = handlers_top - pane->offset;
    int rows_top = heading_top + title_height + pane_scale(window, 34);
    int buttons_top = rows_top + pane->handler_count * row_height + field_gap;
    int button_gap = pane_scale(window, 8), handler_button = pane_scale(window, 120);
    pane_place(pane->handler_title, margin, heading_top, available, title_height);
    for (int column = 0; column < 3; column++) {
        int left = margin + label_width + column * (part + field_gap);
        int field_width = column == 2 ? max(1, margin + available - left) : part;
        pane_place(pane->handler_header[column], left, heading_top + title_height + pane_scale(window, 6), field_width, pane_scale(window, 24));
        for (int row = 0; row < 16; row++) {
            HWND edit = pane->handler_edit[row * 3 + column];
            pane_place(edit, left, rows_top + row * row_height, field_width, pane_scale(window, 26));
            ShowWindow(edit, row < pane->handler_count ? SW_SHOW : SW_HIDE);
        }
    }
    for (int row = 0; row < 16; row++) {
        pane_place(pane->handler_label[row], margin, rows_top + row * row_height + pane_scale(window, 3), label_width, pane_scale(window, 24));
        ShowWindow(pane->handler_label[row], row < pane->handler_count ? SW_SHOW : SW_HIDE);
    }
    pane_place(pane->handler_add, margin, buttons_top, handler_button, button_height);
    pane_place(pane->handler_remove, margin + handler_button + button_gap, buttons_top, handler_button, button_height);
    pane_place(pane->handler_save, margin + (handler_button + button_gap) * 2, buttons_top, handler_button, button_height);
    pane_place(pane->handler_status, margin + (handler_button + button_gap) * 3, buttons_top,
        max(1, available - (handler_button + button_gap) * 3), button_height);
    EnableWindow(pane->handler_add, pane->handler_count < 16);
    EnableWindow(pane->handler_remove, pane->handler_count > 3);
    pane->height = buttons_top + pane->offset + button_height + margin;
    SCROLLINFO scroll = { sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL };
    scroll.nMax = max(0, pane->height - 1);
    scroll.nPage = client.bottom;
    scroll.nPos = pane->offset;
    SetScrollInfo(window, SB_VERT, &scroll, TRUE);
    int adjusted = GetScrollPos(window, SB_VERT);
    pane->layout = false;
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    if (adjusted != pane->offset) { pane->offset = adjusted; pane_layout(window, pane); }
}

/* Scrolls the pane so that the control is fully visible. */
static void pane_reveal(HWND window, PastePane *pane, HWND control)
{
    RECT rect, client;
    GetWindowRect(control, &rect);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&rect), 2);
    GetClientRect(window, &client);
    int margin = pane_scale(window, 12), delta = 0;
    if (rect.top < 0) delta = rect.top - margin;
    else if (rect.bottom > client.bottom) delta = rect.bottom - client.bottom + margin;
    if (!delta) return;
    SCROLLINFO scroll = { sizeof(scroll), SIF_POS };
    scroll.nPos = pane->offset + delta;
    SetScrollInfo(window, SB_VERT, &scroll, TRUE);
    pane->offset = GetScrollPos(window, SB_VERT);
    pane_layout(window, pane);
}

static void pane_save_handlers(HWND window, PastePane *pane)
{
    OPTION_INFO candidate = option;
    candidate.config_handler_count = pane->handler_count;
    for (int row = 0; row < 16; row++) {
        auto &handler = candidate.config_handlers[row];
        if (row >= pane->handler_count) { ZeroMemory(&handler, sizeof(handler)); continue; }
        GetWindowText(pane->handler_edit[row * 3], handler.formats, ARRAYSIZE(handler.formats));
        GetWindowText(pane->handler_edit[row * 3 + 1], handler.pattern, ARRAYSIZE(handler.pattern));
        GetWindowText(pane->handler_edit[row * 3 + 2], handler.script, ARRAYSIZE(handler.script));
        if ((!*handler.formats) != (!*handler.script)) {
            SetWindowText(pane->handler_status, L"\uAC01 handler\uC5D0\uB294 \uB300\uC0C1 \uC11C\uC2DD\uACFC PowerShell \uCC98\uB9AC\uAC00 \uBAA8\uB450 \uD544\uC694\uD569\uB2C8\uB2E4.");
            SetFocus(pane->handler_edit[row * 3 + (*handler.formats ? 2 : 0)]);
            return;
        }
    }
    bool saved = SendMessage(GetParent(window), WM_PASTEPLUS_APPLY_HANDLERS, 0, reinterpret_cast<LPARAM>(&candidate)) != FALSE;
    SetWindowText(pane->handler_status, saved ? L"Handler\uB97C \uC800\uC7A5\uD588\uC2B5\uB2C8\uB2E4." :
        L"Handler\uB97C \uC800\uC7A5\uD558\uC9C0 \uBABB\uD588\uC2B5\uB2C8\uB2E4. \uB2E8\uCD95\uD0A4 \uCDA9\uB3CC\uACFC \uC800\uC7A5 \uAD8C\uD55C\uC744 \uD655\uC778\uD574 \uC8FC\uC138\uC694.");
}

static void pane_text_paste(HWND window, const std::wstring &text)
{
    if (text.empty()) return;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    void *destination = memory ? GlobalLock(memory) : nullptr;
    if (!destination) { if (memory) GlobalFree(memory); return; }
    CopyMemory(destination, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(memory);
    DATA_INFO *format = data_create_data(CF_UNICODETEXT, const_cast<wchar_t *>(L"UNICODE TEXT"), memory,
        static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)), FALSE, nullptr);
    if (!format) { GlobalFree(memory); return; }
    SendMessage(GetParent(window), WM_PASTEPLUS_PASTE, 0, reinterpret_cast<LPARAM>(format));
    data_free(format);
}

static bool pane_text_copy(HWND window, const std::wstring &text)
{
    if (text.empty()) return false;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
    void *destination = memory ? GlobalLock(memory) : nullptr;
    if (!destination) { if (memory) GlobalFree(memory); return false; }
    CopyMemory(destination, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(memory);
    DATA_INFO *format = data_create_data(CF_UNICODETEXT, const_cast<wchar_t *>(L"UNICODE TEXT"), memory,
        static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)), FALSE, nullptr);
    if (!format) { GlobalFree(memory); return false; }
    bool copied = SendMessage(GetParent(window), WM_PASTEPLUS_COPY, 0, reinterpret_cast<LPARAM>(format)) != FALSE;
    data_free(format);
    return copied;
}

static std::wstring clip_date_text(const FILETIME &stamp)
{
    SYSTEMTIME captured;
    wchar_t text[48] = L"";
    if (FileTimeToSystemTime(&stamp, &captured))
        wsprintf(text, L"%04d-%02d-%02d %02d:%02d:%02d", captured.wYear, captured.wMonth, captured.wDay,
            captured.wHour, captured.wMinute, captured.wSecond);
    return text;
}

static std::wstring edit_line_breaks(const std::wstring &text)
{
    std::wstring result;
    for (size_t index = 0; index < text.size(); index++) {
        if (text[index] == L'\r') continue;
        if (text[index] == L'\n') result += L"\r\n";
        else result += text[index];
    }
    return result;
}

/* Reads the next older image clip each time it is pressed; the first press uses the newest one. */
static void pane_extract_text(HWND window, PastePane *pane)
{
    if (pane->image_clips.empty()) { SetWindowText(pane->status, L"No image clip exists."); return; }
    if (pane->next_image >= pane->image_clips.size()) { SetWindowText(pane->status, L"No older image clip."); return; }
    DATA_INFO *clip = pane->image_clips[pane->next_image], *format = nullptr;
    for (DATA_INFO *candidate = clip->child; candidate; candidate = candidate->next)
        if (format_is_ocr_image(candidate)) { format = candidate; break; }
    pane->ocr_clip_time = clip->modified;
    pane->ocr_clip_number = ++pane->next_image;
    DATA_INFO *image = format ? data_item_copy(format, FALSE, FALSE, nullptr) : nullptr;
    if (!image) { SetWindowText(pane->status, L"Could not prepare the image for OCR."); return; }
    pane->ocr_running = true;
    EnableWindow(pane->extract_text, FALSE);
    SetWindowText(pane->status, L"Recognizing text...");
    try { std::thread(extract_text_worker, window, image).detach(); }
    catch (...) {
        pane->ocr_running = false;
        EnableWindow(pane->extract_text, TRUE);
        data_free(image);
        SetWindowText(pane->status, L"Could not start OCR.");
    }
}

static void pane_paste_image(HWND window, PastePane *pane, DATA_INFO *selected)
{
    DATA_INFO *item = data_create_item(nullptr, FALSE, nullptr);
    if (!item) {
        SendMessage(GetParent(window), WM_PASTEPLUS_PASTE, 0, reinterpret_cast<LPARAM>(selected));
        return;
    }
    DATA_INFO **tail = &item->child;
    if (pane->snapshot) for (DATA_INFO *format = pane->snapshot->child; format; format = format->next) {
        if (!format_is_ocr_image(format) && format->format != CF_PALETTE) continue;
        DATA_INFO *copy = data_item_copy(format, FALSE, FALSE, nullptr);
        if (!copy) continue;
        *tail = copy;
        tail = &copy->next;
    }
    if (item->child) SendMessage(GetParent(window), WM_PASTEPLUS_PASTE, 0, reinterpret_cast<LPARAM>(item));
    else SendMessage(GetParent(window), WM_PASTEPLUS_PASTE, 0, reinterpret_cast<LPARAM>(selected));
    data_free(item);
}

static LRESULT CALLBACK pane_proc(HWND window, UINT message, WPARAM parameter, LPARAM data)
{
    PastePane *pane = reinterpret_cast<PastePane *>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (message == WM_CREATE) {
        pane = new(std::nothrow) PastePane;
        if (!pane) return -1;
        SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pane));
        pane->information = pane_control(window, L"STATIC", L"", SS_LEFT, 100);
        pane->status = pane_control(window, L"STATIC", L"", SS_LEFT, 101);
        pane->extract_text = pane_control(window, L"BUTTON", L"Extract text", BS_OWNERDRAW | WS_TABSTOP, 102);
        pane->extracted = pane_control(window, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 103);
        pasteplus_button_style(pane->extract_text, PASTEPLUS_BUTTON_PRIMARY);
        pane->handler_count = min(16, max(3, option.config_handler_count));
        pane->handler_title = pane_control(window, L"STATIC", L"Paste handler", SS_LEFT, 2000);
        static const wchar_t *handler_titles[] = { L"\uB300\uC0C1", L"\uC870\uAC74", L"\uCC98\uB9AC (PowerShell)" };
        for (int column = 0; column < 3; column++)
            pane->handler_header[column] = pane_control(window, L"STATIC", handler_titles[column], SS_LEFT, 2001 + column);
        for (int row = 0; row < 16; row++) {
            wchar_t label[8];
            wsprintf(label, L"PH%d", row + 1);
            pane->handler_label[row] = pane_control(window, L"STATIC", label, SS_LEFT, 2010 + row);
            for (int column = 0; column < 3; column++) {
                const wchar_t *text = column == 0 ? option.config_handlers[row].formats :
                    column == 1 ? option.config_handlers[row].pattern : option.config_handlers[row].script;
                HWND edit = pane_control(window, L"EDIT", text, ES_AUTOHSCROLL | WS_TABSTOP, 2100 + row * 3 + column);
                SendMessage(edit, EM_SETLIMITTEXT, column == 0 ? 127 : column == 1 ? 255 : 2047, 0);
                pane->handler_edit[row * 3 + column] = edit;
            }
        }
        pane->handler_add = pane_control(window, L"BUTTON", L"Handler \uCD94\uAC00", BS_OWNERDRAW | WS_TABSTOP, 2200);
        pane->handler_remove = pane_control(window, L"BUTTON", L"\uB9C8\uC9C0\uB9C9 \uD589 \uC0AD\uC81C", BS_OWNERDRAW | WS_TABSTOP, 2201);
        pane->handler_save = pane_control(window, L"BUTTON", L"Handler \uC800\uC7A5", BS_OWNERDRAW | WS_TABSTOP, 2202);
        pane->handler_status = pane_control(window, L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, 2203);
        pasteplus_button_style(pane->handler_add, PASTEPLUS_BUTTON_NORMAL);
        pasteplus_button_style(pane->handler_remove, PASTEPLUS_BUTTON_NORMAL);
        pasteplus_button_style(pane->handler_save, PASTEPLUS_BUTTON_PRIMARY);
        return 0;
    }
    if (!pane) return DefWindowProc(window, message, parameter, data);
    switch (message) {
    case WM_SIZE:
    case WM_DPICHANGED_AFTERPARENT: pane_layout(window, pane); return 0;
        case WM_DRAWITEM:
            if (pasteplus_button_draw(reinterpret_cast<DRAWITEMSTRUCT *>(data))) return TRUE;
            break;
    case WM_MOUSEWHEEL:
    case WM_VSCROLL: {
        SCROLLINFO scroll = { sizeof(scroll), SIF_ALL };
        GetScrollInfo(window, SB_VERT, &scroll);
        if (message == WM_MOUSEWHEEL) scroll.nPos -= GET_WHEEL_DELTA_WPARAM(parameter) / WHEEL_DELTA * pane_scale(window, 72);
        else switch (LOWORD(parameter)) {
            case SB_LINEUP: scroll.nPos -= pane_scale(window, 32); break;
            case SB_LINEDOWN: scroll.nPos += pane_scale(window, 32); break;
            case SB_PAGEUP: scroll.nPos -= scroll.nPage; break;
            case SB_PAGEDOWN: scroll.nPos += scroll.nPage; break;
            case SB_THUMBTRACK: scroll.nPos = scroll.nTrackPos; break;
            case SB_TOP: scroll.nPos = 0; break;
            case SB_BOTTOM: scroll.nPos = scroll.nMax; break;
        }
        scroll.fMask = SIF_POS;
        SetScrollInfo(window, SB_VERT, &scroll, TRUE);
        pane->offset = GetScrollPos(window, SB_VERT);
        pane_layout(window, pane);
        return 0;
    }
    case WM_COMMAND: {
        int identifier = LOWORD(parameter);
        if (identifier >= 2100 && identifier < 2100 + 48) {
            if (HIWORD(parameter) == EN_SETFOCUS) pane_reveal(window, pane, reinterpret_cast<HWND>(data));
        } else if (identifier == 2200 || identifier == 2201) {
            pane->handler_count = min(16, max(3, pane->handler_count + (identifier == 2200 ? 1 : -1)));
            SetWindowText(pane->handler_status, L"");
            pane_layout(window, pane);
            pane_reveal(window, pane, pane->handler_save);
        } else if (identifier == 2202) {
            pane_save_handlers(window, pane);
        } else if (identifier == 102) {
            if (!pane->ocr_running) pane_extract_text(window, pane);
        } else if (identifier >= 1000 && identifier < 1000 + static_cast<int>(pane->tiles.size())) {
            FormatTile &tile = pane->tiles[identifier - 1000];
            if (tile.handler < 0 && format_is_ocr_image(tile.format)) pane_paste_image(window, pane, tile.format);
            else if (tile.handler < 0) SendMessage(GetParent(window), WM_PASTEPLUS_PASTE, 0, reinterpret_cast<LPARAM>(tile.format));
            else {
                std::wstring output, error;
                SetWindowText(pane->status, L"Processing...");
                UpdateWindow(pane->status);
                HCURSOR previous = SetCursor(LoadCursor(nullptr, IDC_WAIT));
                bool result = powershell_transform(option.config_handlers[tile.handler].pattern,
                    option.config_handlers[tile.handler].script, format_text(tile.format, nullptr, true), output, error);
                SetCursor(previous);
                SetWindowText(pane->status, result ? L"" : error.c_str());
                if (result) pane_text_paste(window, output);
            }
        }
        return 0;
    }
    case WM_PASTEPLUS_OCR_DONE: {
        OcrCompletion *completion = reinterpret_cast<OcrCompletion *>(data);
        pane->ocr_running = false;
        EnableWindow(pane->extract_text, !pane->image_clips.empty());
        if (completion) {
            wchar_t position[96];
            std::wstring message;
            wsprintf(position, L"Image clip %d of %d (", static_cast<int>(pane->ocr_clip_number),
                static_cast<int>(pane->image_clips.size()));
            message = std::wstring(position) + clip_date_text(pane->ocr_clip_time) + L"): ";
            if (!completion->text.empty()) {
                SetWindowText(pane->extracted, edit_line_breaks(completion->text).c_str());
                message += pane_text_copy(window, completion->text) ? L"text extracted and copied to the clipboard." :
                    L"text was extracted but could not be copied.";
            } else {
                SetWindowText(pane->extracted, L"");
                message += completion->error;
            }
            if (pane->ocr_clip_number < pane->image_clips.size()) message += L" Press Extract text again for an older image.";
            SetWindowText(pane->status, message.c_str());
            delete completion;
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        /* A read-only edit would be drawn gray; the extracted text box stays white like an input box. */
        if (reinterpret_cast<HWND>(data) == pane->extracted) {
            HDC context = reinterpret_cast<HDC>(parameter);
            SetTextColor(context, dark_mode_get_color(COLOR_WINDOWTEXT));
            SetBkColor(context, dark_mode_get_color(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(dark_mode_get_brush(COLOR_WINDOW));
        }
        break;
    case WM_NCDESTROY:
        pane_clear(pane);
        delete pane;
        SetWindowLongPtr(window, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProc(window, message, parameter, data);
}

static void pane_add_tile(HWND window, PastePane *pane, DATA_INFO *format, int handler,
    const std::vector<BYTE> &bytes)
{
    int identifier = 1000 + static_cast<int>(pane->tiles.size());
    HWND tile = pane_control(window, L"STATIC", L"", WS_BORDER | WS_CLIPCHILDREN, identifier);
    wchar_t title[256];
    if (handler >= 0) wsprintf(title, L"PH%d - %s", handler + 1, format->format_name);
    else lstrcpyn(title, format->format_name ? format->format_name : L"Format", ARRAYSIZE(title));
    HWND label = pane_control(tile, L"STATIC", title, SS_LEFT, 1);
    HBITMAP image = handler < 0 && format_is_ocr_image(format) ?
        create_image_preview(format, pane_scale(window, 232), pane_scale(window, 176)) : nullptr;
    HWND preview = pane_control(tile, image ? L"STATIC" : L"EDIT",
        image ? L"" : format_text(format, &bytes).c_str(),
        image ? SS_BITMAP | SS_CENTERIMAGE : ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_TABSTOP, 2);
    if (image) SendMessage(preview, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(image));
    SetWindowSubclass(label, format_tile_proc, 1, reinterpret_cast<DWORD_PTR>(window));
    SetWindowSubclass(preview, format_tile_proc, 1, reinterpret_cast<DWORD_PTR>(window));
    FormatTile entry = { format, handler, tile, preview, image,
        static_cast<DWORD>(bytes.size()), bytes.empty() ? 0 : bytes_hash(bytes) };
    pane->tiles.push_back(std::move(entry));
}

extern "C" void pasteplus_refresh(HWND window, DATA_INFO *item)
{
    PastePane *pane = reinterpret_cast<PastePane *>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (!pane) return;
    pane_clear(pane);
    pane->snapshot = item ? data_create_item(item->title, FALSE, nullptr) : nullptr;
    if (pane->snapshot) {
        pane->snapshot->modified = item->modified;
        DATA_INFO **tail = &pane->snapshot->child;
        for (DATA_INFO *source = item->child; source; source = source->next) {
            if (format_is_unavailable(source) && !format_has_handler(source)) continue;
            DATA_INFO *copy = data_item_copy(source, FALSE, FALSE, nullptr);
            if (!copy) continue;
            *tail = copy;
            tail = &copy->next;
        }
    }
    /* Image clips, newest first: "Extract text" starts with the current clip if it holds an image,
       otherwise with the newest clip that does, and each further press goes one image older. */
    for (DATA_INFO *clip = item; clip; clip = clip->next) {
        if (clip->type != TYPE_ITEM) continue;
        for (DATA_INFO *format = clip->child; format; format = format->next) {
            if (!format_is_ocr_image(format)) continue;
            pane->image_clips.push_back(clip);
            break;
        }
    }
    int total = 0, duplicate = 0, unavailable = 0, pasteable = 0;
    if (pane->snapshot) {
        for (DATA_INFO *format = pane->snapshot->child; format; format = format->next) {
            total++;
            if (option.config_force_unicode && (format->format == CF_TEXT || format->format == CF_OEMTEXT)) { duplicate++; continue; }
            if (format_is_unavailable(format)) {
                unavailable++;
                continue;
            }
            bool graphic = format->format == CF_BITMAP || format->format == CF_DIB || format->format == CF_DIBV5 ||
                format->format == CF_ENHMETAFILE || format->format == CF_METAFILEPICT;
            bool text = format_is_text(format);
            if (graphic || !text) {
                pane_add_tile(window, pane, format, -1, {});
                pasteable++;
                continue;
            }
            DWORD size = 0;
            BYTE *raw = format_data_to_bytes(format, &size);
            if (!raw) raw = clipboard_data_to_bytes(format, &size);
            std::vector<BYTE> bytes;
            if (raw && size) bytes.assign(raw, raw + size);
            mem_free(reinterpret_cast<void **>(&raw));
            ULONGLONG hash = bytes.empty() ? 0 : bytes_hash(bytes);
            bool same = false;
            if (!bytes.empty()) for (auto &previous : pane->tiles)
                if (previous.handler < 0 && previous.byte_size == bytes.size() && previous.byte_hash == hash) { same = true; break; }
            if (same) { duplicate++; continue; }
            pane_add_tile(window, pane, format, -1, bytes);
            pasteable++;
        }
        for (int index = 0; index < option.config_handler_count; index++) {
            if (!*option.config_handlers[index].formats || !*option.config_handlers[index].script) continue;
            for (DATA_INFO *format = pane->snapshot->child; format; format = format->next) {
                if (target_matches(option.config_handlers[index].formats, format)) {
                    pane_add_tile(window, pane, format, index, {});
                    break;
                }
            }
        }
    }
    wchar_t information[512], date[96] = L"";
    if (pane->snapshot) {
        SYSTEMTIME captured;
        if (FileTimeToSystemTime(&pane->snapshot->modified, &captured))
            wsprintf(date, L"%04d-%02d-%02d %02d:%02d:%02d", captured.wYear, captured.wMonth, captured.wDay,
                captured.wHour, captured.wMinute, captured.wSecond);
    }
    wsprintf(information, L"%s\r\nFormats: %d    Duplicates / Unicode aliases: %d    Pasteable: %d    Unavailable: %d",
        *date ? date : L"No captured clip", total, duplicate, pasteable, unavailable);
    SetWindowText(pane->information, information);
    EnableWindow(pane->extract_text, !pane->ocr_running && !pane->image_clips.empty());
    SetWindowText(pane->extracted, L"");
    if (pane->image_clips.empty()) SetWindowText(pane->status, L"No image clip exists.");
    else if (pane->image_clips[0] != item)
        SetWindowText(pane->status, (L"Extract text starts with the newest image clip (" +
            clip_date_text(pane->image_clips[0]->modified) + L").").c_str());
    else SetWindowText(pane->status, L"");
    pane_layout(window, pane);
}

extern "C" HWND pasteplus_create(HINSTANCE instance, HWND owner, int identifier)
{
    WNDCLASS descriptor = {};
    descriptor.hInstance = instance;
    descriptor.lpfnWndProc = pane_proc;
    descriptor.lpszClassName = L"ClipMasterPastePlus";
    descriptor.hCursor = LoadCursor(nullptr, IDC_ARROW);
    descriptor.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    if (!RegisterClass(&descriptor) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    return CreateWindowEx(WS_EX_CONTROLPARENT, descriptor.lpszClassName, L"", WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN,
        0, 0, 0, 0, owner, reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)), instance, nullptr);
}

extern "C" HWND pasteplus_symbols_create(HINSTANCE instance, HWND owner, int identifier)
{
    WNDCLASS descriptor = {};
    descriptor.hInstance = instance;
    descriptor.lpfnWndProc = symbols_proc;
    descriptor.lpszClassName = L"ClipMasterSymbols";
    descriptor.hCursor = LoadCursor(nullptr, IDC_ARROW);
    descriptor.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    if (!RegisterClass(&descriptor) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    return CreateWindowEx(WS_EX_CONTROLPARENT, descriptor.lpszClassName, L"", WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN,
        0, 0, 0, 0, owner, reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)), instance, nullptr);
}

static bool format_is_ocr_image(DATA_INFO *format)
{
    return format->format == CF_BITMAP || format->format == CF_DIB || format->format == CF_DIBV5 ||
        format->format == CF_ENHMETAFILE || format->format == CF_METAFILEPICT ||
        (format->format_name && (StrStrI(format->format_name, L"image/") ||
         !lstrcmpi(format->format_name, L"PNG") || !lstrcmpi(format->format_name, L"JPEG") ||
         !lstrcmpi(format->format_name, L"JFIF") || !lstrcmpi(format->format_name, L"GIF") ||
         !lstrcmpi(format->format_name, L"TIFF")));
}

static bool metafile_to_pixels(DATA_INFO *format, std::vector<BYTE> &pixels, UINT &width, UINT &height)
{
    HENHMETAFILE enhanced = nullptr;
    METAFILEPICT picture = {};
    if (format->format == CF_ENHMETAFILE) {
        ENHMETAHEADER header = {};
        enhanced = static_cast<HENHMETAFILE>(format->data);
        if (!GetEnhMetaFileHeader(enhanced, sizeof(header), &header)) return false;
        LONGLONG image_width = static_cast<LONGLONG>(header.rclBounds.right) - header.rclBounds.left;
        LONGLONG image_height = static_cast<LONGLONG>(header.rclBounds.bottom) - header.rclBounds.top;
        if (image_width < 1) image_width = 1;
        if (image_height < 1) image_height = 1;
        width = static_cast<UINT>(image_width > 4096 ? 4096 : image_width);
        height = static_cast<UINT>(image_height > 4096 ? 4096 : image_height);
    } else {
        METAFILEPICT *source = static_cast<METAFILEPICT *>(GlobalLock(format->data));
        if (!source) return false;
        picture = *source;
        GlobalUnlock(format->data);
        LONGLONG image_width = picture.xExt, image_height = picture.yExt;
        if (image_width < 0) image_width = -image_width;
        if (image_height < 0) image_height = -image_height;
        if (image_width < 1) image_width = 1;
        if (image_height < 1) image_height = 1;
        width = static_cast<UINT>(image_width > 4096 ? 4096 : image_width);
        height = static_cast<UINT>(image_height > 4096 ? 4096 : image_height);
    }
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC memory = bitmap ? CreateCompatibleDC(screen) : nullptr;
    HGDIOBJ previous = memory ? SelectObject(memory, bitmap) : nullptr;
    bool rendered = false;
    if (previous && previous != HGDI_ERROR) {
        PatBlt(memory, 0, 0, width, height, WHITENESS);
        if (enhanced) {
            RECT target = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
            rendered = PlayEnhMetaFile(memory, enhanced, &target) != FALSE;
        } else {
            SetMapMode(memory, MM_ANISOTROPIC);
            SetWindowExtEx(memory, max(1, abs(picture.xExt)), max(1, abs(picture.yExt)), nullptr);
            SetViewportExtEx(memory, static_cast<int>(width), static_cast<int>(height), nullptr);
            rendered = picture.hMF && PlayMetaFile(memory, picture.hMF);
        }
        if (rendered) pixels.assign(static_cast<BYTE *>(bits), static_cast<BYTE *>(bits) +
            static_cast<size_t>(width) * height * 4);
        SelectObject(memory, previous);
    }
    if (memory) DeleteDC(memory);
    if (bitmap) DeleteObject(bitmap);
    ReleaseDC(nullptr, screen);
    return rendered;
}

static bool image_to_pixels(DATA_INFO *format, std::vector<BYTE> &pixels, UINT &width, UINT &height)
{
    if (format->format == CF_ENHMETAFILE || format->format == CF_METAFILEPICT)
        return metafile_to_pixels(format, pixels, width, height);
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> source;
    if (format->format == CF_BITMAP) {
        if (FAILED(factory->CreateBitmapFromHBITMAP(static_cast<HBITMAP>(format->data), nullptr,
            WICBitmapUseAlpha, &bitmap))) return false;
        source = bitmap.Get();
    } else {
        BYTE *memory = static_cast<BYTE *>(GlobalLock(format->data));
        if (!memory) return false;
        DWORD available = static_cast<DWORD>(min(GlobalSize(format->data), format->size ? format->size : MAXDWORD));
        if (available > 256 * 1024 * 1024) { GlobalUnlock(format->data); return false; }
        std::vector<BYTE> encoded;
        if (format->format == CF_DIB || format->format == CF_DIBV5) {
            if (available < sizeof(BITMAPINFOHEADER)) { GlobalUnlock(format->data); return false; }
            BITMAPINFOHEADER *header = reinterpret_cast<BITMAPINFOHEADER *>(memory);
            if (header->biSize < sizeof(BITMAPINFOHEADER) || header->biSize > available) {
                GlobalUnlock(format->data);
                return false;
            }
            DWORD offset = header->biSize;
            if (header->biSize == sizeof(BITMAPINFOHEADER) && header->biCompression == BI_BITFIELDS)
                offset += 3 * sizeof(DWORD);
            else if (header->biSize == sizeof(BITMAPINFOHEADER) && header->biCompression == 6)
                offset += 4 * sizeof(DWORD);
            DWORD colors = header->biClrUsed;
            if (!colors && header->biBitCount <= 8) colors = 1U << header->biBitCount;
            if (colors > (available - min(offset, available)) / sizeof(RGBQUAD)) {
                GlobalUnlock(format->data);
                return false;
            }
            offset += colors * sizeof(RGBQUAD);
            encoded.resize(sizeof(BITMAPFILEHEADER) + available);
            BITMAPFILEHEADER file_header = {};
            file_header.bfType = 0x4D42;
            file_header.bfSize = static_cast<DWORD>(encoded.size());
            file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + offset;
            CopyMemory(encoded.data(), &file_header, sizeof(file_header));
            CopyMemory(encoded.data() + sizeof(file_header), memory, available);
        } else {
            encoded.assign(memory, memory + available);
        }
        GlobalUnlock(format->data);
        if (encoded.empty() || encoded.size() > MAXDWORD) return false;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromMemory(encoded.data(), static_cast<DWORD>(encoded.size()))) ||
            FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame))) return false;
        source = frame.Get();
    }
    if (FAILED(source->GetSize(&width, &height)) || !width || !height ||
        static_cast<ULONGLONG>(width) * height > 40000000ULL) return false;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
            nullptr, 0.0, WICBitmapPaletteTypeCustom))) return false;
    UINT stride = width * 4;
    pixels.resize(static_cast<size_t>(stride) * height);
    return SUCCEEDED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data()));
}

static HBITMAP create_image_preview(DATA_INFO *format, UINT max_width, UINT max_height)
{
    std::vector<BYTE> source;
    UINT source_width = 0, source_height = 0;
    if (!image_to_pixels(format, source, source_width, source_height)) return nullptr;
    double scale = min(1.0, min(static_cast<double>(max_width) / source_width,
        static_cast<double>(max_height) / source_height));
    UINT width = max(1U, static_cast<UINT>(source_width * scale));
    UINT height = max(1U, static_cast<UINT>(source_height * scale));
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *destination = nullptr;
    HBITMAP image = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &destination, nullptr, 0);
    if (!image || !destination) { if (image) DeleteObject(image); return nullptr; }
    BYTE *output = static_cast<BYTE *>(destination);
    for (UINT y = 0; y < height; y++) {
        UINT source_y = static_cast<UINT>((static_cast<ULONGLONG>(y) * source_height) / height);
        for (UINT x = 0; x < width; x++) {
            UINT source_x = static_cast<UINT>((static_cast<ULONGLONG>(x) * source_width) / width);
            CopyMemory(output + (static_cast<size_t>(y) * width + x) * 4,
                source.data() + (static_cast<size_t>(source_y) * source_width + source_x) * 4, 4);
        }
    }
    return image;
}

static bool ocr_runtime_available()
{
    HMODULE winrt = LoadLibrary(L"api-ms-win-core-winrt-l1-1-0.dll");
    HMODULE winrt_error = LoadLibrary(L"api-ms-win-core-winrt-error-l1-1-1.dll");
    if (winrt) FreeLibrary(winrt);
    if (winrt_error) FreeLibrary(winrt_error);
    return winrt != nullptr && winrt_error != nullptr;
}

static void extract_text_worker(HWND window, DATA_INFO *image)
{
    OcrCompletion *completion = new(std::nothrow) OcrCompletion;
    if (!completion) { data_free(image); return; }
    bool initialized = false;
    if (!ocr_runtime_available()) completion->error = L"OCR requires Windows 10 or later.";
    else try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        initialized = true;
        std::vector<BYTE> pixels;
        UINT width = 0, height = 0;
        if (!image_to_pixels(image, pixels, width, height)) completion->error = L"Could not decode the clipboard image.";
        else {
            auto engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
            if (!engine) {
                wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
                if (GetUserDefaultLocaleName(locale, ARRAYSIZE(locale)))
                    engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromLanguage(
                        winrt::Windows::Globalization::Language(locale));
            }
            if (!engine) completion->error = L"No OCR language is installed for the current Windows profile.";
            else {
                auto buffer = winrt::Windows::Security::Cryptography::CryptographicBuffer::CreateFromByteArray(
                    winrt::array_view<uint8_t const>(pixels.data(), pixels.size()));
                auto bitmap = winrt::Windows::Graphics::Imaging::SoftwareBitmap::CreateCopyFromBuffer(buffer,
                    winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8, width, height,
                    winrt::Windows::Graphics::Imaging::BitmapAlphaMode::Ignore);
                completion->text = engine.RecognizeAsync(bitmap).get().Text().c_str();
                if (completion->text.empty()) completion->error = L"No text was found in the image.";
            }
        }
    } catch (const winrt::hresult_error &error) {
        completion->error = error.message().c_str();
    } catch (...) {
        completion->error = L"OCR failed.";
    }
    if (initialized) winrt::uninit_apartment();
    data_free(image);
    if (!PostMessage(window, WM_PASTEPLUS_OCR_DONE, 0, reinterpret_cast<LPARAM>(completion))) delete completion;
}
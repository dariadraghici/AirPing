#define WIN32_LEAN_AND_MEAN
#define _USE_MATH_DEFINES // for M_PI
#include <windows.h>
#include <wincodec.h>
#include <string>
#include <cmath>
#include <thread>
#include <chrono>
#include <vector>
#include "overlay.h"
#include <objidl.h>
#include <gdiplus.h>
using namespace Gdiplus;

using namespace std;

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "gdiplus.lib")

static const int PLANE_WIDTH   = 264;
static const int PLANE_HEIGHT   = 132;
static const int BANNER_WIDTH  = 572;
static const int BANNER_HEIGHT = 132;
static const int GAP = 20; // between banner and plane
static const int TOTAL_WIDTH  = BANNER_WIDTH + GAP + PLANE_WIDTH;
static const int TOTAL_HEIGHT = PLANE_HEIGHT + 20;
static const int SCREEN_Y_POS_BASE = 80; // for the first row of planes
static const int PLANE_ROW_SPACING  = TOTAL_HEIGHT + 10;
static const int FPS = 60;
static const int DURATION_MS  = 15000; // 15 sec

static const int CURVE_N = 16;
static const float CURVE_DY[CURVE_N] = { // offset Y for the center of the banner at t=i/(CURVE_N-1)
    +2.3f, -9.8f, -14.3f, -16.1f, -15.5f, -13.2f, -9.2f, -4.0f,
    +2.9f,  +8.0f, +11.5f, +13.2f, +12.6f,  +8.0f,  -1.7f, -1.7f
};

static float BannerCurveY(float t) {
    float idx = t * (CURVE_N - 1);
    int i0 = (int)idx;
    int i1 = i0 + 1;
    if (i0 < 0)
        return CURVE_DY[0];
    if (i1 >= CURVE_N)
        return CURVE_DY[CURVE_N - 1];

    float frac = idx - i0;
    return CURVE_DY[i0] * (1.0f - frac) + CURVE_DY[i1] * frac; // linear interpolation
}

static float BannerCurveSlope(float t) { // numerical derivative of BannerCurveY(t) for slope at t, normalized by BANNER_WIDTH
    const float dt = 0.01f;
    float t0 = t - dt;
    if (t0 < 0.0f)
        t0 = 0.0f;

    float t1 = t + dt;
    if (t1 > 1.0f)
        t1 = 1.0f;

    return (BannerCurveY(t1) - BannerCurveY(t0)) / ((t1 - t0) * BANNER_WIDTH); // slope = dy/dx, dx = BANNER_WIDTH * (t1-t0)
}

static ULONG_PTR g_gdiplusToken = 0;

static void EnsureGdiplus() { // initialize GDI+ if not already done
    if (g_gdiplusToken)
        return;

    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdiplusToken, &gsi, nullptr);
}

static void DrawCurvedText(HDC hdcOut, const string& text, int bannerOffY) {
    EnsureGdiplus();

    Graphics g(hdcOut);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);

    const int FONT_SIZE = 18;
    const float BANNER_MARGIN = 30.0f; // lateral padding for text on the banner
    const float usableW = BANNER_WIDTH - BANNER_MARGIN;

    FontFamily fontFamily(L"Segoe UI");
    Gdiplus::Font font(&fontFamily, (REAL)FONT_SIZE, FontStyleBold, UnitPixel);

    StringFormat sf;
    sf.SetFormatFlags(StringFormatFlagsNoClip | StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);


    auto measureW = [&](const wstring& ws) -> float {// measure the width of a wstring using GDI+
        if (ws.empty())
            return 0.0f;

        RectF b;
        PointF o(0, 0);
        g.MeasureString(ws.c_str(), (int)ws.size(), &font, o, &sf, &b);
        return b.Width;
    };


    auto getLeadingPad = [&]() -> float { // measure the leading padding of the font by comparing "X" and "XX"
        float mX  = measureW(L"X");
        float mXX = measureW(L"XX");
        float advX = mXX - mX;
        return mX - advX;
    };

    float leadingPad = getLeadingPad();
    
    auto buildAdvances = [&](const wstring& ws) -> vector<float> { // build the advance width of each character in the wstring
        int n = (int)ws.size();
        vector<float> adv(n, 0.0f);

        for (int i = 0; i < n; i++) {
            wstring base = L"X";
            wstring test = L"X";
            test += ws[i];
            adv[i] = measureW(test) - measureW(base);
        }

        return adv;
    };

    float charH = 0.0f;
    {
        RectF b;
        PointF o(0, 0);
        g.MeasureString(L"Ag", 2, &font, o, &sf, &b); // A is tall and g is descender
        charH = b.Height;
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    vector<wchar_t> wbuf(wlen);
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wbuf.data(), wlen);
    wstring fullText(wbuf.data());

    for (auto& ch : fullText) {
        if (ch == L'\n')
            ch = L' ';
    }

    vector<wstring> wrappedLines;
    wrappedLines.push_back(fullText);

    if (wrappedLines.empty())
        return;

    float lineGap = charH * 0.1f;
    int nLines = (int)wrappedLines.size();
    float totalH = nLines * charH + (nLines - 1) * lineGap;
    float bannerCenterY = bannerOffY + BANNER_HEIGHT * 0.5f;

    for (int li = 0; li < nLines; li++) {
        const wstring& lineText = wrappedLines[li];
        if (lineText.empty())
            continue;

        int nc = (int)lineText.size();

        vector<float> advances = buildAdvances(lineText); // advance width of each character in the line

        float lineW = 0.0f;
        for (float a : advances)
            lineW += a;

        float startX = (BANNER_WIDTH - lineW) * 0.5f - leadingPad;
        float lineOffsetY = -totalH * 0.5f + li * (charH + lineGap) + charH * 0.5f;
        float curX = startX;

        for (int ci = 0; ci < nc; ci++) {
            wchar_t ch[2] = { lineText[ci], L'\0' }; // convert wchar_t to null-terminated string for DrawString
            float adv = advances[ci];

            float charCenterX = curX + leadingPad + adv * 0.5f;
            float t = charCenterX / BANNER_WIDTH;
            if (t < 0.0f)
                t = 0.0f;
            if (t > 1.0f)
                t = 1.0f;

            float dy = BannerCurveY(t);
            float slope = BannerCurveSlope(t);
            float angleDeg = (float)(atan((double)slope) * 180.0 / M_PI);

            float cx = charCenterX;
            float cy = bannerCenterY + dy + lineOffsetY;

            g.ResetTransform();
            g.TranslateTransform(cx, cy);
            g.RotateTransform(angleDeg);
            g.TranslateTransform(-leadingPad - adv * 0.5f, -charH * 0.5f); // position the character so that its center is at (0,0) for rotation

            SolidBrush shadowBrush(Color(180, 0, 0, 0));
            g.TranslateTransform(1.5f, 1.5f);
            g.DrawString(ch, 1, &font, PointF(0, 0), &sf, &shadowBrush);

            g.TranslateTransform(-1.5f, -1.5f);
            SolidBrush textBrush(Color(255, 255, 255, 255)); // white text
            g.DrawString(ch, 1, &font, PointF(0, 0), &sf, &textBrush);

            curX += adv;
        }
    }

    g.ResetTransform();
}

struct OverlayData {
    string bannerText;
    HWND hwnd; // handle to the overlay window
    int screenWidth;
    int screenHeight;
    float xPos;
    float speed;
    bool done;
    int yPos; // SCREEN_Y_POS_BASE + offset * PLANE_ROW_SPACING
};

static HBITMAP LoadPngAsAlphaBitmap(const wchar_t* path, int desiredWidth, int desiredHeight) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IWICImagingFactory* pFactory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
    if (FAILED(hr))
        return nullptr;

    IWICBitmapDecoder* pDecoder = nullptr;
    hr = pFactory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &pDecoder);
    if (FAILED(hr)) {
        pFactory->Release();
        return nullptr; 
    }

    IWICBitmapFrameDecode* pFrame = nullptr;
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr)) { 
        pDecoder->Release(); 
        pFactory->Release(); 
        return nullptr; 
    }

    IWICBitmapScaler* pScaler = nullptr;
    hr = pFactory->CreateBitmapScaler(&pScaler);
    if (FAILED(hr)) { 
        pFrame->Release(); 
        pDecoder->Release(); 
        pFactory->Release(); 
        return nullptr;
    }

    hr = pScaler->Initialize(pFrame, (UINT)desiredWidth, (UINT)desiredHeight, WICBitmapInterpolationModeFant); // scale the image to the desired size
    if (FAILED(hr)) {
        pScaler->Release();
        pFrame->Release();
        pDecoder->Release();
        pFactory->Release();
        return nullptr;
    }

    IWICFormatConverter* pConverter = nullptr;
    hr = pFactory->CreateFormatConverter(&pConverter); // convert to 32bppPBGRA format
    if (FAILED(hr)) {
        pScaler->Release();
        pFrame->Release();
        pDecoder->Release();
        pFactory->Release();
        return nullptr;
    }

    hr = pConverter->Initialize(pScaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut); // convert to 32bppPBGRA format
    if (FAILED(hr)) {
        pConverter->Release();
        pScaler->Release();
        pFrame->Release();
        pDecoder->Release();
        pFactory->Release();
        return nullptr;
    }

    BITMAPINFO bmi = {}; // create a DIB section to hold the pixel data
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = desiredWidth;
    bmi.bmiHeader.biHeight = -desiredHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB; // no compression (raw pixel data)

    void* pvBits = nullptr;
    HDC hdcScreen = GetDC(nullptr);
    HBITMAP hBmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pvBits, nullptr, 0); // create a DIB section to hold the pixel data
    ReleaseDC(nullptr, hdcScreen);

    if (!hBmp) {
        pConverter->Release();
        pScaler->Release();
        pFrame->Release();
        pDecoder->Release();
        pFactory->Release();
        return nullptr;
    }

    UINT stride = desiredWidth * 4; // number of bytes per row (4 bytes per pixel for 32bpp)
    UINT bufSize = stride * desiredHeight; // total size of the pixel buffer in bytes
    hr = pConverter->CopyPixels(nullptr, stride, bufSize, (BYTE*)pvBits);

    pConverter->Release();
    pScaler->Release();
    pFrame->Release();
    pDecoder->Release();
    pFactory->Release();

    if (FAILED(hr)) {
        DeleteObject(hBmp);
        return nullptr;
    }

    return hBmp;
}

static HBITMAP CompositeFrame(HDC hdcRef, const string& text) {
    int W = TOTAL_WIDTH;
    int H = TOTAL_HEIGHT;

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* last = wcsrchr(exePath, L'\\');
    if (last)
        *(last+1) = L'\0';
    else
        exePath[0] = L'\0';

    wchar_t planePath[MAX_PATH], bannerPath[MAX_PATH];
    wcscpy_s(planePath, exePath); 
    wcscat_s(planePath, L"images\\airplane.png");
    wcscpy_s(bannerPath, exePath); 
    wcscat_s(bannerPath, L"images\\banner.png");

    HBITMAP hBmpPlane = LoadPngAsAlphaBitmap(planePath, PLANE_WIDTH, PLANE_HEIGHT);
    HBITMAP hBmpBanner = LoadPngAsAlphaBitmap(bannerPath, BANNER_WIDTH, BANNER_HEIGHT);

    BITMAPINFO bmi = {}; // create a DIB section to hold the final composited image
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -H;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pvOut = nullptr; // pointer to the pixel data of the final composited image
    HBITMAP hOut = CreateDIBSection(hdcRef, &bmi, DIB_RGB_COLORS, &pvOut, nullptr, 0);
    if (!hOut) {
        if (hBmpPlane)
            DeleteObject(hBmpPlane);
        if (hBmpBanner)
            DeleteObject(hBmpBanner);
        return nullptr;
    }

    memset(pvOut, 0, W * H * 4);

    HDC hdcOut = CreateCompatibleDC(hdcRef);
    HBITMAP hOldOut = (HBITMAP)SelectObject(hdcOut, hOut);

    int bannerOffY = (H - BANNER_HEIGHT) / 2;
    int planeOffX = BANNER_WIDTH + GAP;
    int planeOffY = (H - PLANE_HEIGHT) / 2;

    if (hBmpBanner) { // if image was loaded successfully draw it onto the output bitmap with alpha blending
        HDC hdcSrc = CreateCompatibleDC(hdcRef);
        HBITMAP hOldSrc = (HBITMAP)SelectObject(hdcSrc, hBmpBanner);

        BLENDFUNCTION bf = {};
        bf.BlendOp = AC_SRC_OVER;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;
        AlphaBlend(hdcOut, 0, bannerOffY, BANNER_WIDTH, BANNER_HEIGHT, hdcSrc, 0, 0, BANNER_WIDTH, BANNER_HEIGHT, bf);

        SelectObject(hdcSrc, hOldSrc);
        DeleteDC(hdcSrc);
        DeleteObject(hBmpBanner);
    }

    if (hBmpPlane) { // if image was loaded successfully draw it onto the output bitmap with alpha blending
        HDC hdcSrc = CreateCompatibleDC(hdcRef);
        HBITMAP hOldSrc = (HBITMAP)SelectObject(hdcSrc, hBmpPlane);

        BLENDFUNCTION bf = {};
        bf.BlendOp = AC_SRC_OVER;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;
        AlphaBlend(hdcOut, planeOffX, planeOffY, PLANE_WIDTH, PLANE_HEIGHT, hdcSrc, 0, 0, PLANE_WIDTH, PLANE_HEIGHT, bf);

        SelectObject(hdcSrc, hOldSrc);
        DeleteDC(hdcSrc);
        DeleteObject(hBmpPlane);
    }

    int ropeX0 = BANNER_WIDTH;
    int ropeY0 = bannerOffY + BANNER_HEIGHT / 2;
    int ropeX1 = planeOffX;
    int ropeY1 = planeOffY + PLANE_HEIGHT / 2;

    HPEN hPenRope = CreatePen(PS_SOLID, 3, RGB(200, 50, 50));
    SelectObject(hdcOut, hPenRope);
    SelectObject(hdcOut, GetStockObject(NULL_BRUSH));
    MoveToEx(hdcOut, ropeX0, ropeY0, nullptr);
    LineTo(hdcOut, ropeX1, ropeY1);
    DeleteObject(hPenRope);

    DrawCurvedText(hdcOut, text, bannerOffY);

    DWORD* px = (DWORD*)pvOut;
    for (int i = 0; i < W * H; i++) {
        DWORD c = px[i];
        BYTE a = (BYTE)(c >> 24); 
        if (a == 0) {
            BYTE r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
            if (r != 0 || g != 0 || b != 0) {
                BYTE na = 220;
                px[i] = ((DWORD)na << 24) | ((DWORD)(r * na / 255) << 16) | ((DWORD)(g * na / 255) << 8) | ((DWORD)(b * na / 255)); // premultiply the RGB values by the new alpha value
            }
        }
    }

    SelectObject(hdcOut, hOldOut);
    DeleteDC(hdcOut);
    return hOut;
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    OverlayData* pData = reinterpret_cast<OverlayData*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    if (msg == WM_TIMER) { // update the position of the overlay window and redraw it
        if (!pData) {
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        
        int sw = GetSystemMetrics(SM_CXSCREEN);

        pData->xPos += pData->speed;
        int ix = (int)pData->xPos;

        if (ix > sw + TOTAL_WIDTH + 50) {
            pData->done = true;
            PostMessage(hwnd, WM_CLOSE, 0, 0);
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }

        SetWindowPos(hwnd, HWND_TOPMOST, ix, pData->yPos, TOTAL_WIDTH, TOTAL_HEIGHT, SWP_NOACTIVATE | SWP_SHOWWINDOW);

        HDC hdcScreen = GetDC(nullptr);
        HBITMAP hBmp  = CompositeFrame(hdcScreen, pData->bannerText);
        if (hBmp) {
            HDC hdcMem = CreateCompatibleDC(hdcScreen);
            HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBmp);

            POINT ptSrc = { 0, 0 };
            SIZE  szWnd = { TOTAL_WIDTH, TOTAL_HEIGHT };
            POINT ptDst = { ix, pData->yPos };

            BLENDFUNCTION bf = {};
            bf.BlendOp = AC_SRC_OVER;
            bf.SourceConstantAlpha = 255;
            bf.AlphaFormat = AC_SRC_ALPHA;

            UpdateLayeredWindow(hwnd, hdcScreen, &ptDst, &szWnd, hdcMem, &ptSrc, 0, &bf, ULW_ALPHA);

            SelectObject(hdcMem, hOld);
            DeleteDC(hdcMem);
            DeleteObject(hBmp);
        }
        ReleaseDC(nullptr, hdcScreen);
    }
    else if (msg == WM_CLOSE) { // close the overlay window and clean up resources
        KillTimer(hwnd, 1);
        DestroyWindow(hwnd);
    }
    else if (msg == WM_DESTROY) { // clean up the OverlayData and post a quit message to exit the message loop
        if (pData) {
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
            delete pData;
        }
        PostQuitMessage(0);
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}


void ShowPlaneOverlay(const string& bannerText, int yOffset) {
    HINSTANCE hInst = GetModuleHandle(nullptr);

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "GoogleCalendarOverlay";
    wc.hbrBackground = nullptr;
    RegisterClassExA(&wc); // register the window class for the overlay window

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int yPos = SCREEN_Y_POS_BASE + yOffset * PLANE_ROW_SPACING;

    OverlayData* pOvData = new OverlayData();
    pOvData->bannerText = bannerText;
    pOvData->screenWidth = sw;
    pOvData->screenHeight = GetSystemMetrics(SM_CYSCREEN);
    pOvData->xPos = (float)(-TOTAL_WIDTH - 10);
    pOvData->speed = (float)(sw + TOTAL_WIDTH + 100) / ((float)DURATION_MS / 1000.0f * FPS);
    pOvData->done = false;
    pOvData->yPos = yPos;

    HWND hwnd = CreateWindowExA(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "GoogleCalendarOverlay", "", WS_POPUP, (int)pOvData->xPos, yPos, TOTAL_WIDTH, TOTAL_HEIGHT, nullptr, nullptr, hInst, nullptr);

    if (!hwnd) {
        delete pOvData;
        return;
    }

    pOvData->hwnd = hwnd;
    SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pOvData));

    SetTimer(hwnd, 1, 1000 / FPS, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    MSG msg;
    while (IsWindow(hwnd) && GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

void ShowMultiplePlaneOverlays(const vector<string>& bannerTexts) {
    if (bannerTexts.empty())
        return;

    for (int i = 0; i < (int)bannerTexts.size(); ++i) { // show each overlay in a separate thread with a delay between them
        string text = bannerTexts[i];
        int yOffset = i;

        if (i > 0) {
            this_thread::sleep_for(chrono::milliseconds(150 * i)); // 150 ms delay between each overlay
        }

        thread([text, yOffset]() {
            ShowPlaneOverlay(text, yOffset);
        }).detach();
    }
}


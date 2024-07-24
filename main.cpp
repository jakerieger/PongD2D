#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

#define scast static_cast
#define dcast dynamic_cast
#define rcast reinterpret_cast

struct GameObject {
    D2D1_RECT_F Rect       = {};
    D2D1_COLOR_F Color     = {};
    D2D1_POINT_2F Position = {};
    D2D1_POINT_2F Rotation = {};
    D2D1_POINT_2F Scale    = {};

    virtual void Update()                              = 0;
    virtual void Draw(ID2D1RenderTarget* renderTarget) = 0;
    virtual ~GameObject()                              = default;
};

static HWND g_Hwnd;
static ID2D1Factory* g_Factory;
static ID2D1HwndRenderTarget* g_RenderTarget;
static std::unordered_map<std::string, GameObject*> g_GameObjects;

struct Ball final : GameObject {
    void Update() override {}

    void Draw(ID2D1RenderTarget* renderTarget) override {}
};

struct Paddle final : GameObject {
    void Update() override {}

    void Draw(ID2D1RenderTarget* renderTarget) override {}
};

void Initialize() {
    auto hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_Factory);
    if (FAILED(hr)) {
        MessageBox(nullptr, "D2D1CreateFactory failed!", "Error", MB_OK);
        return;
    }

    RECT rc;
    GetClientRect(g_Hwnd, &rc);
    hr = g_Factory->CreateHwndRenderTarget(
      D2D1::RenderTargetProperties(),
      D2D1::HwndRenderTargetProperties(g_Hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
      &g_RenderTarget);
    if (FAILED(hr)) {
        MessageBox(nullptr, "D2D1CreateFactory failed!", "Error", MB_OK);
    }
}

void Shutdown() {
    if (g_RenderTarget) {
        g_RenderTarget->Release();
        g_RenderTarget = nullptr;
    }

    if (g_Factory) {
        g_Factory->Release();
        g_Factory = nullptr;
    }
}

void Update() {
    for (const auto& go : g_GameObjects) {
        go->Update();
    }
}

void Frame() {
    if (g_RenderTarget) {
        g_RenderTarget->BeginDraw();
        g_RenderTarget->Clear(D2D1::ColorF(D2D1::ColorF::Black));

        // Draw game stuff here
        for (const auto& go : g_GameObjects) {
            go->Draw(g_RenderTarget);
        }

        if (const auto hr = g_RenderTarget->EndDraw(); FAILED(hr)) {
            MessageBox(nullptr, "EndDraw failed!", "Error", MB_OK);
            return;
        }
    }
}

void OnResize(const int w, const int h) {
    if (g_RenderTarget) {
        g_RenderTarget->Resize(D2D1::SizeU(w, h));
    }
}

void OnKeyDown(int keyCode) {
    if (keyCode == VK_ESCAPE) {
        ::PostQuitMessage(0);
    }
}
void OnKeyUp(int keyCode) {}
void OnMouseMove(int x, int y) {}
void OnMouseDown(int button, int state, int x, int y) {}
void OnMouseUp(int button, int state, int x, int y) {}

LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        case WM_SIZE: {
            RECT rc;
            ::GetClientRect(hwnd, &rc);
            const auto w = rc.right - rc.left;
            const auto h = rc.bottom - rc.top;
            OnResize(w, h);
        }
            return 0;
        case WM_KEYDOWN:
            OnKeyDown(scast<int>(wParam));
            return 0;
        case WM_KEYUP:
            OnKeyUp(scast<int>(wParam));
            return 0;
        case WM_MOUSEMOVE:
            OnMouseMove(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_LBUTTONDOWN:
            OnMouseDown(0, 0, 0, 0);
            return 0;
        case WM_LBUTTONUP:
            OnMouseUp(0, 0, 0, 0);
            return 0;
        case WM_RBUTTONDOWN:
            OnMouseDown(1, 0, 0, 0);
            return 0;
        case WM_RBUTTONUP:
            OnMouseUp(1, 0, 0, 0);
            return 0;
    }

    return ::DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Initialize the window class
    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(WNDCLASSEXA));
    wc.cbSize        = sizeof(WNDCLASSEXA);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "PongWindowClass";

    ::RegisterClassEx(&wc);

    // Create the window
    g_Hwnd = ::CreateWindowExA(0,
                               wc.lpszClassName,
                               "PongC++",
                               WS_OVERLAPPEDWINDOW,
                               300,
                               300,
                               800,
                               600,
                               nullptr,
                               nullptr,
                               hInstance,
                               nullptr);

    ::ShowWindow(g_Hwnd, nCmdShow);
    ::UpdateWindow(g_Hwnd);

    Initialize();

    // Enter the main loop
    MSG msg = {};
    for (;;) {
        Update();

        while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
        }

        if (msg.message == WM_QUIT)
            break;

        Frame();
    }

    Shutdown();

    return scast<int>(msg.wParam);
}
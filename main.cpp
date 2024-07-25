#include <Windows.h>
#include <codecvt>
#include <d2d1.h>
#include <dwrite.h>
#include <ranges>
#include <string>
#include <unordered_map>
#include <comdef.h>
#include <locale>
#include <utility>

#include "res/resource.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

static constexpr int kWindowWidth        = 1200;
static constexpr int kWindowHeight       = 900;
static constexpr bool kDrawBoundingBoxes = true;

namespace Map {
    constexpr auto Values = std::ranges::views::values;
    constexpr auto Keys   = std::ranges::views::keys;
}  // namespace Map

inline void WideToANSI(const std::wstring& value, std::string& converted) {
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    converted = converter.to_bytes(value);
}

inline void ANSIToWide(const std::string& value, std::wstring& converted) {
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    converted = converter.from_bytes(value);
}

class ComError final : public std::exception {
public:
    explicit ComError(std::string msg) : message(std::move(msg)) {}

    [[nodiscard]] const char* what() const noexcept override {
        return message.c_str();
    }

private:
    std::string message;
};

static void CheckResult(const HRESULT hr) {
    if (FAILED(hr)) {
        const _com_error err(hr);
        throw ComError(err.ErrorMessage());
    }
}

#define scast static_cast
#define dcast dynamic_cast
#define rcast reinterpret_cast

struct GameState {
    int PlayerScore   = 0;
    int OpponentScore = 0;
    int ElapsedTime   = 0;
};

static GameState g_GameState = {};

struct InputListener {
    virtual ~InputListener() = default;
    virtual void OnKeyDown(int keyCode) {}
    virtual void OnKeyUp(int keyCode) {}
    virtual void OnMouseMove(int mouseX, int mouseY) {}
    virtual void OnMouseDown(int mouseX, int mouseY) {}
    virtual void OnMouseUp(int mouseX, int mouseY) {}
};

struct GameObject {
    D2D1_RECT_F BoundingBox = {};
    D2D1_COLOR_F Color      = {};
    D2D1_POINT_2F Position  = {};
    D2D1_POINT_2F Rotation  = {};
    D2D1_POINT_2F Scale     = {};

    virtual void Start()                               = 0;
    virtual void Update(double dT)                     = 0;
    virtual void Draw(ID2D1RenderTarget* renderTarget) = 0;
    virtual ~GameObject()                              = default;

    void UpdateBoundingBox() {
        const auto top    = Position.y - Scale.y;
        const auto bottom = Position.y + Scale.y;
        const auto left   = Position.x - Scale.x;
        const auto right  = Position.x + Scale.x;

        const auto boundingBox = D2D1::RectF(left, top, right, bottom);
        BoundingBox            = boundingBox;
    }

    void DrawBoundingBox(ID2D1RenderTarget* renderTarget) const {
        ID2D1SolidColorBrush* boundsBrush = nullptr;
        CheckResult(
          renderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Red), &boundsBrush));

        renderTarget->DrawRectangle(BoundingBox, boundsBrush, 1);
        boundsBrush->Release();
    }
};

static HWND g_Hwnd;
static ID2D1Factory* g_Factory;
static ID2D1HwndRenderTarget* g_RenderTarget;
static IDWriteFactory* g_DWriteFactory;
static std::unordered_map<std::string, GameObject*> g_GameObjects;
static std::vector<InputListener*> g_InputListeners;

struct Ball final : GameObject {
    void Reset(const RECT& windowRect) {
        m_Velocity = {300.f, 0.f};
        Position   = D2D1::Point2F(windowRect.right / 2.f, windowRect.bottom / 2.f);
    }

    void Move(const float dT) {
        Position.x += m_Velocity.x * dT;
        Position.y += m_Velocity.y * dT;
    }

    void CheckCollision() {
        auto paddlePlayer   = g_GameObjects["Player"];
        auto paddleOpponent = g_GameObjects["Opponent"];
    }

    void CheckOOB(const RECT& windowRect) {
        if (Position.x < 0.0) {
            // Score and reset ball
            g_GameState.OpponentScore++;
            // Reset(windowRect);
            m_Velocity.x = -m_Velocity.x;
        } else if (Position.x > scast<float>(windowRect.right)) {
            // Score opponent and reset ball
            g_GameState.PlayerScore++;
            // Reset(windowRect);
            m_Velocity.x = -m_Velocity.x;
        }
    }

    void Start() override {
        RECT windowRect;
        ::GetClientRect(g_Hwnd, &windowRect);
        Reset(windowRect);
    }

    void Update(const double dT) override {
        RECT windowRect;
        ::GetClientRect(g_Hwnd, &windowRect);

        Move(scast<float>(dT));
        UpdateBoundingBox();
        CheckCollision();
        CheckOOB(windowRect);
    }

    void Draw(ID2D1RenderTarget* renderTarget) override {
        ID2D1SolidColorBrush* brush = nullptr;
        CheckResult(renderTarget->CreateSolidColorBrush(Color, &brush));

        renderTarget->FillEllipse(D2D1::Ellipse(Position, Scale.x, Scale.y), brush);
        brush->Release();
    }

private:
    D2D1_POINT_2F m_Velocity = {0.f, 0.f};
};

struct Paddle final : GameObject,
                      InputListener {
    explicit Paddle(const bool isAI) : m_IsAI(isAI) {}

    void Start() override {}

    void MoveAI() {
        auto ballPosition = g_GameObjects["Ball"]->Position;
        // Calculate paddle move-to location based on balls velocity and trajectory
    }

    void Update(double dT) override {
        if (m_IsAI) {
            MoveAI();
        }
    }

    void Draw(ID2D1RenderTarget* renderTarget) override {}

    void OnKeyDown(int keyCode) override {}

    void OnKeyUp(int keyCode) override {}

private:
    bool m_IsAI;
};

void Initialize() {
    auto hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_Factory);
    CheckResult(hr);

    RECT rc;
    GetClientRect(g_Hwnd, &rc);
    hr = g_Factory->CreateHwndRenderTarget(
      D2D1::RenderTargetProperties(),
      D2D1::HwndRenderTargetProperties(g_Hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
      &g_RenderTarget);
    CheckResult(hr);

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                             __uuidof(IDWriteFactory),
                             rcast<IUnknown**>(&g_DWriteFactory));
    CheckResult(hr);

    {  // Initialize the game objects
        const auto ball = new Ball;
        ball->Color     = D2D1::ColorF(D2D1::ColorF::White);
        ball->Position =
          D2D1::Point2F(scast<float>(rc.right - rc.left) / 2, scast<float>(rc.bottom - rc.top) / 2);
        ball->Scale = D2D1::Point2F(16, 16);

        const auto paddlePlayer = new Paddle(false);
        paddlePlayer->Color     = D2D1::ColorF(D2D1::ColorF::White);
        paddlePlayer->Position =
          D2D1::Point2F(scast<float>(rc.left) + 100, scast<float>(rc.bottom - rc.top) / 2);
        paddlePlayer->Scale = D2D1::Point2F(16, 200);

        const auto paddleOpponent = new Paddle(true);
        paddleOpponent->Color     = D2D1::ColorF(D2D1::ColorF::White);
        paddleOpponent->Position =
          D2D1::Point2F(scast<float>(rc.right) - 100, scast<float>(rc.bottom - rc.top) / 2);
        paddleOpponent->Scale = D2D1::Point2F(16, 200);

        g_InputListeners.push_back(paddlePlayer);
        g_GameObjects["Player"]   = paddlePlayer;
        g_GameObjects["Opponent"] = paddleOpponent;
        g_GameObjects["Ball"]     = ball;
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

void Start() {
    for (const auto& go : g_GameObjects | Map::Values) {
        go->Start();
    }
}

void Update(const double dT) {
    for (const auto& go : g_GameObjects | Map::Values) {
        go->Update(dT);
    }
}

void Frame() {
    if (g_RenderTarget) {
        g_RenderTarget->BeginDraw();
        g_RenderTarget->Clear(D2D1::ColorF(D2D1::ColorF::Black));

        // Draw game stuff here
        for (const auto& go : g_GameObjects | Map::Values) {
            go->Draw(g_RenderTarget);

            if constexpr (kDrawBoundingBoxes) {
                go->DrawBoundingBox(g_RenderTarget);
            }
        }

        CheckResult(g_RenderTarget->EndDraw());
    }
}

void OnResize(const int w, const int h) {
    if (g_RenderTarget) {
        CheckResult(g_RenderTarget->Resize(D2D1::SizeU(w, h)));
    }
}

void OnKeyDown(int keyCode) {
    if (keyCode == VK_ESCAPE) {
        ::PostQuitMessage(0);
    }

    for (const auto listener : g_InputListeners) {
        listener->OnKeyDown(keyCode);
    }
}

void OnKeyUp(int keyCode) {
    for (const auto listener : g_InputListeners) {
        listener->OnKeyUp(keyCode);
    }
}

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

namespace Timer {
    static LARGE_INTEGER g_Frequency;
    static LARGE_INTEGER g_LastTime;

    void StartTimer() {
        QueryPerformanceFrequency(&g_Frequency);
        QueryPerformanceCounter(&g_LastTime);
    }

    double GetDeltaTime() {
        LARGE_INTEGER currentTime;
        QueryPerformanceCounter(&currentTime);
        const LONGLONG elapsed = currentTime.QuadPart - g_LastTime.QuadPart;
        g_LastTime             = currentTime;
        return scast<double>(elapsed) / scast<double>(g_Frequency.QuadPart);
    }
}  // namespace Timer

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    HICON appIcon = ::LoadIcon(hInstance, MAKEINTRESOURCE(APPICON));

    // Initialize the window class
    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(WNDCLASSEXA));
    wc.cbSize        = sizeof(WNDCLASSEXA);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = appIcon;
    wc.hIconSm       = wc.hIcon;
    wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "PongWindowClass";

    ::RegisterClassEx(&wc);

    const int scrWidth  = ::GetSystemMetrics(SM_CXSCREEN);
    const int scrHeight = ::GetSystemMetrics(SM_CYSCREEN);
    const int posX      = (scrWidth - kWindowWidth) / 2;
    const int posY      = (scrHeight - kWindowHeight) / 2;

    // Create the window
    g_Hwnd = ::CreateWindowExA(0,
                               wc.lpszClassName,
                               "PongD2D",
                               WS_SYSMENU | WS_CAPTION | WS_MINIMIZEBOX,
                               posX,
                               posY,
                               kWindowWidth,
                               kWindowHeight,
                               nullptr,
                               nullptr,
                               hInstance,
                               nullptr);

    ::ShowWindow(g_Hwnd, nCmdShow);
    ::UpdateWindow(g_Hwnd);

    Initialize();

    // Enter the main loop
    MSG msg = {};
    Timer::StartTimer();
    Start();

    for (;;) {
        Update(Timer::GetDeltaTime());

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
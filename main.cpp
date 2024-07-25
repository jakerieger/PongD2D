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
#include <thread>

#include "res/resource.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

static constexpr int kWindowWidth        = 1200;
static constexpr int kWindowHeight       = 900;
static constexpr bool kDrawBoundingBoxes = false;
static constexpr float kBallSpeed        = 500.f;

static bool g_IsRunning = false;
static HWND g_Hwnd;

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

static RECT GetWindowRect(HWND hwnd) {
    RECT rect;
    ::GetClientRect(hwnd, &rect);
    return rect;
}

#define scast static_cast
#define dcast dynamic_cast
#define rcast reinterpret_cast

struct GameState {
    int PlayerScore   = 0;
    int OpponentScore = 0;
    int ScoreLimit    = 0;

    [[nodiscard]] int TotalScore() const {
        return PlayerScore + OpponentScore;
    }

    void Reset(const int scoreLimit) {
        PlayerScore   = 0;
        PlayerScore   = 0;
        OpponentScore = 0;
        ScoreLimit    = scoreLimit;
    }
};

struct KeyState {
    bool Pressed  = false;
    bool Released = false;
};

struct KeyEvent {
    int KeyCode;
};

struct MouseEvent {
    int Button;
};

struct MouseMoveEvent {
    double X;
    double Y;
};

struct InputListener {
    virtual ~InputListener() = default;
    virtual void OnKey(KeyEvent event) {}
    virtual void OnKeyDown(KeyEvent event) {}
    virtual void OnKeyUp(KeyEvent event) {}
    virtual void OnMouseMove(MouseMoveEvent event) {}
    virtual void OnMouseDown(MouseEvent event) {}
    virtual void OnMouseUp(MouseEvent event) {}
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

    virtual void Reset() {}

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
        try {
            CheckResult(
              renderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Red), &boundsBrush));
        } catch (ComError& err) {
            MessageBoxA(g_Hwnd, err.what(), "COM Error", MB_OK | MB_ICONERROR);
        }

        renderTarget->DrawRectangle(BoundingBox, boundsBrush, 1);
        boundsBrush->Release();
    }
};

static GameState g_GameState = {.ScoreLimit = 10};
static ID2D1Factory* g_Factory;
static ID2D1HwndRenderTarget* g_RenderTarget;
static IDWriteFactory* g_DWriteFactory;
static std::unordered_map<std::string, GameObject*> g_GameObjects;
static std::vector<InputListener*> g_InputListeners;
static std::unordered_map<int, KeyState> g_KeyStates;
std::thread g_InputDispatcherThread;

bool Overlaps(const D2D1_RECT_F& rectA, const D2D1_RECT_F& rectB) {
    if (rectA.right <= rectB.left || rectB.right <= rectA.left) {
        return false;
    }

    if (rectA.bottom <= rectB.top || rectB.bottom <= rectA.top) {
        return false;
    }

    return true;
}

struct Ball final : GameObject {
    void Reset(const RECT& windowRect) {
        GameObject::Reset();

        if (m_LastToScore == 0) {
            m_Velocity = {kBallSpeed, 0.f};
        } else {
            m_Velocity = {-kBallSpeed, 0.f};
        }
        Position = D2D1::Point2F(windowRect.right / 2.f, windowRect.bottom / 2.f);
    }

    void Move(const float dT) {
        Position.x += m_Velocity.x * dT;
        Position.y += m_Velocity.y * dT;
    }

    void CheckCollision() {
        const auto paddlePlayer   = g_GameObjects["Player"];
        const auto paddleOpponent = g_GameObjects["Opponent"];

        if (Overlaps(BoundingBox, paddlePlayer->BoundingBox)) {
            m_Velocity.x = -m_Velocity.x;
        }

        if (Overlaps(BoundingBox, paddleOpponent->BoundingBox)) {
            m_Velocity.x = -m_Velocity.x;
        }
    }

    void CheckOOB(const RECT& windowRect) {
        if (Position.x < 0.0) {
            // Score and reset ball
            g_GameState.OpponentScore++;
            m_LastToScore = 0;
            Reset(windowRect);
        } else if (Position.x > scast<float>(windowRect.right)) {
            // Score opponent and reset ball
            g_GameState.PlayerScore++;
            m_LastToScore = 1;
            Reset(windowRect);
        }
    }

    void Start() override {
        Reset(GetWindowRect(g_Hwnd));
    }

    void Update(const double dT) override {
        Move(scast<float>(dT));
        UpdateBoundingBox();
        CheckCollision();
        CheckOOB(GetWindowRect(g_Hwnd));
    }

    void Draw(ID2D1RenderTarget* renderTarget) override {
        ID2D1SolidColorBrush* brush = nullptr;
        try {
            CheckResult(renderTarget->CreateSolidColorBrush(Color, &brush));
        } catch (ComError& err) {
            MessageBoxA(g_Hwnd, err.what(), "COM Error", MB_OK | MB_ICONERROR);
        }

        renderTarget->FillEllipse(D2D1::Ellipse(Position, Scale.x, Scale.y), brush);
        brush->Release();
    }

private:
    D2D1_POINT_2F m_Velocity = {0.f, 0.f};
    int m_LastToScore        = 0;
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

        UpdateBoundingBox();
    }

    void Draw(ID2D1RenderTarget* renderTarget) override {
        ID2D1SolidColorBrush* brush = nullptr;
        try {
            CheckResult(renderTarget->CreateSolidColorBrush(Color, &brush));
        } catch (ComError& err) {
            MessageBoxA(g_Hwnd, err.what(), "COM Error", MB_OK | MB_ICONERROR);
        }
        renderTarget->FillRectangle(BoundingBox, brush);
        brush->Release();
    }

    void OnKey(const KeyEvent event) override {
        // TODO: Bug where arrow key and alpha key pressed together double paddle speed
        if (event.KeyCode == VK_UP || event.KeyCode == 'W') {
            Position.y -= 10.f;
        } else if (event.KeyCode == VK_DOWN || event.KeyCode == 'S') {
            Position.y += 10.f;
        }
    }

private:
    bool m_IsAI;
};

void InputDispatcher() {
    while (g_IsRunning) {
        for (const auto& [key, state] : g_KeyStates) {
            if (state.Pressed) {
                const KeyEvent event {key};
                for (const auto& listener : g_InputListeners) {
                    listener->OnKey(event);
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}

void Initialize() {
    auto hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_Factory);
    try {
        CheckResult(hr);
    } catch (ComError& err) {
        MessageBoxA(g_Hwnd, err.what(), "COM Error", MB_OK | MB_ICONERROR);
    }

    RECT rc;
    GetClientRect(g_Hwnd, &rc);
    hr = g_Factory->CreateHwndRenderTarget(
      D2D1::RenderTargetProperties(),
      D2D1::HwndRenderTargetProperties(g_Hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
      &g_RenderTarget);
    try {
        CheckResult(hr);
    } catch (ComError& err) {
        MessageBoxA(g_Hwnd, err.what(), "COM Error", MB_OK | MB_ICONERROR);
    }

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                             __uuidof(IDWriteFactory),
                             rcast<IUnknown**>(&g_DWriteFactory));
    try {
        CheckResult(hr);
    } catch (ComError& err) {
        MessageBoxA(g_Hwnd, err.what(), "COM Error", MB_OK | MB_ICONERROR);
    }

    g_InputDispatcherThread = std::thread(InputDispatcher);

    {  // Initialize the game objects
        const auto ball = new Ball;
        ball->Color     = D2D1::ColorF(D2D1::ColorF::White);
        ball->Position =
          D2D1::Point2F(scast<float>(rc.right - rc.left) / 2, scast<float>(rc.bottom - rc.top) / 2);
        ball->Scale = D2D1::Point2F(16, 16);

        const auto paddlePlayer = new Paddle(false);
        paddlePlayer->Color     = D2D1::ColorF(D2D1::ColorF::CornflowerBlue);
        paddlePlayer->Position =
          D2D1::Point2F(scast<float>(rc.left) + 100, scast<float>(rc.bottom - rc.top) / 2);
        paddlePlayer->Scale = D2D1::Point2F(16, 100);

        const auto paddleOpponent = new Paddle(true);
        paddleOpponent->Color     = D2D1::ColorF(0xED64A6);
        paddleOpponent->Position =
          D2D1::Point2F(scast<float>(rc.right) - 100, scast<float>(rc.bottom - rc.top) / 2);
        paddleOpponent->Scale = D2D1::Point2F(16, 100);

        g_InputListeners.push_back(paddlePlayer);
        g_GameObjects["Player"]   = paddlePlayer;
        g_GameObjects["Opponent"] = paddleOpponent;
        g_GameObjects["Ball"]     = ball;
    }
}

void Reset() {
    g_GameState.Reset(10);
    for (const auto& go : g_GameObjects | Map::Values) {
        go->Reset();
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

    g_InputDispatcherThread.join();
}

void Start() {
    for (const auto& go : g_GameObjects | Map::Values) {
        go->Start();
    }
}

void Update(const double dT) {
    if (g_GameState.TotalScore() >= g_GameState.ScoreLimit) {
        // Game is over, announce winner
        if (g_GameState.OpponentScore == g_GameState.PlayerScore) {
            // TIE
            MessageBoxA(g_Hwnd, "Game ended in a tie!", "Game Over", MB_OK);
        } else if (g_GameState.OpponentScore > g_GameState.PlayerScore) {
            // Opponent wins
            MessageBoxA(g_Hwnd, "You lost.", "Game Over", MB_OK);
        } else {
            // Player wins
            MessageBoxA(g_Hwnd, "You won!", "Game Over", MB_OK);
        }

        Reset();
    }

    for (const auto& go : g_GameObjects | Map::Values) {
        go->Update(dT);
    }
}

void Frame() {
    if (g_RenderTarget) {
        g_RenderTarget->BeginDraw();
        g_RenderTarget->Clear(D2D1::ColorF(0x11121C));

        // Draw game stuff here
        for (const auto& go : g_GameObjects | Map::Values) {
            go->Draw(g_RenderTarget);

            if constexpr (kDrawBoundingBoxes) {
                go->DrawBoundingBox(g_RenderTarget);
            }
        }

        try {
            CheckResult(g_RenderTarget->EndDraw());
        } catch (ComError& err) {
            MessageBoxA(g_Hwnd, err.what(), "COM Error", MB_OK | MB_ICONERROR);
        }
    }
}

void OnResize(const int w, const int h) {
    if (g_RenderTarget) {
        try {
            CheckResult(g_RenderTarget->Resize(D2D1::SizeU(w, h)));
        } catch (ComError& err) {
            MessageBoxA(g_Hwnd, err.what(), "COM Error", MB_OK | MB_ICONERROR);
        }
    }
}

void OnKeyDown(const int keyCode) {
    if (keyCode == VK_ESCAPE) {
        ::PostQuitMessage(0);
    }

    g_KeyStates[keyCode].Pressed  = true;
    g_KeyStates[keyCode].Released = false;

    for (const auto listener : g_InputListeners) {
        listener->OnKeyDown({keyCode});
    }
}

void OnKeyUp(const int keyCode) {
    g_KeyStates[keyCode].Pressed  = false;
    g_KeyStates[keyCode].Released = true;

    for (const auto listener : g_InputListeners) {
        listener->OnKeyUp({keyCode});
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
            auto [left, top, right, bottom] = GetWindowRect(hwnd);
            const auto w                    = right - left;
            const auto h                    = bottom - top;
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
        default:
            break;
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
    MSG msg     = {};
    g_IsRunning = true;
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

    g_IsRunning = false;
    Shutdown();

    return scast<int>(msg.wParam);
}
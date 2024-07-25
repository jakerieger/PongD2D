/*
 __   __   ___  __   __   __   __   ___  __   __   __   __
|__) |__) |__  |__) |__) /  \ /  ` |__  /__` /__` /  \ |__)
|    |  \ |___ |    |  \ \__/ \__, |___ .__/ .__/ \__/ |  \
 */
#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <xaudio2.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "xaudio2")

#include <codecvt>
#include <ranges>
#include <string>
#include <unordered_map>
#include <comdef.h>
#include <locale>
#include <utility>
#include <thread>
#include <format>

#include "res/resource.h"

static constexpr bool kDrawBoundingBoxes = false;

static float g_InitBallSpeed = 10.f;
static bool g_IsRunning      = false;
static HWND g_Hwnd;

/*
___      __   ___     __   ___  ___  __
 |  \ / |__) |__     |  \ |__  |__  /__`
 |   |  |    |___    |__/ |___ |    .__/
*/
namespace Map {
    constexpr auto Values = std::ranges::views::values;
    constexpr auto Keys   = std::ranges::views::keys;
}  // namespace Map

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

#define SCAST static_cast
#define DCAST dynamic_cast
#define RCAST reinterpret_cast
#define CCAST const_cast
#define CATCH_COM_EXCEPTION                                                                        \
    try {                                                                                          \
        CheckResult(hr);                                                                           \
    } catch (ComError & err) {                                                                     \
        MessageBoxA(g_Hwnd, err.what(), "COM Error", MB_OK | MB_ICONERROR);                        \
        if (g_Hwnd) {                                                                              \
            ::PostQuitMessage(0);                                                                  \
        } else {                                                                                   \
            exit(1);                                                                               \
        }                                                                                          \
    }

struct Vector2 {
    float X = 0;
    float Y = 0;

    Vector2 operator-() const {
        return {-X, -Y};
    }

    Vector2 operator*(const Vector2& rhs) const {
        return {X * rhs.X, Y * rhs.Y};
    }

    Vector2 operator*(const float scalar) const {
        return {X * scalar, Y * scalar};
    }

    static float Dot(const Vector2& lhs, const Vector2& rhs) {
        return lhs.X * rhs.X + lhs.Y * rhs.Y;
    }

    static Vector2 Reflect(const Vector2& velocity, const Vector2& normal) {
        const float dotProduct = Dot(velocity, normal);
        Vector2 reflection;
        reflection.X = velocity.X - 2 * dotProduct * normal.X;
        reflection.Y = velocity.Y - 2 * dotProduct * normal.Y;
        return reflection;
    }

    [[nodiscard]] D2D1_POINT_2F GetPoint() const {
        return D2D1::Point2F(X, Y);
    }
};

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
    Vector2 Position        = {};
    Vector2 Rotation        = {};
    Vector2 Size            = {};
    Vector2 Velocity        = {};

    virtual void Start()                               = 0;
    virtual void Update(double dT)                     = 0;
    virtual void Draw(ID2D1RenderTarget* renderTarget) = 0;
    virtual ~GameObject()                              = default;

    virtual void Reset() {};
    virtual void FixedUpdate() {};

    void UpdateBoundingBox() {
        const auto top    = Position.Y - Size.Y;
        const auto bottom = Position.Y + Size.Y;
        const auto left   = Position.X - Size.X;
        const auto right  = Position.X + Size.X;

        const auto boundingBox = D2D1::RectF(left, top, right, bottom);
        BoundingBox            = boundingBox;
    }

    void DrawBoundingBox(ID2D1RenderTarget* renderTarget) const {
        ID2D1SolidColorBrush* boundsBrush = nullptr;
        const auto hr =
          renderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Red), &boundsBrush);
        CATCH_COM_EXCEPTION;

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
std::thread g_FixedUpdateThread;

/*
      __        __   __   __
|__| |__  |    |__) |__  |__) /__`
|  | |___ |___ |    |___ |  \ .__/
*/

void FixedUpdate();

inline void WideToANSI(const std::wstring& value, std::string& converted) {
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    converted = converter.to_bytes(value);
}

inline void ANSIToWide(const std::string& value, std::wstring& converted) {
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    converted = converter.from_bytes(value);
}

bool Overlaps(const D2D1_RECT_F& rectA, const D2D1_RECT_F& rectB) {
    if (rectA.right <= rectB.left || rectB.right <= rectA.left) {
        return false;
    }

    if (rectA.bottom <= rectB.top || rectB.bottom <= rectA.top) {
        return false;
    }

    return true;
}

/*
 __              ___     __   __        ___  __  ___     __             __   __   ___  __
/ _`  /\   |\/| |__     /  \ |__)    | |__  /  `  |     /  ` |     /\  /__` /__` |__  /__`
\__> /~~\  |  | |___    \__/ |__) \__/ |___ \__,  |     \__, |___ /~~\ .__/ .__/ |___ .__/
*/

struct Ball final : GameObject {
    void Reset(const RECT& windowRect) {
        GameObject::Reset();
        m_Speed = g_InitBallSpeed;

        if (m_LastToScore == 1) {
            // TODO: Randomize Y velocity
            Velocity = {m_Speed, 0.f};
        } else {
            Velocity = {-m_Speed, 0.f};
        }
        Position = {windowRect.right / 2.f, windowRect.bottom / 2.f};
    }

    void Move() {
        Position.X += Velocity.X;
        Position.Y += Velocity.Y;
    }

    void CheckCollision() {
        const auto paddlePlayer   = g_GameObjects["Player"];
        const auto paddleOpponent = g_GameObjects["Opponent"];

        // TODO: Implement calculations for reflecting ball off paddle correctly
        if (Overlaps(BoundingBox, paddlePlayer->BoundingBox) ||
            Overlaps(BoundingBox, paddleOpponent->BoundingBox)) {
            Velocity = -Velocity;
            // increase ball speed
            Velocity.X *= 1.05;
            Velocity.Y *= 1.05;
        }
    }

    void CheckOOB(const RECT& windowRect) {
        if (Position.X < 0.0) {
            // Score and reset ball
            g_GameState.OpponentScore++;
            m_LastToScore = 0;
            Reset(windowRect);
        } else if (Position.X > SCAST<float>(windowRect.right)) {
            // Score opponent and reset ball
            g_GameState.PlayerScore++;
            m_LastToScore = 1;
            Reset(windowRect);
        }
    }

    void Start() override {
        Reset(GetWindowRect(g_Hwnd));
    }

    void FixedUpdate() override {
        UpdateBoundingBox();
        CheckCollision();
        CheckOOB(GetWindowRect(g_Hwnd));
        Move();
    }

    void Update(const double dT) override {}

    void Draw(ID2D1RenderTarget* renderTarget) override {
        ID2D1SolidColorBrush* brush = nullptr;
        const auto hr               = renderTarget->CreateSolidColorBrush(Color, &brush);
        CATCH_COM_EXCEPTION;

        renderTarget->FillEllipse(D2D1::Ellipse(Position.GetPoint(), Size.X, Size.Y), brush);
        brush->Release();
    }

private:
    int m_LastToScore = 0;
    float m_Speed     = g_InitBallSpeed;
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
        const auto hr               = renderTarget->CreateSolidColorBrush(Color, &brush);
        CATCH_COM_EXCEPTION;

        renderTarget->FillRectangle(BoundingBox, brush);
        brush->Release();
    }

    void OnKey(const KeyEvent event) override {
        // TODO: Bug where arrow key and alpha key pressed together double paddle speed
        if (event.KeyCode == VK_UP || event.KeyCode == 'W') {
            Position.Y -= 10.f;
            Velocity.Y = -100.f;
        } else if (event.KeyCode == VK_DOWN || event.KeyCode == 'S') {
            Position.Y += 10.f;
            Velocity.Y = 100.f;
        }
    }

private:
    bool m_IsAI;
};

struct GameText final : GameObject {
    void Start() override {
        auto hr = g_DWriteFactory->CreateTextFormat(L"Unispace",
                                                    nullptr,
                                                    DWRITE_FONT_WEIGHT_BOLD,
                                                    DWRITE_FONT_STYLE_NORMAL,
                                                    DWRITE_FONT_STRETCH_NORMAL,
                                                    40.f,
                                                    L"en-us",
                                                    &m_TextFormat);
        CATCH_COM_EXCEPTION;

        hr = m_TextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        CATCH_COM_EXCEPTION;

        hr = m_TextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        CATCH_COM_EXCEPTION;
    }

    void Update(double dT) override {
        const auto fmt = std::format("{} | {}", g_GameState.PlayerScore, g_GameState.OpponentScore);
        ANSIToWide(fmt, m_Text);
        UpdateBoundingBox();
    }

    void Draw(ID2D1RenderTarget* renderTarget) override {
        ID2D1SolidColorBrush* brush = nullptr;
        const auto hr               = renderTarget->CreateSolidColorBrush(Color, &brush);
        CATCH_COM_EXCEPTION;

        renderTarget->DrawTextA(m_Text.c_str(),
                                wcslen(m_Text.c_str()),
                                m_TextFormat,
                                D2D1::RectF(0, 0, Position.X, Position.Y),
                                brush);
    }

private:
    IDWriteTextFormat* m_TextFormat = nullptr;
    std::wstring m_Text;
};

/*
        ___  ___  __       __        ___           ___ ___       __   __   __
|    | |__  |__  /  ` \ / /  ` |    |__      |\/| |__   |  |__| /  \ |  \ /__`
|___ | |    |___ \__,  |  \__, |___ |___     |  | |___  |  |  | \__/ |__/ .__/
*/

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
    CATCH_COM_EXCEPTION;

    RECT rc;
    GetClientRect(g_Hwnd, &rc);
    hr = g_Factory->CreateHwndRenderTarget(
      D2D1::RenderTargetProperties(),
      D2D1::HwndRenderTargetProperties(g_Hwnd, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top)),
      &g_RenderTarget);
    CATCH_COM_EXCEPTION;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                             __uuidof(IDWriteFactory),
                             RCAST<IUnknown**>(&g_DWriteFactory));
    CATCH_COM_EXCEPTION;

    // Load UI font
    {}

    g_InputDispatcherThread = std::thread(InputDispatcher);
    g_FixedUpdateThread     = std::thread(FixedUpdate);

    {
        // Initialize the game objects
        const auto ball = new Ball;
        ball->Color     = D2D1::ColorF(D2D1::ColorF::White);
        ball->Position  = {SCAST<float>(rc.right - rc.left) / 2,
                           SCAST<float>(rc.bottom - rc.top) / 2};
        ball->Size      = {16, 16};

        const auto paddlePlayer = new Paddle(false);
        paddlePlayer->Color     = D2D1::ColorF(D2D1::ColorF::CornflowerBlue);
        paddlePlayer->Position  = {SCAST<float>(rc.left) + 100,
                                   SCAST<float>(rc.bottom - rc.top) / 2};
        paddlePlayer->Size      = {16, 100};

        const auto paddleOpponent = new Paddle(true);
        paddleOpponent->Color     = D2D1::ColorF(0xED64A6);
        paddleOpponent->Position  = {SCAST<float>(rc.right) - 100,
                                     SCAST<float>(rc.bottom - rc.top) / 2};
        paddleOpponent->Size      = {16, 100};

        const auto gameText = new GameText;
        gameText->Position  = {SCAST<float>(rc.right), 140.f};
        gameText->Size      = {32, 0};  // 16pt font, Y value not needed
        gameText->Color     = D2D1::ColorF(D2D1::ColorF::White);

        g_InputListeners.push_back(paddlePlayer);
        g_GameObjects["Player"]   = paddlePlayer;
        g_GameObjects["Opponent"] = paddleOpponent;
        g_GameObjects["Ball"]     = ball;
        g_GameObjects["GameText"] = gameText;
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
    g_FixedUpdateThread.join();
}

void Start() {
    for (const auto& go : g_GameObjects | Map::Values) {
        go->Start();
    }
}

void FixedUpdate() {
    while (g_IsRunning) {
        for (const auto& go : g_GameObjects | Map::Values) {
            go->FixedUpdate();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
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

        const auto hr = g_RenderTarget->EndDraw();
        CATCH_COM_EXCEPTION;
    }
}

void OnResize(const int w, const int h) {
    if (g_RenderTarget) {
        const auto hr = g_RenderTarget->Resize(D2D1::SizeU(w, h));
        CATCH_COM_EXCEPTION;
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
            OnKeyDown(SCAST<int>(wParam));
            return 0;
        case WM_KEYUP:
            OnKeyUp(SCAST<int>(wParam));
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
        return SCAST<double>(elapsed) / SCAST<double>(g_Frequency.QuadPart);
    }
}  // namespace Timer

/*
|\/|  /\  | |\ |
|  | /~~\ | | \|
*/
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

    // Create the window
    g_Hwnd = ::CreateWindowExA(0,
                               wc.lpszClassName,
                               "PongD2D",
                               WS_POPUP,
                               0,
                               0,
                               scrWidth,
                               scrHeight,
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

    constexpr int FPS        = 60;
    constexpr int frameDelay = 1000 / FPS;

    for (;;) {
        auto frameStart = std::chrono::high_resolution_clock::now();
        Update(Timer::GetDeltaTime());

        while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
        }

        if (msg.message == WM_QUIT)
            break;

        Frame();

        auto frameEnd = std::chrono::high_resolution_clock::now();
        std::chrono::duration<float, std::milli> frameDuration = frameEnd - frameStart;
        const auto fps                                         = 1000.f / frameDuration.count();
        const auto fmt = std::format("PongD2D | FPS: {:.2f}", fps);
        ::SetWindowTextA(g_Hwnd, fmt.c_str());
    }

    g_IsRunning = false;
    Shutdown();

    return SCAST<int>(msg.wParam);
}
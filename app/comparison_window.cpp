#include "comparison_window.h"
#include "fingerprint_image.h"
#include "kisa_api.h"

#include <commdlg.h>
#include <array>
#include <cwchar>

namespace {

constexpr wchar_t kWindowClass[] = L"FingerprintComparisonWindow";
constexpr int kChooseEnroll = 101;
constexpr int kChooseAttempt = 102;
constexpr int kCompare = 103;
constexpr int kTemplateCapacity = 1000000;

struct WindowState {
    std::wstring enrollPath;
    std::wstring attemptPath;
    HWND enrollField = nullptr;
    HWND attemptField = nullptr;
    HWND compareButton = nullptr;
    HWND status = nullptr;
};

HWND control(HWND parent, const wchar_t* kind, const wchar_t* text, DWORD style,
             int x, int y, int width, int height, int id = 0)
{
    HWND window = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)), nullptr);
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return window;
}

bool createControls(HWND window, WindowState& state)
{
    const auto enrollLabel = control(window, L"STATIC", L"등록용 지문", 0, 20, 20, 170, 20);
    state.enrollField = control(window, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL,
                               20, 42, 438, 26);
    const auto enrollButton = control(window, L"BUTTON", L"파일 선택…", WS_TABSTOP,
                                      470, 42, 108, 26, kChooseEnroll);
    const auto attemptLabel = control(window, L"STATIC", L"비교용 지문", 0, 20, 82, 170, 20);
    state.attemptField = control(window, L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_READONLY | ES_AUTOHSCROLL,
                                20, 104, 438, 26);
    const auto attemptButton = control(window, L"BUTTON", L"파일 선택…", WS_TABSTOP,
                                       470, 104, 108, 26, kChooseAttempt);
    state.compareButton = control(window, L"BUTTON", L"비교", WS_TABSTOP | WS_DISABLED | BS_PUSHBUTTON,
                                  20, 150, 108, 34, kCompare);
    state.status = control(window, L"STATIC", L"FVC TIFF 파일 두 장을 선택하세요.", 0,
                           20, 202, 558, 44);
    return enrollLabel && state.enrollField && enrollButton && attemptLabel
        && state.attemptField && attemptButton && state.compareButton && state.status;
}

void chooseFile(HWND owner, WindowState& state, bool enroll)
{
    std::array<wchar_t, 32768> path{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"TIFF 이미지 (*.tif;*.tiff)\0*.tif;*.tiff\0\0";
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrTitle = enroll ? L"등록용 지문 선택" : L"비교용 지문 선택";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) {
        if (CommDlgExtendedError() != 0) {
            SetWindowTextW(state.status, L"파일 선택 창을 열 수 없습니다.");
        }
        return;
    }

    (enroll ? state.enrollPath : state.attemptPath) = path.data();
    SetWindowTextW(enroll ? state.enrollField : state.attemptField, path.data());
    const bool ready = !state.enrollPath.empty() && !state.attemptPath.empty();
    EnableWindow(state.compareButton, ready);
    SetWindowTextW(state.status, ready ? L"비교 버튼을 누르세요." : L"다른 TIFF 파일도 선택하세요.");
}

std::wstring compareFiles(const WindowState& state)
{
    FingerprintImage enrolled;
    FingerprintImage attempt;
    std::wstring error;
    if (!loadFingerprintImage(state.enrollPath.c_str(), enrolled, error)) {
        return L"등록용 지문: " + error;
    }
    if (!loadFingerprintImage(state.attemptPath.c_str(), attempt, error)) {
        return L"비교용 지문: " + error;
    }

    std::vector<unsigned char> enrollTemplate(kTemplateCapacity);
    std::vector<unsigned char> attemptTemplate(kTemplateCapacity);
    int enrollSize = 0;
    int attemptSize = 0;
    int errorCode = 0;
    char message[256]{};
    double score = 0;
    if (KISA_Enroll(enrolled.type, enrolled.pixels.data(), enrollTemplate.data(),
                    &enrollSize, &errorCode, message) != 1) {
        return L"등록용 특징 추출 실패 (오류 " + std::to_wstring(errorCode) + L")";
    }
    if (KISA_Attempt(attempt.type, attempt.pixels.data(), attemptTemplate.data(),
                     &attemptSize, &errorCode, message) != 1) {
        return L"비교용 특징 추출 실패 (오류 " + std::to_wstring(errorCode) + L")";
    }
    if (KISA_Compare(enrollTemplate.data(), attemptTemplate.data(), &score, &errorCode, message) != 1) {
        return L"특징 비교 실패 (오류 " + std::to_wstring(errorCode) + L")";
    }

    wchar_t result[64]{};
    std::swprintf(result, std::size(result), L"초안 비교 점수 %.3f / 1.000", score);
    return result;
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE:
        state = static_cast<WindowState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return createControls(window, *state) ? 0 : -1;
    case WM_COMMAND:
        if (state && lParam && HIWORD(wParam) == BN_CLICKED) {
            switch (LOWORD(wParam)) {
            case kChooseEnroll:
            case kChooseAttempt:
                chooseFile(window, *state, LOWORD(wParam) == kChooseEnroll);
                return 0;
            case kCompare:
                if (IsWindowEnabled(state->compareButton)) {
                    const auto result = compareFiles(*state);
                    SetWindowTextW(state->status, result.c_str());
                }
                return 0;
            }
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int runComparisonWindow(HINSTANCE instance, int showCommand)
{
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    windowClass.lpszClassName = kWindowClass;

    WindowState state;
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT bounds{0, 0, 598, 256};
    AdjustWindowRect(&bounds, style, FALSE);
    HWND window = nullptr;
    if (RegisterClassW(&windowClass)) {
        window = CreateWindowExW(WS_EX_CONTROLPARENT, kWindowClass, L"지문 비교", style,
            CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, instance, &state);
    }
    if (!window) {
        MessageBoxW(nullptr, L"비교 창을 열 수 없습니다.", L"지문 비교", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(window, showCommand);
    MSG message{};
    BOOL received = 0;
    while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return received < 0 ? 1 : static_cast<int>(message.wParam);
}

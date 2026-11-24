#include "evaluation_window.h"
#include "evaluation/run.h"
#include "fvc_dataset.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <exception>
#include <thread>

namespace {

namespace fs = std::filesystem;
using evaluation::Settings;
constexpr wchar_t windowClass[] = L"FingerprintEvaluationWindow";
constexpr wchar_t title[] = L"지문 인식 성능 평가";
constexpr UINT_PTR timer = 1;

enum PathIndex { Dll, Database, Templates, Logs, Reports, PairFile, PathCount };
enum Id { EditPath = 100, BrowsePath = 110, Rule = 120, Value = 130, Seed = 140,
          Workers, Timeout, Accuracy, Failures, Timing, Start, Cancel, Report, Progress, Status };

std::wstring widen(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    UINT page = CP_UTF8;
    int count = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0);
    if (!count) {
        page = CP_ACP;
        count = MultiByteToWideChar(page, 0, text.data(), length, nullptr, 0);
    }
    std::wstring result(count, L'\0');
    if (count) {
        MultiByteToWideChar(page, 0, text.data(), length, result.data(), count);
    }
    return result;
}

fs::path executableDirectory()
{
    std::wstring buffer(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (count == 0 || count >= buffer.size()) {
        return fs::current_path();
    }
    buffer.resize(count);
    return fs::path(buffer).parent_path();
}

fs::path workspaceFrom(const fs::path& executable)
{
    for (auto root = executable; root.has_relative_path(); root = root.parent_path()) {
        if (fs::is_directory(root / "data" / "fvc")) {
            return root;
        }
    }
    return fs::current_path();
}

struct ReleaseCom {
    void operator()(IUnknown* value) const { value->Release(); }
};
template<class T>
using ComPtr = std::unique_ptr<T, ReleaseCom>;

fs::path chooseFolder(HWND owner)
{
    IFileOpenDialog* rawDialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&rawDialog)))) {
        throw std::runtime_error("Cannot open the folder picker");
    }
    const ComPtr<IFileOpenDialog> dialog(rawDialog);
    if (FAILED(dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST))
        || FAILED(dialog->SetTitle(L"폴더 선택"))) {
        throw std::runtime_error("Cannot configure the folder picker");
    }
    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return {};
    }
    IShellItem* rawItem = nullptr;
    if (FAILED(shown) || FAILED(dialog->GetResult(&rawItem))) {
        throw std::runtime_error("Cannot read the selected folder");
    }
    const ComPtr<IShellItem> item(rawItem);
    PWSTR rawPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath))) {
        throw std::runtime_error("Cannot read the selected folder path");
    }
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(rawPath, CoTaskMemFree);
    return fs::path(path.get());
}

fs::path chooseFile(HWND owner, const wchar_t* filter)
{
    std::array<wchar_t, 32768> path{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) {
        return fs::path(path.data());
    }
    if (CommDlgExtendedError() != 0) {
        throw std::runtime_error("Cannot open the file picker");
    }
    return {};
}

class EvaluationWindow {
public:
    EvaluationWindow() : cancel_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
        if (!cancel_) {
            throw std::runtime_error("Cannot create the evaluation cancellation event");
        }
    }

    ~EvaluationWindow()
    {
        if (run_.joinable()) {
            SetEvent(cancel_.get());
            run_.join();
        }
    }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE) {
            auto* self = static_cast<EvaluationWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<EvaluationWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self) {
            try {
                return self->dispatch(message, wParam, lParam);
            } catch (const std::exception& error) {
                MessageBoxW(window, widen(error.what()).c_str(), title, MB_OK | MB_ICONERROR);
                return message == WM_CREATE ? -1 : 0;
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

private:
    HWND control(const wchar_t* kind, const wchar_t* text, DWORD style, int x, int y, int width, int height,
                 int id = 0, DWORD extended = 0)
    {
        const auto handle = CreateWindowExW(extended, kind, text, style | WS_CHILD | WS_VISIBLE,
            x, y, width, height, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window_, GWLP_HINSTANCE)), nullptr);
        if (!handle) {
            throw std::runtime_error("Cannot create an evaluation control");
        }
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return handle;
    }

    HWND get(int id) const { return GetDlgItem(window_, id); }

    void set(int id, const std::wstring& value) { SetWindowTextW(get(id), value.c_str()); }

    std::wstring text(int id) const
    {
        std::wstring value(GetWindowTextLengthW(get(id)) + 1, L'\0');
        const int length = GetWindowTextW(get(id), value.data(), static_cast<int>(value.size()));
        value.resize(length);
        return value;
    }

    void number(const wchar_t* label, int id, const wchar_t* initial, int x, int y)
    {
        control(L"STATIC", label, 0, x, y, 120, 20);
        control(L"EDIT", initial, WS_TABSTOP | ES_NUMBER, x + 122, y - 3, 78, 24, id, WS_EX_CLIENTEDGE);
    }

    void createControls()
    {
        control(L"STATIC", L"시험 대상 및 저장 위치", 0, 20, 18, 680, 22);
        const wchar_t* fields[PathCount] = {L"알고리즘 DLL", L"테스트 DB 폴더", L"알고리즘 생성물 폴더",
                                           L"로그 폴더", L"보고서 폴더", L"부분 평가 쌍 CSV (선택)"};
        for (int i = 0; i < PathCount; ++i) {
            const int y = 47 + 34 * i;
            control(L"STATIC", fields[i], 0, 20, y + 4, 168, 22);
            control(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 192, y, 529, 25, EditPath + i, WS_EX_CLIENTEDGE);
            control(L"BUTTON", L"찾기…", WS_TABSTOP, 733, y, 86, 25, BrowsePath + i);
        }

        control(L"STATIC", L"시험 조합", 0, 20, 260, 170, 22);
        control(L"STATIC", L"규칙", 0, 192, 260, 180, 20);
        control(L"STATIC", L"촬영 번호 / 표본 수", 0, 403, 260, 170, 20);
        const wchar_t* ruleLabels[] = {L"등록용 지문", L"본인 비교 (등록당)", L"타인 비교 (타인별)"};
        const wchar_t* rules[] = {L"전체", L"첫 번째", L"지정 촬영 번호", L"무작위 표본"};
        for (int i = 0; i < 3; ++i) {
            const int y = 285 + 32 * i;
            control(L"STATIC", ruleLabels[i], 0, 20, y + 3, 160, 20);
            const auto combo = control(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                       192, y, 195, 130, Rule + i);
            for (const auto* rule : rules) {
                SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(rule));
            }
            SendMessageW(combo, CB_SETCURSEL, 0, 0);
            control(L"EDIT", L"1", WS_TABSTOP | ES_NUMBER, 403, y, 78, 24, Value + i, WS_EX_CLIENTEDGE);
        }

        number(L"무작위 시드", Seed, L"42", 20, 396);
        number(L"동시 워커 (1~16)", Workers, L"1", 268, 396);
        number(L"쌍 타임아웃 (초)", Timeout, L"30", 525, 396);
        control(L"STATIC", L"보고서 항목", 0, 20, 444, 140, 20);
        control(L"BUTTON", L"인식 지표 (EER, FMR/FNMR)", WS_TABSTOP | BS_AUTOCHECKBOX,
                192, 439, 250, 24, Accuracy);
        control(L"BUTTON", L"실패율 (FTE, REJ)", WS_TABSTOP | BS_AUTOCHECKBOX, 444, 439, 165, 24, Failures);
        control(L"BUTTON", L"시간 및 템플릿 크기", WS_TABSTOP | BS_AUTOCHECKBOX, 610, 439, 210, 24, Timing);
        for (int id : {Accuracy, Failures, Timing}) {
            SendMessageW(get(id), BM_SETCHECK, BST_CHECKED, 0);
        }

        control(PROGRESS_CLASSW, L"", 0, 20, 487, 799, 14, Progress);
        control(L"STATIC", L"DLL과 DB를 선택한 뒤 평가를 시작하세요.", 0, 20, 516, 799, 24, Status);
        control(L"BUTTON", L"평가 시작", WS_TABSTOP | BS_DEFPUSHBUTTON, 20, 556, 125, 34, Start);
        control(L"BUTTON", L"중단", WS_TABSTOP | WS_DISABLED, 157, 556, 100, 34, Cancel);
        control(L"BUTTON", L"보고서 열기", WS_TABSTOP | WS_DISABLED, 269, 556, 128, 34, Report);

        const auto binary = executableDirectory();
        const auto workspace = workspaceFrom(binary);
        if (fs::is_regular_file(workspace / "app" / "evaluation" / "run.cpp")) {
            latestReport_ = workspace / "reports" / "fingerprint_evaluation.html";
        }
        const auto library = binary / "IndustryAlgorithm.dll";
        const auto database = workspace / "data" / "fvc";
        if (fs::is_regular_file(library)) {
            set(EditPath + Dll, library.wstring());
        }
        if (fs::is_directory(database)) {
            set(EditPath + Database, database.wstring());
        }
        const auto output = workspace / "build" / "app-results";
        set(EditPath + Templates, (output / "templates").wstring());
        set(EditPath + Logs, (output / "logs").wstring());
        set(EditPath + Reports, (output / "reports").wstring());
    }

    evaluation::Selection selection(int index) const
    {
        const auto rule = static_cast<evaluation::Rule>(SendMessageW(get(Rule + index), CB_GETCURSEL, 0, 0));
        unsigned value = 1;
        if (rule == evaluation::Rule::Impression || rule == evaluation::Rule::Random) {
            value = fvc::parseInteger<unsigned>(fs::path(text(Value + index)).u8string());
            if (value == 0) {
                throw std::runtime_error("The selected rules require a positive impression number or sample count");
            }
        }
        return {rule, value};
    }

    Settings settings() const
    {
        Settings result;
        result.dll = text(EditPath + Dll);
        result.database = text(EditPath + Database);
        result.templates = text(EditPath + Templates);
        result.logs = text(EditPath + Logs);
        result.reports = text(EditPath + Reports);
        result.latestReport = latestReport_;
        result.pairFile = text(EditPath + PairFile);
        if (result.pairFile.empty()) {
            result.enrollment = selection(0);
            result.genuine = selection(1);
            result.impostor = selection(2);
        }
        result.seed = fvc::parseInteger<std::uint32_t>(fs::path(text(Seed)).u8string());
        result.workers = fvc::parseInteger<unsigned>(fs::path(text(Workers)).u8string());
        const auto seconds = fvc::parseInteger<unsigned>(fs::path(text(Timeout)).u8string());
        if (seconds == 0 || seconds > 3600) {
            throw std::runtime_error("Set a timeout from 1 to 3600 seconds");
        }
        result.timeoutMilliseconds = seconds * 1000;
        result.reportAccuracy = SendMessageW(get(Accuracy), BM_GETCHECK, 0, 0) == BST_CHECKED;
        result.reportFailures = SendMessageW(get(Failures), BM_GETCHECK, 0, 0) == BST_CHECKED;
        result.reportTiming = SendMessageW(get(Timing), BM_GETCHECK, 0, 0) == BST_CHECKED;
        return result;
    }

    void start()
    {
        if (run_.joinable()) {
            return;
        }
        const auto config = settings();
        result_ = {};
        error_ = nullptr;
        finished_ = false;
        progress_.total = 0;
        progress_.finished = 0;
        ResetEvent(cancel_.get());
        run_ = std::thread([this, config]() {
            try {
                result_ = evaluation::run(config, cancel_.get(), progress_);
            } catch (...) {
                error_ = std::current_exception();
            }
            finished_ = true;
        });
        EnableWindow(get(Start), FALSE);
        EnableWindow(get(Cancel), TRUE);
        EnableWindow(get(Report), FALSE);
        SendMessageW(get(Progress), PBM_SETRANGE32, 0, 1000);
        SendMessageW(get(Progress), PBM_SETPOS, 0, 0);
        set(Status, L"시험 조합 준비 중…");
    }

    void update()
    {
        if (!run_.joinable()) {
            return;
        }
        const auto total = progress_.total.load();
        const auto done = progress_.finished.load();
        if (total != 0) {
            SendMessageW(get(Progress), PBM_SETPOS, static_cast<WPARAM>(1000.0 * done / total), 0);
            const bool stopping = WaitForSingleObject(cancel_.get(), 0) == WAIT_OBJECT_0;
            set(Status, std::wstring(stopping ? L"중단 중 · " : L"평가 중 · ")
                      + std::to_wstring(done) + L" / " + std::to_wstring(total) + L"쌍");
        }
        if (!finished_) {
            return;
        }
        run_.join();
        if (closing_) {
            DestroyWindow(window_);
            return;
        }
        EnableWindow(get(Start), TRUE);
        EnableWindow(get(Cancel), FALSE);
        if (error_) {
            set(Status, L"평가 오류");
            std::rethrow_exception(error_);
        }
        set(Status, std::wstring(result_.cancelled ? L"중단 · " : L"완료 · ")
                  + L"처리 " + std::to_wstring(result_.completed)
                  + L"쌍 · 실패 " + std::to_wstring(result_.failed) + L"쌍");
        EnableWindow(get(Report), TRUE);
        if (!result_.reportPublishError.empty()) {
            const auto message = L"최신 평가 보고서를 갱신하지 못했습니다.\n"
                + latestReport_.wstring() + L"\n\n" + widen(result_.reportPublishError);
            MessageBoxW(window_, message.c_str(), title, MB_OK | MB_ICONWARNING);
        }
    }

    void command(int id)
    {
        if (id >= BrowsePath && id < BrowsePath + PathCount) {
            const int index = id - BrowsePath;
            fs::path selected;
            if (index == Dll) {
                selected = chooseFile(window_, L"Windows DLL (*.dll)\0*.dll\0\0");
            } else if (index == PairFile) {
                selected = chooseFile(window_, L"시험 쌍 CSV (*.csv)\0*.csv\0\0");
            } else {
                selected = chooseFolder(window_);
            }
            if (!selected.empty()) {
                set(EditPath + index, selected.wstring());
            }
        } else if (id == Start) {
            start();
        } else if (id == Cancel) {
            SetEvent(cancel_.get());
            EnableWindow(get(Cancel), FALSE);
        } else if (id == Report && !result_.report.empty()) {
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", result_.report.c_str(),
                                                        nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
                throw std::runtime_error("Cannot open the HTML report");
            }
        }
    }

    LRESULT dispatch(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message) {
        case WM_CREATE:
            createControls();
            if (SetTimer(window_, timer, 150, nullptr) == 0) {
                throw std::runtime_error("Cannot monitor the evaluation progress");
            }
            return 0;
        case WM_TIMER:
            update();
            return 0;
        case WM_COMMAND:
            if (lParam && HIWORD(wParam) == BN_CLICKED) {
                command(LOWORD(wParam));
            }
            return 0;
        case WM_CLOSE:
            if (run_.joinable()) {
                closing_ = true;
                SetEvent(cancel_.get());
            } else {
                DestroyWindow(window_);
            }
            return 0;
        case WM_DESTROY:
            KillTimer(window_, timer);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    HWND window_ = nullptr;
    evaluation::Handle cancel_;
    evaluation::Progress progress_;
    std::atomic<bool> finished_{false};
    bool closing_ = false;
    std::exception_ptr error_;
    evaluation::RunResult result_;
    fs::path latestReport_;
    std::thread run_;
};

} // namespace

int runEvaluationWindow(HINSTANCE instance, int showCommand)
{
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
        InitCommonControlsEx(&controls);
        WNDCLASSW settings{};
        settings.lpfnWndProc = EvaluationWindow::procedure;
        settings.hInstance = instance;
        settings.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        settings.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        settings.lpszClassName = windowClass;
        if (!RegisterClassW(&settings)) {
            throw std::runtime_error("Cannot register the evaluation window");
        }
        EvaluationWindow state;
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT size{0, 0, 840, 613};
        AdjustWindowRect(&size, style, FALSE);
        const auto window = CreateWindowExW(WS_EX_CONTROLPARENT, windowClass, title, style,
            CW_USEDEFAULT, CW_USEDEFAULT, size.right - size.left, size.bottom - size.top,
            nullptr, nullptr, instance, &state);
        if (!window) {
            throw std::runtime_error("Cannot open the evaluation window");
        }
        ShowWindow(window, showCommand);
        MSG message{};
        BOOL status = 0;
        while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!IsDialogMessageW(window, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return status < 0 ? 1 : static_cast<int>(message.wParam);
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, widen(error.what()).c_str(), title, MB_OK | MB_ICONERROR);
        return 1;
    }
}

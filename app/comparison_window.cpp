#include "comparison_window.h"
#include "fingerprint_image.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <mutex>
#include <map>
#include <sstream>
#include <thread>
#include <chrono>
#include <random>
#include <tuple>

// Four pages share one evaluation state; navigation never restarts an evaluation.
// Windows messages and painting stay here; rule generation and DLL calls are in
// fingerprint_image.cpp.
namespace
{
using namespace demo;
constexpr wchar_t kClassName[] = L"FingerprintLabDemo";
constexpr COLORREF kBackground = RGB(244, 246, 251), kCard = RGB(255, 255, 255),
                   kInk = RGB(29, 38, 62);
constexpr COLORREF kMuted = RGB(120, 130, 151), kBorder = RGB(228, 232, 242),
                   kAccent = RGB(91, 79, 224);
constexpr COLORREF kAccentLight = RGB(240, 238, 255), kGenuine = RGB(26, 143, 118),
                   kImpostor = RGB(213, 145, 43);
constexpr COLORREF kDanger = RGB(207, 73, 96);
enum Id
{
    Db = 100,
    BrowseDb,
    Registration,
    Genuine,
    Impostor,
    RegistrationIndex,
    GenuineIndex,
    ImpostorIndex,
    RandomCount,
    Generate,
    RunStop,
    FilterAll,
    FilterGenuine,
    FilterImpostor,
    FilterErrors,
    ChartDistribution,
    ChartErrors,
    Threshold,
    PairTable,
    EmptyTable,
    Parallel,
    SavePairs,
    LoadPairs,
    NavSettings,
    NavResults,
    NavRecognition,
    NavPerformance,
    NextPage,
    ChartRoc
};
enum Font
{
    Small,
    Body,
    Medium,
    Heading,
    Number,
    FontCount
};
struct AppState
{
    HWND window = nullptr;
    std::map<int, HWND> controls;
    std::array<HFONT, FontCount> fonts{};
    HBRUSH white = nullptr;
    double sx = 1, sy = 1, threshold = .85;
    fs::path root, database;
    std::vector<fs::path> databaseChoices;
    Plan plan;
    std::vector<Result> results, liveResults;
    std::vector<size_t> visible;
    Statistics statistics;
    EvaluationTiming timing;
    std::chrono::steady_clock::time_point runStarted;
    double elapsedMilliseconds = 0;
    int filter = 0, chart = 0, page = 0;
    int sortColumn = 1;
    bool sortAscending = true;
    bool dirty = false, running = false, ready = false;
    std::wstring status = L"준비 완료 · 규칙을 확인하고 비교를 실행하세요.";
    std::thread worker;
    std::mutex mutex;
    std::atomic<bool> cancel{false}, finished{false};
    std::atomic<size_t> completed{0};
    std::wstring workerError;
    ~AppState()
    {
        cancel = true;
        if (worker.joinable())
            worker.join();
        for (auto font : fonts)
            if (font)
                DeleteObject(font);
        if (white)
            DeleteObject(white);
    }
};

// 1. Geometry and drawing helpers. All positions use a 1280 x 830 design canvas.
RECT box(const AppState &s, int x, int y, int w, int h)
{
    return {static_cast<LONG>(x * s.sx), static_cast<LONG>(y * s.sy),
            static_cast<LONG>((x + w) * s.sx), static_cast<LONG>((y + h) * s.sy)};
}
void fill(HDC dc, RECT rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}
void rounded(HDC dc, RECT rect, COLORREF color, COLORREF border, int radius)
{
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    auto oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}
void write(const AppState &s, HDC dc, const std::wstring &value, int x, int y, int w, int h,
           Font font = Body, COLORREF color = kInk,
           UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS)
{
    auto previous = SelectObject(dc, s.fonts[font]);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT area = box(s, x, y, w, h);
    DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &area, flags);
    SelectObject(dc, previous);
}
void line(const AppState &s, HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int width = 1)
{
    HPEN pen = CreatePen(PS_SOLID, width, color);
    auto previous = SelectObject(dc, pen);
    MoveToEx(dc, static_cast<int>(x1 * s.sx), static_cast<int>(y1 * s.sy), nullptr);
    LineTo(dc, static_cast<int>(x2 * s.sx), static_cast<int>(y2 * s.sy));
    SelectObject(dc, previous);
    DeleteObject(pen);
}
void dot(const AppState &s, HDC dc, int x, int y, int size, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    auto oldBrush = SelectObject(dc, brush), oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    auto r = box(s, x, y, size, size);
    Ellipse(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
}
std::wstring number(double value, int decimals = 3)
{
    if (!std::isfinite(value))
        return L"—";
    std::wostringstream out;
    out << std::fixed << std::setprecision(decimals) << value;
    return out.str();
}
std::wstring percent(double value)
{
    return std::isfinite(value) ? number(value * 100, 1) + L"%" : L"—";
}
std::wstring fromUtf8(const std::string &value)
{
    int n =
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(n, L' ');
    if (n)
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(),
                            n);
    return out;
}
std::wstring text(HWND field)
{
    int length = GetWindowTextLengthW(field);
    std::wstring out(length + 1, L'\0');
    GetWindowTextW(field, out.data(), length + 1);
    out.resize(length);
    return out;
}
int selected(const AppState &s, int id)
{
    return static_cast<int>(SendMessageW(s.controls.at(id), CB_GETCURSEL, 0, 0));
}
void invalidate(AppState &s) { InvalidateRect(s.window, nullptr, FALSE); }

// 2. A small set of native input controls; cards, buttons and graphs are custom drawn.
HWND add(AppState &s, int id, const wchar_t *kind, const wchar_t *value, DWORD style,
         DWORD extended = 0)
{
    HWND control = CreateWindowExW(extended, kind, value, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
                                   s.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    if (!control)
        throw std::runtime_error("화면 컨트롤을 만들지 못했습니다.");
    s.controls[id] = control;
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(s.fonts[Body]), FALSE);
    return control;
}
void addButton(AppState &s, int id, const wchar_t *title)
{
    add(s, id, L"BUTTON", title, WS_TABSTOP | BS_OWNERDRAW);
}
void addCombo(AppState &s, int id, const std::vector<const wchar_t *> &options, int initial = 0)
{
    HWND c = add(s, id, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
    for (auto item : options)
        SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    SendMessageW(c, CB_SETCURSEL, initial, 0);
}
void makeFonts(AppState &s)
{
    const int heights[]{12, 14, 17, 27, 30};
    double scale = (std::min)(s.sx, s.sy);
    for (int i = 0; i < FontCount; ++i)
    {
        if (s.fonts[i])
            DeleteObject(s.fonts[i]);
        s.fonts[i] = CreateFontW(-static_cast<int>(heights[i] * scale), 0, 0, 0,
                                 i >= Medium ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, L"맑은 고딕");
    }
    for (const auto &[id, control] : s.controls)
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(s.fonts[Body]), FALSE);
}
void place(AppState &s, int id, int x, int y, int w, int h)
{
    RECT r = box(s, x, y, w, h);
    MoveWindow(s.controls.at(id), r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
}
void updatePageVisibility(AppState &s)
{
    // Controls exist once. Only their page is visible and reachable with Tab.
    for (const auto &[id, control] : s.controls)
    {
        bool visible = id >= NavSettings && id <= NavPerformance;
        if (s.page == 0)
            visible = visible || id == Db || id == BrowseDb || id == Registration ||
                      id == Genuine || id == Impostor || id == RandomCount || id == Parallel ||
                      id == Generate || id == NextPage || id == LoadPairs;
        if (s.page == 1)
            visible = visible || (id >= FilterAll && id <= FilterErrors) || id == PairTable ||
                      id == RunStop || id == SavePairs || id == LoadPairs ||
                      (id == EmptyTable && s.visible.empty());
        if (s.page == 2)
            visible = visible || id == ChartDistribution || id == ChartErrors || id == ChartRoc ||
                      id == Threshold;
        for (auto entry : {std::pair<int, int>{RegistrationIndex, Registration},
                           {GenuineIndex, Genuine},
                           {ImpostorIndex, Impostor}})
            if (id == entry.first)
                visible =
                    s.page == 0 && (entry.second == Registration ? selected(s, entry.second) == 2
                                                                 : selected(s, entry.second) == 3);
        ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    }
}
void updateEnabled(AppState &s)
{
    for (int id : {Db, BrowseDb, Registration, Genuine, Impostor, RegistrationIndex, GenuineIndex,
                   ImpostorIndex, RandomCount, Generate, Parallel, LoadPairs})
        EnableWindow(s.controls.at(id), !s.running);
    EnableWindow(s.controls.at(SavePairs), !s.running && !s.dirty && !s.plan.pairs.empty());
    EnableWindow(s.controls.at(NextPage), !s.dirty && !s.plan.pairs.empty());
    EnableWindow(s.controls.at(RunStop), s.running || (!s.dirty && !s.plan.pairs.empty()));
    bool random =
        selected(s, Registration) == 1 || selected(s, Genuine) == 2 || selected(s, Impostor) == 2;
    EnableWindow(s.controls.at(RandomCount), !s.running && random);
    SetWindowTextW(s.controls.at(RunStop), s.running ? L"비교 중지" : L"비교 실행  →");
    updatePageVisibility(s);
}
void layout(AppState &s)
{
    RECT area{};
    GetClientRect(s.window, &area);
    s.sx = area.right / 1280.;
    s.sy = area.bottom / 830.;
    makeFonts(s);
    for (int page = 0; page < 4; ++page)
        place(s, NavSettings + page, 24 + 312 * page, 126, 296, 44);
    place(s, Db, 48, 270, 332, 220);
    place(s, BrowseDb, 48, 318, 332, 38);
    place(s, Registration, 448, 288, 372, 220);
    place(s, RegistrationIndex, 758, 252, 62, 28);
    place(s, Genuine, 448, 374, 372, 220);
    place(s, GenuineIndex, 758, 338, 62, 28);
    place(s, Impostor, 448, 460, 372, 220);
    place(s, ImpostorIndex, 758, 424, 62, 28);
    place(s, RandomCount, 448, 546, 372, 38);
    place(s, Parallel, 888, 288, 344, 38);
    place(s, Generate, 888, 432, 344, 44);
    place(s, NextPage, 888, 492, 344, 44);
    place(s, RunStop, 1036, 330, 220, 40);
    place(s, FilterAll, 24, 330, 90, 40);
    place(s, FilterGenuine, 124, 330, 90, 40);
    place(s, FilterImpostor, 224, 330, 90, 40);
    place(s, FilterErrors, 324, 330, 144, 40);
    place(s, SavePairs, 704, 330, 150, 40);
    if (s.page == 0)
        place(s, LoadPairs, 48, 416, 332, 38);
    else
        place(s, LoadPairs, 870, 330, 150, 40);
    place(s, PairTable, 24, 392, 1232, 356);
    place(s, EmptyTable, 350, 520, 580, 76);
    const int widths[]{40, 70, 70, 40, 48, 124, 54, 50};
    RECT tableArea{};
    GetClientRect(s.controls.at(PairTable), &tableArea);
    const int available =
        (std::max)(100, static_cast<int>(tableArea.right) - GetSystemMetrics(SM_CXVSCROLL) - 4);
    int used = 0;
    for (int i = 0; i < 8; ++i)
    {
        int width = i == 7 ? available - used : available * widths[i] / 496;
        used += width;
        SendMessageW(s.controls.at(PairTable), LVM_SETCOLUMNWIDTH, i, width);
    }
    place(s, ChartDistribution, 48, 344, 240, 36);
    place(s, ChartErrors, 300, 344, 240, 36);
    place(s, ChartRoc, 552, 344, 260, 36);
    place(s, Threshold, 880, 438, 352, 36);
    updateEnabled(s);
    for (int id : {NavSettings, NavResults, NavRecognition, NavPerformance, ChartDistribution,
                   ChartErrors, ChartRoc})
        InvalidateRect(s.controls.at(id), nullptr, TRUE);
    invalidate(s);
}

int positiveInput(AppState &s, int id, int limit)
{
    auto value = text(s.controls.at(id));
    size_t end = 0;
    long long number = 0;
    try
    {
        number = std::stoll(value, &end);
    }
    catch (...)
    {
        throw std::runtime_error("촬영번호·개수에 정수를 입력하세요.");
    }
    int minimum = 1;
    if (end != value.size() || number < minimum || number > limit)
        throw std::runtime_error("입력 숫자의 범위를 확인하세요.");
    return static_cast<int>(number);
}
Settings settings(AppState &s)
{
    Settings out;
    out.database = s.database;
    out.registration = static_cast<RegistrationRule>(selected(s, Registration));
    out.genuine = static_cast<ComparisonRule>(selected(s, Genuine));
    out.impostor = static_cast<ComparisonRule>(selected(s, Impostor));
    out.registrationIndex = positiveInput(s, RegistrationIndex, 9999);
    out.genuineIndex = positiveInput(s, GenuineIndex, 9999);
    out.impostorIndex = positiveInput(s, ImpostorIndex, 9999);
    out.randomCount = positiveInput(s, RandomCount, 100000);
    out.seed =
        std::random_device{}() & 0x7fffffffu; // Internal only; CSV stores the exact selected pairs.
    out.parallelism = positiveInput(s, Parallel, 16);
    return out;
}
bool failed(const Result &r)
{
    return r.status != Status::Success && r.status != Status::Pending &&
           r.status != Status::Running && r.status != Status::Cancelled;
}
void refreshTable(AppState &s)
{
    s.visible.clear();
    for (size_t i = 0; i < s.plan.pairs.size(); ++i)
    {
        const auto &p = s.plan.pairs[i];
        const auto &r = s.results[i];
        if (s.filter == 1 && !p.genuine)
            continue;
        if (s.filter == 2 && p.genuine)
            continue;
        if (s.filter == 3 && (!failed(r) && !(r.status == Status::Success &&
                                              ((r.score >= s.threshold) != p.genuine))))
            continue;
        s.visible.push_back(i);
    }
    // Sort only the display indices. Plan order remains the saved execution order.
    std::sort(s.visible.begin(), s.visible.end(),
              [&](size_t a, size_t b)
              {
                  if (s.sortColumn == 0)
                      return s.sortAscending ? a < b : b < a;
                  auto key = [&](size_t row)
                  {
                      const auto &pair = s.plan.pairs[row];
                      const auto &first =
                          s.plan.images[s.sortColumn == 1 ? pair.registration : pair.attempt];
                      const auto &second =
                          s.plan.images[s.sortColumn == 1 ? pair.attempt : pair.registration];
                      return std::make_tuple(first.finger, first.impression, second.finger,
                                             second.impression, row);
                  };
                  return s.sortAscending ? key(a) < key(b) : key(b) < key(a);
              });
    HWND header =
        reinterpret_cast<HWND>(SendMessageW(s.controls.at(PairTable), LVM_GETHEADER, 0, 0));
    for (int column = 0; column < 3; ++column)
    {
        HDITEMW item{};
        item.mask = HDI_FORMAT;
        SendMessageW(header, HDM_GETITEMW, column, reinterpret_cast<LPARAM>(&item));
        item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (column == s.sortColumn)
            item.fmt |= s.sortAscending ? HDF_SORTUP : HDF_SORTDOWN;
        SendMessageW(header, HDM_SETITEMW, column, reinterpret_cast<LPARAM>(&item));
    }
    SendMessageW(s.controls.at(PairTable), LVM_SETITEMCOUNT, s.visible.size(), LVSICF_NOSCROLL);
    updatePageVisibility(s);
    InvalidateRect(s.controls.at(PairTable), nullptr, FALSE);
    s.statistics = summarize(s.plan, s.results, s.threshold);
    invalidate(s);
}
void generate(AppState &s)
{
    auto plan = buildPlan(settings(s));
    s.plan = std::move(plan);
    s.results.assign(s.plan.pairs.size(), {});
    s.dirty = false;
    s.filter = 0;
    s.completed = 0;
    s.timing = {};
    s.elapsedMilliseconds = 0;
    s.statistics = {};
    s.status = L"비교 목록을 만들었습니다. 같은 이미지끼리의 비교는 제외했습니다.";
    refreshTable(s);
    updateEnabled(s);
}
void startOrStop(AppState &s)
{
    if (s.running)
    {
        s.cancel = true;
        s.status = L"진행 중인 비교가 끝나면 중지합니다…";
        invalidate(s);
        return;
    }
    if (s.dirty || s.plan.pairs.empty())
        return;
    s.plan.settings.parallelism = positiveInput(s, Parallel, 16);
    if (s.worker.joinable())
        s.worker.join();
    s.results.assign(s.plan.pairs.size(), {});
    s.liveResults = s.results;
    s.workerError.clear();
    s.cancel = false;
    s.finished = false;
    s.completed = 0;
    s.timing = {};
    s.elapsedMilliseconds = 0;
    s.running = true;
    s.runStarted = std::chrono::steady_clock::now();
    s.filter = 0;
    s.status = L"동시 실행 " + std::to_wstring(s.plan.settings.parallelism) +
               L"개 · 등록 준비 후 비교 목록을 평가합니다.";
    refreshTable(s);
    updateEnabled(s);
    for (int id : {ChartDistribution, ChartErrors, ChartRoc})
        InvalidateRect(s.controls.at(id), nullptr, TRUE);
    try
    {
        s.worker = std::thread(
            [&s]
            {
                try
                {
                    auto timing = evaluate(
                        s.plan, s.cancel,
                        [&s](size_t row, const Result &result)
                        {
                            {
                                std::lock_guard<std::mutex> guard(s.mutex);
                                s.liveResults[row] = result;
                            }
                            ++s.completed;
                        },
                        [&s](size_t row, int slot)
                        {
                            std::lock_guard<std::mutex> guard(s.mutex);
                            s.liveResults[row].status = Status::Running;
                            s.liveResults[row].worker = slot;
                        });
                    std::lock_guard<std::mutex> guard(s.mutex);
                    s.timing = timing;
                }
                catch (const std::exception &error)
                {
                    std::lock_guard<std::mutex> guard(s.mutex);
                    s.workerError = fromUtf8(error.what());
                }
                s.finished = true;
            });
    }
    catch (...)
    {
        s.running = false;
        updateEnabled(s);
        throw;
    }
}
void pollResults(AppState &s)
{
    if (!s.running)
        return;
    {
        std::lock_guard<std::mutex> guard(s.mutex);
        s.results = s.liveResults;
    }
    s.elapsedMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s.runStarted)
            .count();
    if (s.finished)
    {
        s.worker.join();
        s.running = false;
        if (s.timing.totalMilliseconds > 0)
            s.elapsedMilliseconds = s.timing.totalMilliseconds;
        {
            std::lock_guard<std::mutex> guard(s.mutex);
            s.results = s.liveResults;
        }
        for (auto &result : s.results)
            if (result.status == Status::Pending || result.status == Status::Running)
            {
                if (s.cancel)
                    result.status = Status::Cancelled;
                else if (!s.workerError.empty())
                {
                    result.status = Status::ImageError;
                    result.detail = s.workerError;
                }
            }
        if (!s.workerError.empty())
            s.status = L"실행 오류: " + s.workerError;
        else if (s.cancel)
            s.status = L"비교를 중지했습니다. 그래프는 완료된 비교만 보여 줍니다.";
        else
            s.status = L"비교 완료 · 임계값을 움직이며 본인·타인 판정과 점수 분포를 살펴보세요.";
        updateEnabled(s);
    }
    refreshTable(s);
}

// 3. Page rendering. All pages read the same plan/results; none owns a second run.
void drawCard(AppState &s, HDC dc, int x, int y, int w, int h)
{
    rounded(dc, box(s, x, y + 2, w, h), RGB(235, 238, 246), RGB(235, 238, 246), 16);
    rounded(dc, box(s, x, y, w, h), kCard, kBorder, 16);
}
void drawMetric(AppState &s, HDC dc, int index, const std::wstring &label,
                const std::wstring &value, const std::wstring &caption, COLORREF color = kInk)
{
    int x = 24 + index * 312;
    drawCard(s, dc, x, 196, 296, 106);
    write(s, dc, label, x + 20, 207, 256, 24, Small, kMuted);
    write(s, dc, value, x + 20, 235, 256, 36, Number, color);
    write(s, dc, caption, x + 20, 276, 256, 20, Small, kMuted);
}
struct LatencySummary
{
    double mean = NAN, p95 = NAN, throughput = NAN;
    size_t successful = 0;
};
LatencySummary latencySummary(const AppState &s)
{
    LatencySummary summary;
    std::vector<double> times;
    double sum = 0;
    for (const auto &r : s.results)
        if (r.status == Status::Success)
        {
            times.push_back(r.milliseconds);
            sum += r.milliseconds;
        }
    summary.successful = times.size();
    if (!times.empty())
    {
        std::sort(times.begin(), times.end());
        summary.mean = sum / times.size();
        summary.p95 = times[static_cast<size_t>(std::ceil(times.size() * .95)) - 1];
    }
    if (s.elapsedMilliseconds > 0)
        summary.throughput = summary.successful * 1000. / s.elapsedMilliseconds;
    return summary;
}
void drawSetupPage(AppState &s, HDC dc)
{
    drawCard(s, dc, 24, 196, 380, 290);
    drawCard(s, dc, 24, 506, 380, 280);
    drawCard(s, dc, 424, 196, 420, 590);
    drawCard(s, dc, 864, 196, 392, 390);
    drawCard(s, dc, 864, 606, 392, 180);
    write(s, dc, L"테스트 데이터", 48, 216, 332, 30, Medium);
    write(s, dc, L"비교할 지문이 들어 있는 DB를 선택하세요", 48, 246, 332, 22, Small, kMuted);
    write(s, dc,
          std::to_wstring(s.plan.images.size()) + L"장 · " + std::to_wstring(s.plan.fingers) +
              L"개 손가락",
          48, 370, 332, 25, Body);
    write(s, dc, L"저장한 비교 쌍과 순서를 그대로 사용할 수도 있어요", 48, 458, 332, 20, Small,
          kMuted);
    write(s, dc, L"이렇게 진행하세요", 48, 526, 332, 30, Medium);
    const wchar_t *steps[]{L"1. DB와 비교 규칙을 선택", L"2. 비교 목록을 만들고 실행",
                           L"3. 인식 성능과 처리 성능 확인"};
    for (int i = 0; i < 3; ++i)
        write(s, dc, steps[i], 48, 578 + i * 48, 332, 30, Body, i == 0 ? kAccent : kInk);
    write(s, dc, L"페이지를 이동해도 설정과 결과는 유지됩니다", 48, 744, 332, 22, Small, kMuted);
    write(s, dc, L"비교 목록 규칙", 448, 216, 372, 30, Medium);
    write(s, dc, L"등록 이미지", 448, 252, 270, 28, Body);
    write(s, dc, L"본인 비교 · 같은 손가락", 448, 338, 290, 28, Body);
    write(s, dc, L"타인 비교 · 다른 손가락", 448, 424, 290, 28, Body);
    write(s, dc, L"랜덤 선택 장수", 448, 510, 372, 28, Body);
    write(s, dc, L"랜덤 규칙을 선택했을 때 적용됩니다", 448, 596, 372, 24, Small, kMuted);
    line(s, dc, 448, 642, 820, 642, kBorder);
    write(s, dc, L"같은 이미지끼리의 비교는 제외합니다", 448, 662, 372, 26, Body);
    write(s, dc, L"같은 손가락의 다른 촬영본끼리 비교해야", 448, 702, 372, 24, Small, kMuted);
    write(s, dc, L"촬영 조건이 달라져도 인식하는지 확인할 수 있어요", 448, 728, 372, 24, Small,
          kMuted);
    write(s, dc, L"실행 설정", 888, 216, 344, 30, Medium);
    write(s, dc, L"동시 비교 수 · 1~16개", 888, 252, 344, 28, Body);
    write(s, dc, L"1개는 순차, 2개 이상은 병렬 평가", 888, 340, 344, 24, Small, kMuted);
    write(s, dc, L"한 번에 실행할 1:1 비교 개수를 정합니다", 888, 370, 344, 24, Small, kMuted);
    write(s, dc,
          s.dirty ? L"규칙 변경됨 · 새 목록을 만들어 주세요"
                  : L"현재 목록 " + std::to_wstring(s.plan.pairs.size()) + L"쌍",
          888, 548, 344, 24, Small, s.dirty ? kDanger : kMuted);
    write(s, dc, L"현재 실험", 888, 626, 344, 30, Medium);
    write(s, dc, s.database.filename().wstring(), 888, 668, 344, 28, Body);
    write(s, dc, L"등록 이미지 " + std::to_wstring(s.plan.registrations) + L"장", 888, 706, 344, 24,
          Body);
    write(s, dc, s.plan.sourceFile.empty() ? L"규칙으로 생성한 목록" : L"저장 파일에서 불러온 목록",
          888, 746, 344, 22, Small, kMuted);
}
void drawResultsPage(AppState &s, HDC dc)
{
    auto genuine = std::count_if(s.plan.pairs.begin(), s.plan.pairs.end(),
                                 [](const auto &p) { return p.genuine; });
    auto performance = latencySummary(s);
    drawMetric(s, dc, 0, L"비교 목록", std::to_wstring(s.plan.pairs.size()) + L"쌍",
               L"본인 " + std::to_wstring(genuine) + L" · 타인 " +
                   std::to_wstring(s.plan.pairs.size() - genuine));
    drawMetric(s, dc, 1, L"완료 / 전체",
               std::to_wstring(s.statistics.completed) + L" / " +
                   std::to_wstring(s.plan.pairs.size()),
               L"처리 실패 " + std::to_wstring(s.statistics.failed) + L"건", kAccent);
    drawMetric(s, dc, 2, L"전체 경과 시간", number(s.elapsedMilliseconds / 1000., 3) + L" 초",
               L"이미지 읽기·등록 준비 포함");
    drawMetric(s, dc, 3, L"성공 처리량", number(performance.throughput, 1) + L" 쌍/초",
               L"성공한 비교 수 / 전체 경과 시간", kGenuine);
    drawCard(s, dc, 24, 382, 1232, 406);
    write(s, dc, std::to_wstring(s.visible.size()) + L"쌍 표시 · 등록/비교 제목을 눌러 정렬", 48,
          760, 590, 22, Small, kMuted);
    write(s, dc,
          s.dirty ? L"규칙이 변경되어 새 목록 생성이 필요합니다"
                  : L"정합 점수는 동일인 확률이 아닙니다",
          650, 760, 582, 22, Small, s.dirty ? kDanger : kMuted,
          DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}
void drawRecognitionPage(AppState &s, HDC dc)
{
    const auto &stats = s.statistics;
    drawMetric(s, dc, 0, L"같은 손가락 평균 점수", number(stats.genuineMean),
               std::to_wstring(stats.genuineOk) + L"쌍 · 성공한 비교 기준", kGenuine);
    drawMetric(s, dc, 1, L"다른 손가락 평균 점수", number(stats.impostorMean),
               std::to_wstring(stats.impostorOk) + L"쌍 · 성공한 비교 기준", kImpostor);
    drawMetric(s, dc, 2, L"등오류율 · EER", percent(stats.eer),
               L"두 오류율이 같아지는 지점의 추정값", kAccent);
    drawMetric(s, dc, 3, L"현재 판정 기준", number(s.threshold),
               L"점수가 기준 이상이면 같은 지문으로 판정");
    drawCard(s, dc, 24, 326, 812, 462);
    drawCard(s, dc, 856, 326, 400, 462);
    if (s.chart != 2)
    {
        dot(s, dc, 48, 402, 7, kGenuine);
        dot(s, dc, 440, 402, 7, kImpostor);
        write(s, dc, s.chart == 0 ? L"같은 손가락" : L"동일 지문 미인식", 65, 389, 340, 28, Small,
              kMuted);
        write(s, dc, s.chart == 0 ? L"다른 손가락" : L"다른 지문 오인식", 457, 389, 350, 28, Small,
              kMuted);
    }
    else
        write(s, dc, L"가로: 다른 지문 오인식 비율 / 세로: 동일 지문 인식 비율", 48, 389, 764, 28,
              Small, kMuted);
    const int left = 82, top = 434, right = 800, bottom = 694;
    const int width = right - left, height = bottom - top;
    std::array<size_t, 20> genuine{}, impostor{};
    double ymax = .2;
    for (double score : stats.genuineScores)
        ++genuine[(std::min)(19, static_cast<int>(score * 20))];
    for (double score : stats.impostorScores)
        ++impostor[(std::min)(19, static_cast<int>(score * 20))];
    if (s.chart == 0)
        for (int i = 0; i < 20; ++i)
        {
            if (stats.genuineOk)
                ymax = (std::max)(ymax, genuine[i] / static_cast<double>(stats.genuineOk));
            if (stats.impostorOk)
                ymax = (std::max)(ymax, impostor[i] / static_cast<double>(stats.impostorOk));
        }
    ymax = s.chart != 0 ? 1. : (std::min)(1., std::ceil(ymax * 5) / 5);
    for (int i = 0; i <= 4; ++i)
    {
        int y = bottom - height * i / 4;
        line(s, dc, left, y, right, y, kBorder);
        write(s, dc, number(ymax * 100 * i / 4, 0) + L"%", 32, y - 10, 42, 20, Small, kMuted,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        write(s, dc, number(i / 4., 2), left + width * i / 4 - 20, bottom + 8, 44, 22, Small,
              kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    bool enough = s.chart == 2 ? std::isfinite(stats.fmr) && std::isfinite(stats.fnmr)
                               : !stats.genuineScores.empty() || !stats.impostorScores.empty();
    if (!enough)
    {
        write(s, dc,
              s.chart == 2 ? L"ROC에는 본인·타인 비교 결과가 모두 필요합니다"
                           : L"비교를 실행하면 점수 분포가 표시됩니다",
              110, 544, 650, 36, Medium, kMuted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    else if (s.chart == 0)
    {
        for (int i = 0; i < 20; ++i)
        {
            int x = left + width * i / 20;
            double g = stats.genuineOk ? genuine[i] / static_cast<double>(stats.genuineOk) : 0;
            double n = stats.impostorOk ? impostor[i] / static_cast<double>(stats.impostorOk) : 0;
            int gh = static_cast<int>(height * g / ymax), nh = static_cast<int>(height * n / ymax);
            fill(dc, box(s, x, bottom - gh, width / 40 - 2, gh), kGenuine);
            fill(dc, box(s, x + width / 40, bottom - nh, width / 40 - 2, nh), kImpostor);
        }
    }
    else
    {
        for (int series = 0; series < (s.chart == 2 ? 1 : 2); ++series)
        {
            HPEN pen = CreatePen(PS_SOLID, 2,
                                 s.chart == 2 ? kAccent
                                 : series     ? kGenuine
                                              : kImpostor);
            auto previous = SelectObject(dc, pen);
            bool first = true;
            for (const auto &point : stats.curve)
            {
                double xValue = s.chart == 2 ? point.fmr : (std::min)(1., point.threshold);
                double yValue = s.chart == 2 ? 1. - point.fnmr : series ? point.fnmr : point.fmr;
                if (!std::isfinite(xValue) || !std::isfinite(yValue))
                    continue;
                int x = static_cast<int>((left + width * xValue) * s.sx);
                int y = static_cast<int>((bottom - height * yValue) * s.sy);
                if (first)
                    MoveToEx(dc, x, y, nullptr);
                else
                    LineTo(dc, x, y);
                first = false;
            }
            SelectObject(dc, previous);
            DeleteObject(pen);
        }
    }
    if (s.chart != 2)
        line(s, dc, left + static_cast<int>(width * s.threshold), top,
             left + static_cast<int>(width * s.threshold), bottom, kAccent);
    else if (enough)
        dot(s, dc, left + static_cast<int>(width * stats.fmr) - 4,
            bottom - static_cast<int>(height * (1. - stats.fnmr)) - 4, 8, kAccent);
    write(s, dc,
          s.chart == 0   ? L"정합 점수"
          : s.chart == 1 ? L"판정 기준"
                         : L"다른 지문 오인식 비율",
          48, 738, 764, 24, Small, kMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    write(s, dc, L"판정 기준 조절", 880, 348, 352, 30, Medium);
    write(s, dc, number(s.threshold), 880, 390, 352, 38, Number, kAccent);
    write(s, dc, L"기준 이상: 같은 지문 / 기준 미만: 다른 지문", 880, 480, 352, 24, Small, kMuted);
    line(s, dc, 880, 518, 1232, 518, kBorder);
    write(s, dc, L"다른 지문 오인식 비율 · FMR", 880, 536, 352, 26, Body);
    write(s, dc, percent(stats.fmr), 880, 571, 352, 38, Number, kImpostor);
    write(s, dc, L"동일 지문 미인식 비율 · FNMR", 880, 632, 352, 26, Body);
    write(s, dc, percent(stats.fnmr), 880, 667, 352, 38, Number, kGenuine);
    write(s, dc, L"FNMR은 본인 비교의 처리 실패도 포함합니다", 880, 730, 352, 22, Small, kMuted);
    write(s, dc, L"기준을 바꾸면 비교 결과의 판정도 함께 바뀝니다", 880, 755, 352, 22, Small,
          kMuted);
}
void drawPerformancePage(AppState &s, HDC dc)
{
    const auto performance = latencySummary(s);
    drawMetric(s, dc, 0, L"전체 경과 시간", number(s.elapsedMilliseconds / 1000., 3) + L" 초",
               L"이미지 읽기·등록 준비·비교 포함");
    drawMetric(s, dc, 1, L"성공 처리량", number(performance.throughput, 1) + L" 쌍/초",
               L"성공한 비교 수 / 전체 경과 시간", kAccent);
    drawMetric(s, dc, 2, L"평균 비교 지연", number(performance.mean, 2) + L" ms",
               L"성공한 비교 기준");
    drawMetric(s, dc, 3, L"P95 비교 지연", number(performance.p95, 2) + L" ms",
               L"성공한 비교의 95%가 이 시간 이내 완료");
    drawCard(s, dc, 24, 326, 500, 208);
    drawCard(s, dc, 24, 554, 500, 234);
    drawCard(s, dc, 544, 326, 712, 462);
    write(s, dc, L"실행 단계별 경과 시간", 48, 348, 452, 30, Medium);
    if (!s.running && s.timing.totalMilliseconds > 0)
    {
        write(s, dc, L"이미지 읽기·등록 준비", 48, 400, 240, 25, Body);
        write(s, dc, number(s.timing.preparationMilliseconds, 1) + L" ms", 300, 400, 200, 25, Body,
              kInk, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        int prep =
            static_cast<int>(452 * s.timing.preparationMilliseconds / s.timing.totalMilliseconds);
        fill(dc, box(s, 48, 446, 452, 14), kAccentLight);
        fill(dc, box(s, 48, 446, prep, 14), kAccent);
        write(s, dc, L"비교 단계", 48, 484, 240, 25, Body);
        write(s, dc, number(s.timing.matchingMilliseconds, 1) + L" ms", 300, 484, 200, 25, Body,
              kInk, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    else
        write(s, dc,
              s.running ? L"평가가 끝나면 단계별 시간을 표시합니다"
                        : L"비교를 실행하면 처리 성능이 표시됩니다",
              48, 422, 452, 52, Body, kMuted);
    write(s, dc, L"시간 지표 읽는 방법", 48, 576, 452, 30, Medium);
    write(s, dc, L"비교 지연: 버퍼 준비 + 특징 추출 + 정합", 48, 626, 452, 26, Body);
    write(s, dc, L"병렬 비교 시간은 개별 지연 시간을 더한 값이 아닙니다", 48, 666, 452, 26, Small,
          kMuted);
    write(s, dc, L"1개는 순차 평가 · 2~16개는 병렬 평가", 48, 706, 452, 26, Body);
    write(s, dc, L"작업 배정과 완료 순서는 실행마다 달라질 수 있습니다", 48, 748, 452, 22, Small,
          kMuted);
    const int slots = static_cast<int>(
        (std::min)(s.plan.pairs.size(), static_cast<size_t>(s.plan.settings.parallelism)));
    write(s, dc, slots > 1 ? L"병렬 작업 현황" : L"순차 작업 현황", 568, 348, 400, 30, Medium);
    write(s, dc, L"최대 " + std::to_wstring(slots) + L"개 동시 비교", 1000, 350, 232, 26, Small,
          kMuted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    for (int slot = 1; slot <= slots; ++slot)
    {
        size_t done = 0, failures = 0, active = 0;
        for (size_t row = 0; row < s.results.size(); ++row)
        {
            const auto &r = s.results[row];
            if (r.worker != slot)
                continue;
            if (r.status == Status::Running)
                active = row + 1;
            else if (r.completionOrder)
            {
                ++done;
                if (failed(r))
                    ++failures;
            }
        }
        int x = 568 + ((slot - 1) % 4) * 166, y = 398 + ((slot - 1) / 4) * 84;
        rounded(dc, box(s, x, y, 150, 72), active ? kAccentLight : RGB(248, 249, 253), kBorder, 10);
        write(s, dc, L"W" + std::to_wstring(slot) + L" · " + std::to_wstring(done) + L"건 완료",
              x + 10, y + 7, 130, 25, Body, active ? kAccent : kInk);
        write(s, dc,
              active ? L"#" + std::to_wstring(active) + L" 비교 중"
                     : L"실패 " + std::to_wstring(failures) + L"건",
              x + 10, y + 38, 130, 22, Small, kMuted);
    }
    write(s, dc,
          L"완료 " + std::to_wstring(s.statistics.completed) + L" / " +
              std::to_wstring(s.plan.pairs.size()) + L"쌍 · 성공 " +
              std::to_wstring(performance.successful) + L"쌍 · 처리 실패 " +
              std::to_wstring(s.statistics.failed) + L"건",
          568, 754, 664, 24, Small, kMuted);
}
void drawDashboard(AppState &s, HDC dc)
{
    RECT client{};
    GetClientRect(s.window, &client);
    fill(dc, client, kBackground);
    fill(dc, box(s, 0, 0, 1280, 108), kCard);
    line(s, dc, 0, 107, 1280, 107, kBorder);
    rounded(dc, box(s, 24, 24, 48, 48), kAccent, kAccent, 12);
    write(s, dc, L"FP", 24, 24, 48, 48, Medium, RGB(255, 255, 255),
          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    const wchar_t *titles[]{L"실험 설정", L"비교 실행 · 결과", L"인식 성능", L"처리 성능"};
    const wchar_t *descriptions[]{L"데이터와 비교 규칙을 정하고 실험을 준비하세요.",
                                  L"각 1:1 비교의 상태와 판정을 확인하세요.",
                                  L"점수 분포와 판정 기준에 따른 오류를 살펴보세요.",
                                  L"얼마나 빠르게, 몇 쌍을 동시에 처리했는지 확인하세요."};
    write(s, dc, titles[s.page], 88, 20, 740, 38, Heading);
    write(s, dc, descriptions[s.page], 90, 62, 820, 25, Body, kMuted);
    rounded(dc, box(s, 1001, 27, 255, 48), RGB(235, 247, 241), RGB(235, 247, 241), 12);
    dot(s, dc, 1018, 47, 8, kGenuine);
    write(s, dc, recognitionBackendLabel(), 1034, 30, 211, 42, Small, kGenuine);
    if (s.page == 0)
        drawSetupPage(s, dc);
    else if (s.page == 1)
        drawResultsPage(s, dc);
    else if (s.page == 2)
        drawRecognitionPage(s, dc);
    else
        drawPerformancePage(s, dc);
    line(s, dc, 26, 808, 1254, 808, kBorder);
    dot(s, dc, 28, 818, 6, s.running ? kAccent : kGenuine);
    write(s, dc, s.status, 43, 807, 1195, 25, Small, kMuted);
}

void drawButton(AppState &s, const DRAWITEMSTRUCT &item)
{
    int id = static_cast<int>(item.CtlID);
    bool disabled = (item.itemState & ODS_DISABLED) != 0;
    bool primary = id == RunStop || id == NextPage;
    bool active =
        (id >= FilterAll && id <= FilterErrors && id - FilterAll == s.filter) ||
        (id >= ChartDistribution && id <= ChartErrors && id - ChartDistribution == s.chart) ||
        (id == ChartRoc && s.chart == 2) ||
        (id >= NavSettings && id <= NavPerformance && id - NavSettings == s.page);
    COLORREF bg = primary  ? (s.running ? kDanger : kAccent)
                  : active ? kAccentLight
                           : RGB(248, 249, 253);
    COLORREF fg = primary ? RGB(255, 255, 255) : active ? kAccent : kMuted;
    if (id == Generate)
    {
        bg = RGB(246, 245, 255);
        fg = kAccent;
    }
    if (disabled)
    {
        bg = RGB(241, 243, 248);
        fg = RGB(172, 180, 195);
    }
    if ((item.itemState & ODS_SELECTED) && !disabled)
        bg = primary ? RGB(72, 60, 195) : RGB(231, 229, 250);
    RECT area = item.rcItem;
    fill(item.hDC, area, kCard);
    InflateRect(&area, -1, -1);
    rounded(item.hDC, area, bg,
            primary  ? bg
            : active ? kAccentLight
                     : kBorder,
            static_cast<int>(9 * (std::min)(s.sx, s.sy)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, fg);
    auto previous = SelectObject(item.hDC, s.fonts[primary ? Medium : Body]);
    auto title = text(item.hwndItem);
    DrawTextW(item.hDC, title.c_str(), -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (item.itemState & ODS_FOCUS)
    {
        InflateRect(&area, -4, -4);
        DrawFocusRect(item.hDC, &area);
    }
    SelectObject(item.hDC, previous);
}

// 4. User actions, list contents and selection details.
std::wstring chooseFolder(HWND owner)
{
    IFileOpenDialog *dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))))
        return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"한 FVC DB의 TIFF 이미지가 들어 있는 폴더 선택");
    std::wstring result;
    if (SUCCEEDED(dialog->Show(owner)))
    {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)))
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}
std::wstring chooseCsv(AppState &s, bool save)
{
    std::array<wchar_t, 32768> file{};
    if (save)
        lstrcpynW(file.data(), L"comparison-pairs.csv", static_cast<int>(file.size()));
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = s.window;
    dialog.lpstrFile = file.data();
    dialog.nMaxFile = static_cast<DWORD>(file.size());
    dialog.lpstrFilter = L"비교 목록 CSV (*.csv)\0*.csv\0\0";
    dialog.lpstrDefExt = L"csv";
    dialog.lpstrTitle = save ? L"전체 비교 쌍과 순서 저장" : L"평가할 비교 쌍 CSV 선택";
    dialog.lpstrInitialDir = s.root.c_str();
    dialog.Flags =
        OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    bool accepted = save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog);
    if (!accepted && CommDlgExtendedError())
        throw std::runtime_error("CSV 파일 선택 창을 열지 못했습니다.");
    return accepted ? file.data() : L"";
}
void useImportedPlan(AppState &s, Plan plan)
{
    const auto config = plan.settings;
    s.ready = false;
    s.database = config.database;
    auto choice = std::find(s.databaseChoices.begin(), s.databaseChoices.end(), s.database);
    size_t index = static_cast<size_t>(choice - s.databaseChoices.begin());
    if (choice == s.databaseChoices.end())
    {
        s.databaseChoices.push_back(s.database);
        auto title = L"CSV · " + s.database.filename().wstring();
        SendMessageW(s.controls.at(Db), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(title.c_str()));
    }
    SendMessageW(s.controls.at(Db), CB_SETCURSEL, index, 0);
    SendMessageW(s.controls.at(Registration), CB_SETCURSEL, static_cast<int>(config.registration),
                 0);
    SendMessageW(s.controls.at(Genuine), CB_SETCURSEL, static_cast<int>(config.genuine), 0);
    SendMessageW(s.controls.at(Impostor), CB_SETCURSEL, static_cast<int>(config.impostor), 0);
    for (auto entry : {std::pair<int, unsigned>{RegistrationIndex,
                                                static_cast<unsigned>(config.registrationIndex)},
                       {GenuineIndex, static_cast<unsigned>(config.genuineIndex)},
                       {ImpostorIndex, static_cast<unsigned>(config.impostorIndex)},
                       {RandomCount, static_cast<unsigned>(config.randomCount)},
                       {Parallel, static_cast<unsigned>(config.parallelism)}})
        SetWindowTextW(s.controls.at(entry.first), std::to_wstring(entry.second).c_str());
    s.ready = true;
    s.plan = std::move(plan);
    s.results.assign(s.plan.pairs.size(), {});
    s.dirty = false;
    s.filter = 0;
    s.completed = 0;
    s.timing = {};
    s.elapsedMilliseconds = 0;
    s.status = L"CSV에서 " + std::to_wstring(s.plan.pairs.size()) +
               L"쌍을 불러왔습니다. 이 목록만 평가합니다. 병렬 완료 순서는 달라질 수 있습니다.";
    refreshTable(s);
    updateEnabled(s);
}

void markDirty(AppState &s)
{
    s.dirty = true;
    s.status = L"규칙이 변경되었습니다. 목록 만들기를 눌러 적용하세요.";
    updateEnabled(s);
    invalidate(s);
}
std::wstring rowText(AppState &s, size_t row, int column)
{
    if (row >= s.visible.size())
        return {};
    size_t index = s.visible[row];
    const auto &pair = s.plan.pairs[index];
    const auto &r = s.results[index];
    switch (column)
    {
    case 0:
        return std::to_wstring(index + 1);
    case 1:
        return s.plan.images[pair.registration].path.stem().wstring();
    case 2:
        return s.plan.images[pair.attempt].path.stem().wstring();
    case 3:
        return pair.genuine ? L"본인" : L"타인";
    case 4:
        return r.status == Status::Success ? number(r.score) : L"—";
    case 5:
        if (r.status == Status::Running)
            return L"비교 중";
        if (r.status == Status::Pending)
            return L"대기";
        if (r.status == Status::Cancelled)
            return L"중지";
        if (r.status != Status::Success)
            return L"처리 실패";
        if ((r.score >= s.threshold) != pair.genuine)
            return pair.genuine ? L"동일 지문 미인식" : L"다른 지문 오인식";
        return pair.genuine ? L"같음 (정상)" : L"다름 (정상)";
    case 6:
        return r.status == Status::Success ? number(r.milliseconds, 2) : L"—";
    case 7:
        return r.worker ? L"W" + std::to_wstring(r.worker) : L"—";
    }
    return {};
}
void selectionDetail(AppState &s)
{
    int row = static_cast<int>(
        SendMessageW(s.controls.at(PairTable), LVM_GETNEXTITEM, -1, LVNI_SELECTED));
    if (row < 0 || static_cast<size_t>(row) >= s.visible.size())
        return;
    const auto index = s.visible[row];
    const auto &pair = s.plan.pairs[index];
    const auto &r = s.results[index];
    s.status = s.plan.images[pair.registration].path.filename().wstring() + L" → " +
               s.plan.images[pair.attempt].path.filename().wstring() + L"  ·  " +
               rowText(s, row, 3) + L"  ·  " + rowText(s, row, 5);
    if (r.status == Status::Success)
    {
        s.status += pair.genuine ? L"  ·  실제: 같은 손가락" : L"  ·  실제: 다른 손가락";
        s.status += r.score >= s.threshold ? L" / 판정: 같은 지문" : L" / 판정: 다른 지문";
    }
    if (r.worker)
        s.status += L"  ·  작업 W" + std::to_wstring(r.worker);
    if (r.completionOrder)
        s.status += L"  ·  완료 순서 " + std::to_wstring(r.completionOrder) + L"번";
    if (!r.detail.empty())
        s.status += L"  ·  " + r.detail;
    invalidate(s);
}
void command(AppState &s, int id, int code)
{
    if (!s.ready)
        return;
    if (code == CBN_SELCHANGE && (id == Registration || id == Genuine || id == Impostor))
    {
        markDirty(s);
        return;
    }
    if (code == CBN_SELCHANGE && id == Db)
    {
        int index = selected(s, Db);
        if (index >= 0 && static_cast<size_t>(index) < s.databaseChoices.size())
        {
            auto previous = s.database;
            s.database = s.databaseChoices[index];
            try
            {
                generate(s);
            }
            catch (...)
            {
                s.database = previous;
                auto old = std::find(s.databaseChoices.begin(), s.databaseChoices.end(), previous);
                if (old != s.databaseChoices.end())
                    SendMessageW(s.controls.at(Db), CB_SETCURSEL, old - s.databaseChoices.begin(),
                                 0);
                throw;
            }
        }
        return;
    }
    if (code == EN_CHANGE &&
        (id == RandomCount || id == RegistrationIndex || id == GenuineIndex || id == ImpostorIndex))
    {
        markDirty(s);
        return;
    }
    if (code != BN_CLICKED)
        return;
    if (id >= FilterAll && id <= FilterErrors)
    {
        s.filter = id - FilterAll;
        refreshTable(s);
        for (int i = FilterAll; i <= FilterErrors; ++i)
            InvalidateRect(s.controls.at(i), nullptr, TRUE);
        return;
    }
    if (id >= NavSettings && id <= NavPerformance)
    {
        s.page = id - NavSettings;
        layout(s);
        return;
    }
    if (id == NextPage)
    {
        s.page = 1;
        layout(s);
        return;
    }
    if (id == ChartDistribution || id == ChartErrors || id == ChartRoc)
    {
        s.chart = id == ChartRoc ? 2 : id - ChartDistribution;
        for (int button : {ChartDistribution, ChartErrors, ChartRoc})
            InvalidateRect(s.controls.at(button), nullptr, TRUE);
        invalidate(s);
        return;
    }
    if (id == Generate)
    {
        generate(s);
        s.page = 1;
        layout(s);
    }
    else if (id == SavePairs)
    {
        auto file = chooseCsv(s, true);
        if (file.empty())
            return;
        auto plan = s.plan;
        plan.settings.parallelism = positiveInput(s, Parallel, 16);
        savePlan(plan, file);
        s.status = L"전체 " + std::to_wstring(plan.pairs.size()) +
                   L"쌍의 파일 경로와 순서를 저장했습니다: " + file;
        invalidate(s);
    }
    else if (id == LoadPairs)
    {
        auto file = chooseCsv(s, false);
        if (!file.empty())
        {
            auto fallback = s.plan.settings;
            fallback.database = s.database;
            useImportedPlan(s, loadPlan(file, fallback));
            s.page = 1;
            layout(s);
        }
    }
    else if (id == RunStop)
        startOrStop(s);
    else if (id == BrowseDb)
    {
        auto path = chooseFolder(s.window);
        if (path.empty())
            return;
        // Build successfully before replacing the current usable DB and results.
        auto candidate = settings(s);
        candidate.database = path;
        auto plan = buildPlan(candidate);
        s.database = path;
        s.databaseChoices.push_back(path);
        auto label = L"선택 폴더 · " + fs::path(path).filename().wstring();
        SendMessageW(s.controls.at(Db), CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        SendMessageW(s.controls.at(Db), CB_SETCURSEL, s.databaseChoices.size() - 1, 0);
        s.plan = std::move(plan);
        s.results.assign(s.plan.pairs.size(), {});
        s.dirty = false;
        s.filter = 0;
        s.status = L"새 DB의 비교 목록을 만들었습니다.";
        refreshTable(s);
        updateEnabled(s);
    }
}

// 5. Initialization and the Windows event loop.
fs::path findRoot()
{
    std::vector<wchar_t> path(32768);
    GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    fs::path directory = fs::path(path.data()).parent_path();
    for (auto p = directory; !p.empty(); p = p.parent_path())
    {
        if (fs::is_directory(p / L"data" / L"fvc"))
            return p;
        if (p == p.parent_path())
            break;
    }
    return directory;
}
void createControls(AppState &s)
{
    s.white = CreateSolidBrush(kCard);
    makeFonts(s);
    s.root = findRoot();
    s.databaseChoices = {s.root / L"data/fvc/FVC2000_DB3_B", s.root / L"data/fvc/FVC2002_DB2_B",
                         s.root / L"data/fvc/FVC2004_DB1_B"};
    s.database = s.databaseChoices[1];
    addCombo(s, Db, {L"FVC 2000 · DB3_B", L"FVC 2002 · DB2_B", L"FVC 2004 · DB1_B"}, 1);
    addButton(s, BrowseDb, L"다른 DB 폴더 선택");
    addCombo(s, Registration,
             {L"첫 번째 촬영본", L"무작위 1장", L"촬영번호 지정", L"모든 촬영본 각각"});
    addCombo(s, Genuine, {L"나머지 촬영본 모두", L"첫 후보 1장", L"무작위 N개", L"촬영번호 지정"});
    addCombo(
        s, Impostor,
        {L"다른 손가락의 모든 촬영본", L"다른 손가락의 첫 촬영본", L"무작위 N개", L"촬영번호 지정"},
        1);
    for (auto input : {std::pair<int, const wchar_t *>{RegistrationIndex, L"1"},
                       {GenuineIndex, L"2"},
                       {ImpostorIndex, L"1"},
                       {RandomCount, L"3"},
                       {Parallel, L"2"}})
        add(s, input.first, L"EDIT", input.second, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
    addButton(s, Generate, L"새 비교 목록 만들기  →");
    addButton(s, RunStop, L"비교 실행  →");
    addButton(s, SavePairs, L"전체 저장");
    addButton(s, LoadPairs, L"불러오기");
    addButton(s, FilterAll, L"전체");
    addButton(s, FilterGenuine, L"본인");
    addButton(s, FilterImpostor, L"타인");
    addButton(s, FilterErrors, L"잘못된 판정");
    addButton(s, ChartDistribution, L"점수 분포");
    addButton(s, ChartErrors, L"오류율");
    addButton(s, ChartRoc, L"ROC 곡선");
    addButton(s, NavSettings, L"01  실험 설정");
    addButton(s, NavResults, L"02  비교 실행 · 결과");
    addButton(s, NavRecognition, L"03  인식 성능");
    addButton(s, NavPerformance, L"04  처리 성능");
    addButton(s, NextPage, L"현재 목록으로 이동  →");
    HWND list = add(s, PairTable, WC_LISTVIEWW, L"비교 목록",
                    WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0);
    SendMessageW(list, CCM_SETUNICODEFORMAT, TRUE, 0);
    SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SendMessageW(list, LVM_SETBKCOLOR, 0, kCard);
    SendMessageW(list, LVM_SETTEXTBKCOLOR, 0, CLR_NONE);
    const wchar_t *columns[]{L"#", L"등록", L"비교", L"관계", L"점수", L"판정", L"ms", L"작업"};
    for (int i = 0; i < 8; ++i)
    {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.pszText = const_cast<wchar_t *>(columns[i]);
        c.cx = 80;
        c.fmt = i == 4 || i == 6 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        SendMessageW(list, LVM_INSERTCOLUMNW, i, reinterpret_cast<LPARAM>(&c));
    }
    add(s, EmptyTable, L"STATIC", L"조건에 맞는 비교가 없습니다", SS_OWNERDRAW);
    HWND slider =
        add(s, Threshold, TRACKBAR_CLASSW, L"판정 임계값", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS);
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
    SendMessageW(slider, TBM_SETPOS, TRUE, 850);
    // Native controls use the Windows Explorer theme; no image assets or extra source files.
    HMODULE theme = LoadLibraryW(L"uxtheme.dll");
    if (theme)
    {
        auto apply = reinterpret_cast<HRESULT(WINAPI *)(HWND, LPCWSTR, LPCWSTR)>(
            GetProcAddress(theme, "SetWindowTheme"));
        if (apply)
            apply(list, L"Explorer", nullptr);
        FreeLibrary(theme);
    }
    s.ready = true;
    layout(s);
    try
    {
        generate(s);
    }
    catch (const std::exception &error)
    {
        s.status = fromUtf8(error.what());
        updateEnabled(s);
    }
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto *s = reinterpret_cast<AppState *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    try
    {
        switch (message)
        {
        case WM_CREATE:
            s = static_cast<AppState *>(reinterpret_cast<CREATESTRUCTW *>(lParam)->lpCreateParams);
            s->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
            createControls(*s);
            SetTimer(window, 1, 80, nullptr);
            return 0;
        case WM_SIZE:
            if (s && s->ready)
                layout(*s);
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO *>(lParam)->ptMinTrackSize = {1080, 720};
            return 0;
        case WM_COMMAND:
            if (s)
                command(*s, LOWORD(wParam), HIWORD(wParam));
            return 0;
        case WM_TIMER:
            if (s)
                pollResults(*s);
            return 0;
        case WM_HSCROLL:
            if (s && reinterpret_cast<HWND>(lParam) == s->controls.at(Threshold))
            {
                s->threshold = SendMessageW(s->controls.at(Threshold), TBM_GETPOS, 0, 0) / 1000.;
                refreshTable(*s);
            }
            return 0;
        case WM_NOTIFY:
            if (s && reinterpret_cast<NMHDR *>(lParam)->idFrom == PairTable)
            {
                auto *header = reinterpret_cast<NMHDR *>(lParam);
                if (header->code == LVN_COLUMNCLICK)
                {
                    const auto *click = reinterpret_cast<NMLISTVIEW *>(lParam);
                    if (click->iSubItem >= 0 && click->iSubItem <= 2)
                    {
                        s->sortAscending =
                            s->sortColumn == click->iSubItem ? !s->sortAscending : true;
                        s->sortColumn = click->iSubItem;
                        LVITEMW item{};
                        item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                        SendMessageW(s->controls.at(PairTable), LVM_SETITEMSTATE, -1,
                                     reinterpret_cast<LPARAM>(&item));
                        refreshTable(*s);
                    }
                    return 0;
                }
                if (header->code == LVN_GETDISPINFOW)
                {
                    auto *item = reinterpret_cast<NMLVDISPINFOW *>(lParam);
                    if (item->item.mask & LVIF_TEXT)
                    {
                        auto value =
                            rowText(*s, static_cast<size_t>(item->item.iItem), item->item.iSubItem);
                        if (item->item.pszText && item->item.cchTextMax > 0)
                            lstrcpynW(item->item.pszText, value.c_str(), item->item.cchTextMax);
                    }
                    return 0;
                }
                if (header->code == LVN_ITEMCHANGED)
                {
                    selectionDetail(*s);
                    return 0;
                }
                if (header->code == NM_CUSTOMDRAW)
                {
                    auto *draw = reinterpret_cast<NMLVCUSTOMDRAW *>(lParam);
                    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
                        return CDRF_NOTIFYITEMDRAW;
                    if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
                    {
                        draw->clrTextBk = draw->nmcd.dwItemSpec % 2 ? RGB(249, 250, 253) : kCard;
                        draw->clrText = kInk;
                        return CDRF_NOTIFYSUBITEMDRAW;
                    }
                    if (draw->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM))
                    {
                        draw->clrText = kInk;
                        size_t row = draw->nmcd.dwItemSpec;
                        if (row < s->visible.size())
                        {
                            const auto index = s->visible[row];
                            const auto &p = s->plan.pairs[index];
                            const auto &r = s->results[index];
                            if (draw->iSubItem == 3 || draw->iSubItem == 4)
                                draw->clrText = p.genuine ? kGenuine : kImpostor;
                            if (draw->iSubItem == 5)
                                draw->clrText =
                                    failed(r) || (r.status == Status::Success &&
                                                  ((r.score >= s->threshold) != p.genuine))
                                        ? kDanger
                                    : r.status == Status::Pending ? kMuted
                                                                  : kGenuine;
                        }
                        return CDRF_DODEFAULT;
                    }
                }
            }
            break;
        case WM_DRAWITEM:
            if (s)
            {
                const auto &item = *reinterpret_cast<DRAWITEMSTRUCT *>(lParam);
                if (item.CtlID == EmptyTable)
                {
                    fill(item.hDC, item.rcItem, kCard);
                    SetBkMode(item.hDC, TRANSPARENT);
                    SetTextColor(item.hDC, kMuted);
                    auto old = SelectObject(item.hDC, s->fonts[Body]);
                    RECT r = item.rcItem;
                    DrawTextW(item.hDC, L"조건에 맞는 비교가 없습니다", -1, &r,
                              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    SelectObject(item.hDC, old);
                }
                else
                    drawButton(*s, item);
                return TRUE;
            }
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            if (s)
            {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, kInk);
                SetBkColor(dc, kCard);
                return reinterpret_cast<LRESULT>(s->white);
            }
            break;
        case WM_ERASEBKGND:
            return TRUE;
        case WM_PAINT:
            if (s)
            {
                PAINTSTRUCT paint{};
                HDC dc = BeginPaint(window, &paint);
                RECT area{};
                GetClientRect(window, &area);
                HDC buffer = CreateCompatibleDC(dc);
                HBITMAP bitmap = CreateCompatibleBitmap(dc, area.right, area.bottom);
                auto old = SelectObject(buffer, bitmap);
                drawDashboard(*s, buffer);
                BitBlt(dc, 0, 0, area.right, area.bottom, buffer, 0, 0, SRCCOPY);
                SelectObject(buffer, old);
                DeleteObject(bitmap);
                DeleteDC(buffer);
                EndPaint(window, &paint);
                return 0;
            }
            break;
        case WM_CLOSE:
            if (s)
                s->cancel = true;
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, 1);
            PostQuitMessage(0);
            return 0;
        }
    }
    catch (const std::exception &error)
    {
        if (message == WM_CREATE)
        {
            MessageBoxW(window, fromUtf8(error.what()).c_str(), L"데모 초기화",
                        MB_OK | MB_ICONERROR);
            return -1;
        }
        if (s)
        {
            s->status = fromUtf8(error.what());
            invalidate(*s);
        }
        MessageBoxW(window, fromUtf8(error.what()).c_str(), L"설정을 확인하세요",
                    MB_OK | MB_ICONINFORMATION);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

int runComparisonWindow(HINSTANCE instance, int showCommand)
{
    HMODULE common = LoadLibraryW(L"comctl32.dll");
    if (!common)
        return 1;
    auto initialize = reinterpret_cast<BOOL(WINAPI *)(const INITCOMMONCONTROLSEX *)>(
        GetProcAddress(common, "InitCommonControlsEx"));
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    if (!initialize || !initialize(&controls))
    {
        FreeLibrary(common);
        return 1;
    }
    WNDCLASSW type{};
    type.lpfnWndProc = windowProcedure;
    type.hInstance = instance;
    type.lpszClassName = kClassName;
    type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    type.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    AppState state;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT bounds{0, 0, 1280, 830};
    constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    AdjustWindowRect(&bounds, style, FALSE);
    int w = (std::min)(bounds.right - bounds.left, work.right - work.left),
        h = (std::min)(bounds.bottom - bounds.top, work.bottom - work.top);
    HWND window =
        RegisterClassW(&type)
            ? CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, L"Fingerprint Lab · 지문 비교 데모",
                              style, work.left + (work.right - work.left - w) / 2,
                              work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr,
                              instance, &state)
            : nullptr;
    if (!window)
    {
        FreeLibrary(common);
        return 1;
    }
    ShowWindow(window, showCommand);
    UpdateWindow(window);
    MSG message{};
    BOOL received;
    while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0)
        if (!IsDialogMessageW(window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    FreeLibrary(common);
    return received < 0 ? 1 : static_cast<int>(message.wParam);
}

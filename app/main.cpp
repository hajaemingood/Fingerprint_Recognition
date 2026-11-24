#include "evaluation_window.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        MessageBoxW(nullptr, L"Windows 구성 요소를 초기화할 수 없습니다.",
                    L"지문 인식 성능 평가", MB_OK | MB_ICONERROR);
        return 1;
    }

    const int result = runEvaluationWindow(instance, showCommand);
    CoUninitialize();
    return result;
}

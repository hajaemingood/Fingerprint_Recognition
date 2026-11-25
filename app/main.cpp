#include "comparison_window.h"

#ifdef _MSC_VER
#pragma comment(                                                                                   \
    linker,                                                                                        \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

// Entry point: initialize Windows image decoding, show the demo, then clean up.
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    {
        MessageBoxW(nullptr, L"Windows 이미지 구성 요소를 초기화하지 못했습니다.",
                    L"Fingerprint Lab", MB_OK | MB_ICONERROR);
        return 1;
    }
    int result = runComparisonWindow(instance, showCommand);
    CoUninitialize();
    return result;
}

# Fingerprint Recognition

## 로컬 UI 통합 브랜치

이 브랜치는 로컬 `main`의 DeepPrint DLL 및 FVC2004 영역 자르기 설정을 유지하면서,
`codex/deepprint-ui-merge`의 다섯 페이지 UI(설정 / 비교 결과 / 인식 성능 / 처리 성능 / 평가 요약)를 사용합니다.

### 새 PC에서 처음부터 실행 (Visual Studio 2022·2026 공통)

`build/`의 DLL·모델·런타임은 Git에 포함되지 않으므로, 클론 직후에는 아래 절차로 한 번 준비합니다.
`ui-*`, `deepprint-release` 프리셋은 Visual Studio 버전 이름에 묶이지 않는 Ninja + MSVC 구성입니다.
기존 `vs2022-*` 프리셋은 VS 2022 전용으로 그대로 유지합니다.

**필요한 것:** Visual Studio 2022 이상의 **C++를 사용한 데스크톱 개발** 워크로드(Windows용 C++ CMake 도구 포함), Git, Python 3.12

1. **모델과 ONNX Runtime 준비** — 일반 PowerShell에서 저장소 루트로 이동해 [DeepPrint 최초 빌드 준비](#deepprint-최초-빌드-준비)의 명령을 그대로 실행합니다.
   완료되면 `build/models/deepprint_texminu_512.onnx`, `build/models/parity/`, `build/dependencies/onnxruntime/`가 생깁니다.
2. **DLL과 UI 빌드** — Visual Studio에서 **로컬 폴더 열기**로 저장소 루트를 열고, 상단 구성에서 `deepprint-release`를 선택한 뒤
   **빌드 → 모두 빌드**를 실행합니다. 명령줄에서는 아래처럼 x64 개발자 셸에서 실행합니다.

   ```powershell
   # Developer PowerShell for VS는 기본이 x86이므로 x64로 전환합니다.
   & "$env:VSINSTALLDIR\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64
   cd <저장소 경로>
   cmake --preset deepprint-release
   cmake --build --preset deepprint-release
   ctest --preset deepprint-release   # 100% tests passed 확인
   ```

3. **실행** — `build/deepprint-release/`에 `fingerprint_compare.exe`, `IndustryAlgorithm.dll`,
   `deepprint_texminu_512.onnx`, `onnxruntime.dll`이 함께 생성됩니다. 앱을 실행하고 **알고리즘 DLL 선택**에서
   같은 폴더의 `IndustryAlgorithm.dll`을 선택합니다.

UI만 수정할 때는 `ui-release`(또는 `ui-debug`)를 선택합니다. 출력은 `build/ui-release/fingerprint_compare.exe`이며,
이후 받은 DLL만 교체하는 경우 `onnxruntime.dll`과 `deepprint_texminu_512.onnx`를 실행 파일 폴더에 한 번 두면 재사용됩니다.
MSYS2 MinGW g++ 환경에서는 `ui-release-mingw`를 사용하며 `g++`, `mingw32-make`가 PATH에 있어야 합니다.

| 프리셋 | 컴파일러 | 빌드 대상 | 출력 폴더 |
| --- | --- | --- | --- |
| `ui-release` / `ui-debug` | MSVC (VS 2022·2026) | UI | `build/ui-release`, `build/ui-debug` |
| `ui-release-mingw` | MSYS2 g++ | UI | `build/ui-release-mingw` |
| `deepprint-release` | MSVC (VS 2022·2026) | DeepPrint DLL + UI + 테스트 | `build/deepprint-release` |

**문제 해결**

- `cmake`를 찾을 수 없음: 일반 PowerShell이 아니라 Developer PowerShell(또는 VS 터미널)에서 실행합니다.
- `CMakeLists.txt:37`의 "DeepPrint requires a Windows x64 build": x86 셸에서 구성된 경우입니다. 위의 x64 전환 후
  `cmake --preset deepprint-release --fresh`를 실행하거나, VS에서 **프로젝트 → CMake 캐시 삭제 및 다시 구성**을 실행합니다.
- 빌드 직후 VS 테스트 메뉴에서 "Process not started"가 나오면 잠시 후 `ctest --preset deepprint-release`로 다시 확인합니다.

### 기존 VS 2022 전용 프리셋

VS 2022에서는 기존 `vs2022-*` 프리셋도 그대로 사용할 수 있습니다. UI만 개발하고 받은 DLL을 연결할 때는 다음 설정을 사용합니다.

```powershell
cmake --preset vs2022-ui-only
cmake --build --preset vs2022-ui-release
```

Visual Studio에서 F5로 실행할 때는 저장소 루트 폴더를 열고 다음을 설정합니다.

1. 상단 구성을 `vs2022-ui-release`로 선택하고 CMake 생성 완료를 기다립니다.
2. 상단 **시작 항목 선택**에서 `fingerprint_compare.exe (Release\fingerprint_compare.exe)`를 선택합니다. `(설치)` 항목은 선택하지 않습니다.
3. 시작 항목이 보이지 않으면 솔루션 탐색기의 **솔루션과 사용 가능한 보기 간 전환 → CMake 대상 뷰**로 전환합니다.
4. **F5**를 누르면 Visual Studio가 UI를 빌드하고 디버깅을 시작합니다.

시작 항목 선택은 PC별 Visual Studio 설정입니다. 새로 클론한 팀원도 처음 한 번 설정해야 합니다.

출력은 `build/vs2022-ui/Release/fingerprint_compare.exe`입니다. 앱 화면은 DLL이 없어도 열립니다.
평가 시 알고리즘 팀에서 받은 x64 DLL과 필요한 모델·런타임을 준비하고 앱에서 DLL과 DB를 선택합니다.
DeepPrint의 공통 모델·런타임은 선택 DLL 폴더 또는 실행 파일 폴더에 두면 됩니다.
저장소의 `build/`, `dist/`, `.vs/`를 다른 PC의 빌드 캐시로 재사용하지 않습니다.
배포 폴더를 옮겨 실행하는 것과 저장소를 새로 클론해 컴파일하는 것은 별도 절차입니다.

Visual Studio에서 `vs2022-debug`를 선택하고 시작 항목을 `fingerprint_compare.exe`로 지정합니다.
기본 `vs2022-x64` 구성은 **DeepPrint DLL과 UI를 함께 빌드**합니다.
명령줄에서는 `cmake --preset vs2022-x64` 후 `cmake --build --preset vs2022-debug`를 실행합니다.
출력 위치는 `build/vs2022-deepprint/Debug/`입니다.

실행 폴더에는 `fingerprint_compare.exe`, `IndustryAlgorithm.dll`,
`deepprint_texminu_512.onnx`, `onnxruntime.dll`이 함께 있어야 합니다.
모델과 런타임은 `build/models/`, `build/dependencies/onnxruntime/`에 준비하고
빌드 시 실행 폴더로 자동 복사합니다. 새 PC에서 저장소를 내려받을 경우 이 파일들은
Git에 포함되어 있지 않으므로 아래 최초 빌드 준비 절차를 먼저 수행해야 합니다.
모델 기준 벡터는 `build/models/parity/`에 저장하고 CTest로 실제 C++ 추론과 대조합니다.

### 앱 사용

앱은 **01 실험 설정 → 02 비교 실행·결과 → 03 인식 성능 → 04 처리 성능 → 05 평가 요약** 다섯 페이지로 구성됩니다.

1. **01 실험 설정**에서 **DB 폴더 선택…**과 **알고리즘 DLL 선택…**으로 DB와 DLL을 지정합니다.
2. 등록 이미지, 본인 비교(같은 손가락), 타인 비교(다른 손가락) 규칙과 동시 비교 수(1~16)를 정하고 **새 비교 목록 만들기**를 누릅니다.
   저장해 둔 비교 목록 CSV는 **불러오기**로 그대로 사용할 수 있습니다.
3. **02 비교 실행·결과**에서 비교를 실행하고, 진행 중에는 **비교 중지**로 멈출 수 있습니다.
4. **03~05** 페이지에서 지표와 그래프를 확인하고 **보고서 저장**으로 HTML 보고서를 저장합니다.

앱에서 **DB 폴더**와 **알고리즘 DLL**을 직접 선택합니다. DLL은 x64이며 공통 KISA
`KISA_Enroll`, `KISA_Attempt`, `KISA_Compare` C ABI와 `__cdecl` 호출 규약을 따라야 합니다.
템플릿은 최대 1,000,000바이트의 불투명 데이터로 취급하며 정합 점수는 높을수록 유사한 0~1 값입니다.
DLL의 종속 라이브러리와 모델은 선택한 DLL 폴더를 우선 사용합니다. DeepPrint의 경우
`onnxruntime.dll`과 `deepprint_texminu_512.onnx`가 필요하며, 선택한 DLL 폴더에 없으면
실행 파일 폴더의 공통 파일을 자동 재사용합니다. 앱은 임시 폴더에 DLL 버전별 실행용
복사본을 준비하므로 다운로드 폴더 원본은 바꾸지 않습니다. 새로운 모델이나 런타임을
요구하는 버전은 해당 파일도 함께 준비해야 합니다. 다른 KISA DLL은 자체 종속 파일 구성을 유지합니다.
기존 배포 폴더에서 DLL만 직접 교체할 때는 앱을 종료한 뒤 교체하고 다시 실행합니다.
처음에는 동시 비교 수 1로 실행하고 DLL의 병렬 호출 지원을 확인한 뒤 늘립니다.
DB는 한 폴더의 `손가락번호_촬영번호.tif` 또는 `.tiff` 파일을 사용합니다.

앱은 매 비교마다 테스트 이미지의 특징을 추출합니다. 비교 실행 시 인식 성능 화면으로 이동하고,
하단 이전·다음 버튼으로 인식 성능 → 처리 성능 → 평가 요약 → 보고서 저장을 이어갈 수 있습니다.
비교 쌍 CSV는 실제 순서를 저장하며, 다른 PC에서 원래 DB 경로가 없으면 같은 이름의 DB를
먼저 선택한 뒤 불러올 수 있습니다. CSV를 불러와도 선택한 DLL은 자동으로 바뀌지 않습니다.

평가 요약은 **핵심 결과 / 상세 지표 / 그래프**로 나눕니다.
평가 후 **보고서 저장**에서 UTF-8 HTML 파일을 저장하면 지표, 실험 설정, SVG 그래프,
비교별 결과를 외부 리소스 없이 확인할 수 있습니다. 보고서의 인쇄 버튼으로 PDF 저장도 가능합니다.
중단한 실행은 부분 결과로 표시하며, 처리 실패와 미완료 행을 보고서에 보존합니다.
EER 계열 지표는 선택 목록 기준이며 공식 FVC 순위와 직접 비교할 수 없습니다.
RAM은 앱·모델·캐시 전체의 Working Set을 관측한 값입니다.

로컬 배포 묶음은 `dist/FingerprintDemo/`에 준비합니다. `build/`, `dist/`, 모델과 런타임 DLL은
Git에 포함되지 않습니다. 다른 PC에는 실행 파일뿐 아니라 배포 폴더 전체를 옮깁니다.

---


## 프로젝트 목표

고급 객체지향 프로그래밍(SME3007) 과제입니다. Windows용 C++17 DeepPrint DLL로 지문 정합 점수를 계산하고, 평가 앱에서 선택한 KISA DLL을 FVC 데이터로 시험해 CSV 로그와 HTML 보고서를 생성합니다.

| 사용 목적 | 빌드 또는 실행 경로 |
| --- | --- |
| Windows 성능 평가 | `fingerprint_compare.exe`에서 DLL, FVC DB, 비교 규칙을 선택하고 HTML 보고서 저장 |
| DeepPrint DLL 빌드 | `FINGERPRINT_DEEPPRINT=ON`으로 `IndustryAlgorithm.dll`과 런타임 파일 생성 |
| 노이즈·회전·밝기 강건성 평가 | `augment_fvc.exe`로 증강 영상을 만들고 `evaluate_noise.exe`로 같은 쌍을 일괄 비교 |

KISA 인터페이스는 [include/kisa_api.h](include/kisa_api.h)에 정의되어 있습니다.

| 함수 | 역할 |
| --- | --- |
| `KISA_Enroll` | 등록 영상 한 장에서 특징량을 추출해 등록 템플릿을 생성합니다. |
| `KISA_Attempt` | 비교 영상 한 장에서 특징량을 추출해 비교 템플릿을 생성합니다. |
| `KISA_Compare` | 두 템플릿을 비교해 0~1 범위의 정합 점수를 반환합니다. |

DLL은 좌상단부터 연속으로 저장된 8비트 회색조 픽셀 배열을 받습니다. 호출자는 등록·비교용 템플릿 버퍼를 각각 1,000,000바이트, 오류 메시지 버퍼를 256바이트로 할당합니다. 템플릿 쌍은 같은 알고리즘 빌드에서 생성해야 합니다. DeepPrint 템플릿은 512차원 특징과 형식 헤더를 포함해 2,056바이트입니다.

## 파일 구조

```text
Advanced_OOP/
├── README.md
├── CMakeLists.txt                    # DLL, 평가 앱, 일괄 평가 도구, 테스트
├── CMakePresets.json                 # Visual Studio·MinGW 빌드 프리셋
├── .gitignore                        # 로컬 빌드 및 실험 산출물 제외
├── .vscode/
│   ├── tasks.json                    # VS Code 빌드 작업
│   └── launch.json                   # 평가 앱 디버그 실행
├── include/
│   ├── kisa_api.h                    # KISA 공개 함수와 버퍼 규약
│   └── image_format.h                # iType별 입력 영상 크기
├── src/
│   ├── api/kisa_api.cpp              # DLL 진입점과 오류 처리
│   ├── feature/
│   │   ├── feature_extractor.h       # 특징 추출 인터페이스
│   │   ├── deepprint_extractor.cpp    # ONNX Runtime CPU 추론
│   │   ├── deepprint_preprocessing.{h,cpp}
│   │   ├── fingerprint_features.h    # 512차원 특징 자료형
│   │   └── template_manager.{h,cpp}   # 템플릿 직렬화·복원
│   ├── experiments/fvc_augmentation.json # 알고리즘 실험의 증강 조건
│   └── matching/fingerprint_matcher.{h,cpp}
├── app/
│   ├── main.cpp                      # Windows GUI 진입점
│   ├── comparison_window.{h,cpp}     # 다섯 페이지 UI, 비교 실행·중지, 보고서 저장
│   └── fingerprint_image.{h,cpp}     # TIFF 읽기, DLL 로드, 비교 목록·지표 계산
├── tools/
│   ├── deepprint/export_model.py     # 오프라인 모델 변환과 기준 벡터 생성
│   ├── fvc/
│   │   ├── fvc_dataset.{h,cpp}       # FVC 파일 목록과 증강 매니페스트
│   │   └── tiff_image.{h,cpp}        # TIFF 입출력
│   ├── noise/
│   │   ├── main.cpp                  # augment_fvc 실행 진입점
│   │   ├── gaussian_noise.{h,cpp}
│   │   └── image_transform.{h,cpp}   # 노이즈, 회전, 밝기, 반전 클래스
│   └── evaluation/
│       ├── kisa_library.h            # 공용 DLL 로더
│       ├── verification_metrics.{h,cpp}
│       ├── noise_evaluation.cpp      # evaluate_noise 실행 진입점
│       └── weight_tuning.cpp         # 저장 템플릿 기반 정합 가중치 실험
├── tests/
│   ├── evaluation_fixture.cpp        # upstream 워커 UI용 (이 브랜치에서는 빌드하지 않음)
│   ├── test_evaluation.cpp           # upstream 워커 UI용 (이 브랜치에서는 빌드하지 않음)
│   ├── test_deepprint.cpp            # DeepPrint 기준 벡터 비교
│   └── test_noise.cpp                # 증강 변환과 지표 계산 검증
├── data/fvc/                         # 원본 FVC TIFF 240장
│   ├── FVC2000_DB3_B/                # iType 1
│   ├── FVC2002_DB2_B/                # iType 2
│   └── FVC2004_DB1_B/                # iType 3
├── reference/                        # 과제 설명서, KISA 안내서, 논문, 원본 ZIP
├── reports/
│   ├── fingerprint_evaluation.html   # 평가 완료 시 갱신되는 최신 HTML 보고서
│   └── assets/                       # 이전 강건성 실험의 SVG 그래프
├── archive/
│   ├── archive-log.csv               # 원래 경로, 보관 경로, DLL 해시와 결과 위치
│   └── histogram/
│       ├── builds/                   # 구형 Release·noise-baseline 빌드 (Git 제외)
│       └── runs/                     # run_1 및 이전 히스토그램 평가 기록
├── log/                              # 가중치 실험 설정, 데이터 분할, 결과
└── build/                            # EXE, DLL, 모델, 증강과 평가 결과 (Git 제외)
```

## 팀별 작업 범위

| 경로 | 담당 범위 |
| --- | --- |
| `app/` | Windows 평가 앱. 시험 조건 입력, DLL 실행 감독, 로그·보고서 출력 |
| `src/` | 알고리즘 DLL 팀. 특징 추출, 템플릿 저장·복원, 매칭, KISA 함수 구현 |
| `include/` | 공동 인터페이스. KISA 함수 선언, 버퍼 규약, `iType`별 영상 크기 |
| `data/fvc/`, `tests/` | 공유 평가 데이터와 DLL 연동 테스트 |
| `tools/` | DeepPrint 모델 변환, TIFF 노이즈 생성, KISA DLL 일괄 평가 |
| `reports/` | 최신 평가 HTML과 이전 실험의 SVG 그래프 |

평가 앱은 선택한 x64 DLL을 임시 폴더에 복사한 뒤 앱 프로세스에서 동적으로 불러옵니다. 명령줄 평가기는 실행 인자로 받은 DLL을 사용합니다. 두 평가기는 공개 KISA 함수로 템플릿을 만들고 비교합니다. 평가 팀은 `FINGERPRINT_DEEPPRINT=OFF`(`ui-release` 프리셋)로 앱을 독립적으로 빌드할 수 있습니다.

## 구형 빌드와 평가 기록

현재 앱과 DeepPrint DLL은 `build/evaluator/`를 기준으로 실행합니다. 구형 히스토그램의 빌드 산출물은 `archive/histogram/builds/`에, 평가 당시의 HTML·CSV·설정·그래프는 `archive/histogram/runs/`에 보관합니다.

이전 보고서에 적힌 DLL 경로는 보관 로그의 `original_path`에서 찾을 수 있습니다. 보관 로그에는 이동 날짜, 옮긴 경로, 확인된 DLL SHA-256, 평가 결과 위치와 확인 근거가 기록됩니다. [보관 로그](archive/archive-log.csv) · [run_1 보고서](archive/histogram/runs/run_1/report.html)

## FVC 데이터

`data/fvc/`에는 평가에 사용하는 원본 TIFF 240장이 있습니다. 원본 ZIP은 `reference/프로젝트 관련 자료/`에 있습니다.

| 폴더 | `iType` | 영상 크기 | 파일 수 |
| --- | --- | --- | --- |
| [FVC2000_DB3_B](data/fvc/FVC2000_DB3_B/) | 1 | 448 × 478 | 80 |
| [FVC2002_DB2_B](data/fvc/FVC2002_DB2_B/) | 2 | 300 × 300 | 80 |
| [FVC2004_DB1_B](data/fvc/FVC2004_DB1_B/) | 3 | 640 × 480 | 80 |

파일명 `101_2.tif`에서 `101`은 손가락 번호, `2`는 해당 손가락의 촬영 번호입니다. 손가락 번호는 같은 DB 안에서 비교합니다.

DLL의 `iType` 매핑은 FVC용 1·2·3과 KISA 안내서의 센서용 5·7·8·9를 포함합니다. 평가 앱과 일괄 평가 도구는 위 세 FVC 크기를 읽습니다. [입력 규격](include/image_format.h)

저장소의 `FVC2002_DB2_B/` 폴더에 있는 80장은 동봉된 `FVC2002_DB3_B.zip`의 TIFF와 바이트 단위로 일치합니다. 일괄 평가 CSV에는 저장소의 실제 폴더명이 기록됩니다.

## 빌드 및 실행

### DeepPrint DLL과 평가 앱 빌드

Visual Studio에서는 위의 [새 PC에서 처음부터 실행](#새-pc에서-처음부터-실행-visual-studio-20222026-공통) 절차와 `deepprint-release` 프리셋을 사용합니다. 아래는 VS Code와 MSYS2 UCRT64의 `g++`, `mingw32-make`, CMake·CTest 3.20 이상을 사용하는 방법입니다. DeepPrint 빌드에는 `build/models/deepprint_texminu_512.onnx`와 `build/dependencies/onnxruntime/`가 필요합니다. [DeepPrint 최초 빌드 준비](#deepprint-최초-빌드-준비)

평가 앱이 열려 있다면 빌드 전에 창을 닫습니다. **VS Code**에서 저장소 폴더를 열고 `Ctrl+Shift+B`를 눌러 **Build DeepPrint and evaluation app**을 실행합니다.

**PowerShell**을 사용한다면 저장소 루트에서 먼저 CMake 경로를 설정합니다. 시스템 CMake가 설치돼 있으면 이를 사용하고, 그 외에는 프로젝트에 설치한 로컬 CMake를 사용합니다.

```powershell
$installedCmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
$cmake = if ($installedCmake) { $installedCmake.Source } else {
    '.\build\dependencies\python\Lib\site-packages\cmake\data\bin\cmake.exe'
}
```

같은 터미널에서 DLL과 평가 앱을 빌드합니다.

```powershell
& $cmake -S . -B build/evaluator -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe `
  -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/mingw32-make.exe `
  -DFINGERPRINT_BUILD_GUI=ON -DFINGERPRINT_DEEPPRINT=ON -DFINGERPRINT_FVC2004_ROI=ON `
  -DFINGERPRINT_DEEPPRINT_REFERENCE_DIR=build/models/parity
& $cmake --build build/evaluator --parallel 4
```

빌드 산출물은 `build/evaluator/`에 생성됩니다.

| 산출물 | 역할 |
| --- | --- |
| `fingerprint_compare.exe` | 다섯 페이지 평가 앱 |
| `IndustryAlgorithm.dll` | DeepPrint의 KISA 함수 구현과 FVC2004 영역 자르기 |
| `deepprint_texminu_512.onnx`, `onnxruntime.dll` | DeepPrint 모델과 추론 런타임 |
| `augment_fvc.exe`, `evaluate_noise.exe` | 영상 증강과 명령줄 일괄 평가 |
| `tune_weights.exe` | 저장된 DeepPrint 템플릿으로 질감·특징점 가중치 비교 |

DeepPrint DLL을 복사할 때는 ONNX 모델과 런타임 DLL도 DeepPrint DLL 옆에 둡니다.

### 평가 앱만 빌드

평가 팀이 별도로 빌드한 KISA DLL을 사용할 때는 위에서 설정한 `$cmake`로 아래 명령을 실행합니다.

```powershell
& $cmake -S . -B build/evaluator-only -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe `
  -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/mingw32-make.exe `
  -DFINGERPRINT_BUILD_GUI=ON -DFINGERPRINT_DEEPPRINT=OFF
& $cmake --build build/evaluator-only --parallel 4 --target fingerprint_compare
.\build\evaluator-only\fingerprint_compare.exe
```

앱은 x64 DLL만 불러옵니다. x86(32비트) DLL을 선택하면 오류를 표시합니다.

### 평가 앱 사용

앱 화면 구성과 사용 순서는 위의 [앱 사용](#앱-사용)을 참고합니다. MinGW로 빌드했다면 아래 명령으로 실행합니다.

```powershell
.\build\evaluator\fingerprint_compare.exe
```

- 비교 목록은 02 페이지의 **전체 비교 쌍과 순서 저장**으로 CSV에 저장하고, 01 페이지의 **불러오기**로 같은 쌍과 순서를 다시 평가할 수 있습니다.
- 평가 결과는 **보고서 저장**에서 지정한 위치에 HTML 한 파일로 저장합니다. 템플릿·로그 폴더를 따로 만들지 않으며,
  `reports/fingerprint_evaluation.html`을 자동으로 갱신하지 않습니다.

### 자동 테스트

위의 PowerShell 빌드 명령을 실행한 터미널에서 CTest를 실행합니다.

```powershell
& (Join-Path (Split-Path -Parent $cmake) 'ctest.exe') --test-dir build/evaluator --output-on-failure
```

Visual Studio 프리셋으로 빌드했다면 `ctest --preset deepprint-release`를 실행합니다.

`noise_and_metrics`는 증강과 평가 지표를 검사합니다. `deepprint_reference`는 `build/models/parity/`의 기준 벡터를 사용해 DeepPrint의 C++ 추론 결과를 대조합니다.

## DeepPrint DLL과 노이즈 평가

DeepPrint DLL은 FVC 회색조 픽셀을 흰색 패딩과 안티앨리어싱 리사이즈로 `299×299`에 맞춘 뒤, ONNX 모델에서 `flx`의 FVC 이진화와 추론을 수행합니다. 질감 256개와 minutiae 256개를 합친 벡터는 형식 헤더를 포함해 2,056바이트입니다. 비교 시 두 분기의 코사인 점수를 각각 `0~1`로 변환한 뒤 질감 0.22와 특징점 0.78의 가중합을 소수점 셋째 자리까지 반환합니다. [정합 가중치 정의](src/matching/fingerprint_matcher.h)

### DeepPrint 최초 빌드 준비

위 CMake 환경(Visual Studio 또는 MSYS2)에 더해 Git과 CPython 3.12가 필요합니다. 아래 명령은 일반 PowerShell에서 저장소 루트를 기준으로 실행합니다. Python은 최초 모델 변환과 기준 벡터 생성에 사용하며, 빌드된 DLL의 추론은 ONNX Runtime CPU로 실행합니다.

```powershell
# 소스와 가중치를 저장할 로컬 경로
New-Item -ItemType Directory -Force build/dependencies | Out-Null
git clone https://github.com/tim-rohwedder/fixed-length-fingerprint-extractors.git build/dependencies/flx
git -C build/dependencies/flx checkout --detach 7accfca1f33b9b42bfd220e43cd5bc13b4a7fa13
curl.exe -fL "https://drive.usercontent.google.com/download?id=1R9v73hFBuy0iihz7v8eDlBcGrMDmQ9TH&export=download&confirm=t" -o build/dependencies/best_model.pyt

py -3.12 -m venv build/dependencies/export-env
$exportPython = '.\build\dependencies\export-env\Scripts\python.exe'
& $exportPython -m pip install numpy==1.26.4 pillow==11.1.0 opencv-python-headless==4.11.0.86 onnx==1.17.0 onnxruntime==1.20.1
& $exportPython -m pip install --index-url https://download.pytorch.org/whl/cpu torch==2.6.0+cpu torchvision==0.21.0+cpu
& $exportPython -B tools/deepprint/export_model.py --flx build/dependencies/flx --checkpoint build/dependencies/best_model.pyt

# Windows 네이티브 C 헤더와 런타임 DLL
curl.exe -fL "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/1.20.1/microsoft.ml.onnxruntime.1.20.1.nupkg" -o build/dependencies/onnxruntime.zip
Expand-Archive build/dependencies/onnxruntime.zip -DestinationPath build/dependencies/onnxruntime
```

변환기는 체크아웃 커밋과 체크포인트 SHA-256을 검사하고, FVC 세 종류에서 PyTorch와 ONNX의 출력 일치를 확인한 뒤 `build/models/`에 ONNX와 기준 벡터를 저장합니다. 원본 코드의 라이선스는 LGPL-3.0입니다. 체크포인트와 변환 모델은 로컬 빌드 경로에서 사용하며, 팀 저장소에 가중치를 배포할 경우 별도 배포 조건을 확인합니다. [flx 저장소와 라이선스](https://github.com/tim-rohwedder/fixed-length-fingerprint-extractors) · [공개 체크포인트](https://drive.google.com/file/d/1R9v73hFBuy0iihz7v8eDlBcGrMDmQ9TH/view)

모델이나 ONNX Runtime을 다른 폴더에 준비했다면 CMake에서 각각 `FINGERPRINT_DEEPPRINT_MODEL`, `FINGERPRINT_ONNXRUNTIME_ROOT`에 해당 경로를 지정합니다. [DeepPrint DLL과 평가 앱 빌드](#deepprint-dll과-평가-앱-빌드)

### 노이즈 TIFF 생성 및 일괄 평가

아래 명령의 `build\evaluator`는 MinGW 빌드 경로입니다. Visual Studio 프리셋으로 빌드했다면 `build\deepprint-release`로 바꿉니다.

아래 예시는 기준 시드 `42`와 8비트 픽셀 기준 가우시안 노이즈 `σ=2, 5, 10`을 사용합니다. 각 실행의 출력은 새 폴더에 저장합니다.

```powershell
.\build\evaluator\augment_fvc.exe data/fvc build/augmentation/fvc-seed42 42 2 5 10
.\build\evaluator\evaluate_noise.exe data/fvc build/augmentation/fvc-seed42 build/evaluator/IndustryAlgorithm.dll build/evaluation/deepprint-seed42
```

`build/augmentation/fvc-seed42/manifest.csv`가 이미 있으면 같은 증강본을 재사용할 수 있습니다. 평가만 반복할 때는 두 번째 명령의 마지막 인자에 새 결과 폴더를 지정합니다.

- `augment_fvc.exe`는 원본 크기에서 노이즈를 더하고 픽셀을 `0~255`로 제한합니다. 노이즈 강도 인자를 생략하면 `2, 5, 10`을 사용합니다. 기본 설정에서는 TIFF 720장과 `manifest.csv`를 생성하며, 자동으로 포함되는 `σ=0` 조건은 원본 TIFF를 가리킵니다. 같은 실행 파일과 시드에서는 같은 픽셀이 생성됩니다.
- `evaluate_noise.exe`는 원본 등록 템플릿을 재사용하고, 비교 영상으로 해당 노이즈 강도의 TIFF를 읽습니다. DB와 강도 조합마다 같은 손가락 280쌍과 다른 손가락 2,880쌍을 비교합니다. `pairs.csv`에는 쌍별 점수와 실패 상태를 기록하고, `summary.csv`에는 성공 점수의 선형보간 EER, 실패율, 평균 시간, 템플릿 크기를 기록합니다. 오류율은 0~1 비율입니다.

### 회전·밝기를 포함한 강건성 평가

저장소 루트에서 아래 명령을 실행합니다. 출력 폴더에는 새 이름을 지정합니다.

```powershell
.\build\evaluator\augment_fvc.exe data/fvc build/augmentation/fvc-robustness --robustness
.\build\evaluator\evaluate_noise.exe data/fvc build/augmentation/fvc-robustness build/evaluator/IndustryAlgorithm.dll build/evaluation/deepprint-robustness
```

`--robustness`는 원본 한 장마다 가우시안 노이즈 6종(`σ = 2, 5, 10`과 시드 `42, 43`), 회전 4종(`−10°, −5°, +5°, +10°`), 밝기 2종(픽셀값 `−15, +15`)을 각각 생성합니다. 양수 각도는 반시계 방향이며, 회전 영상은 원본 크기를 유지하고 바깥쪽을 흰색으로 채웁니다. FVC 원본 240장 기준 증강본은 2,880장입니다. `--robustness --flip`은 좌우 반전 240장을 추가합니다.

평가기에서 원본으로 등록하고 같은 지문 쌍을 조건마다 비교합니다. `pairs.csv`의 `transform`, `parameter`, `probe_original`로 각 결과가 어느 원본과 변환에 해당하는지 볼 수 있습니다. `summary.csv`에는 조건별 과제 EER과 DB별 원본 EER에서 선택한 임계값(`reference_threshold`)의 FMR·FNMR이 기록됩니다.

## 정합 가중치 실험

단일 워커로 완료한 원본 평가의 `pairs.csv`와 저장 템플릿을 사용합니다. 아래는 FVC 원본 9,480쌍으로 다시 실행하는 예시입니다. 출력 폴더에는 새 이름을 지정합니다.

```powershell
$run = 'run-1795498198097-1824-1'
.\build\evaluator\tune_weights.exe data/fvc "build/app-results/logs/$run/pairs.csv" `
  "build/app-results/templates/$run/worker-1" log/weight-tuning-rerun
```

도구는 손가락 ID를 시드 42로 나누고, 개발 데이터에서 질감 가중치 0.00~1.00을 0.01 간격으로 시험합니다. 개발 데이터의 DB별 EER 평균으로 후보를 선택하고, 확인 데이터에서 기존 연결 코사인과 선택한 가중치를 비교합니다. 운영 임계값은 개발 단계에서 각 DB의 FMR이 1% 이하인 최소 공통 값으로 정합니다.

`log/`의 실험 폴더에서 `split.csv`는 손가락별 분할, `development.csv`는 가중치 후보별 결과, `comparison.csv`는 확인·전체 결과, `summary.txt`는 입력 경로와 선택 기준을 기록합니다. CSV의 오류율 단위는 0~1입니다. [22:78 실험 기록](log/weight-tuning-20261124-164133/summary.txt)

## 촬영본 6:2와 마지막 ID 평가

FVC2002·2004에서 DB별 최대 손가락 ID를 최종 평가용으로 지정합니다. 나머지 9개 ID마다 촬영본 6장을 미세조정에, 2장을 새 촬영본 평가에 사용합니다. 개발 단계에서는 각 ID의 6장 중 5장으로 후보를 학습하고 1장으로 설정과 임계값을 선택합니다. 선택 후에는 6장 전체로 다시 학습합니다.

최종 평가에서는 학습에 사용한 ID마다 학습용 원본 1장으로 등록하고 남겨둔 2장으로 비교합니다. 마지막 ID는 첫 원본으로 새로 등록하고 나머지 7장을 본인 및 다른 9개 ID의 등록 템플릿과 비교합니다. 실험은 같은 50:50 정합 가중치로 기준 모델과 학습 후보를 평가합니다. 현재 DLL의 22:78 결과는 참고값으로 기록합니다.

학습과 검증의 증강은 노이즈 `σ=2, 5, 10, 20, 30`, 회전 `±5°, ±10°`, 밝기 `±15`를 사용합니다. 같은 노이즈 강도에서는 시드 42·43의 지표를 먼저 평균하고, 결과를 회전·밝기를 포함한 11개 설정에 같은 비중으로 평균합니다. `evaluation.csv`는 원본 결과와 증강 평균 EER·FMR·FNMR을 각각 한 행으로 저장합니다. 증강 평균 행의 비교 쌍 수는 16개 영상 변형에서 비교한 건수의 합입니다. [증강 설정](src/experiments/fvc_augmentation.json)

캐시는 [임베딩 캐시 도구](tools/deepprint/cache_fvc_embeddings.py)에 `--robustness` 증강 폴더와 시드별 `20 30` 노이즈 증강 폴더 두 개를 각각 `--augmentation`으로 넘겨 생성합니다. 기존 `fvc-noise40-cache`도 사용할 수 있으며 이 실험은 설정 파일에 포함된 조건만 읽습니다. 저장소 루트에서 실행하고 매번 새 출력 경로를 지정합니다.

```powershell
$exportPython = '.\build\dependencies\export-env\Scripts\python.exe'
& $exportPython -B tools/deepprint/finetune_last_id.py `
  --model build/evaluator/deepprint_texminu_512.onnx `
  --cache build/finetune/fvc-noise40-cache `
  --output log/finetune-last-id-6-2-rerun `
  --candidate build/finetune/finetune-last-id-6-2-rerun/deepprint_texminu_512.onnx `
  --dll build/evaluator/IndustryAlgorithm.dll
```

실험 로그에는 촬영본 분할, 개발 점수, 원본·증강 평균 지표, C++ DLL 템플릿 검증이 기록됩니다. [σ=30 증강 평균 결과](log/finetune-last-id-sigma30-mean-20261125-053138/evaluation.csv) · [이전 σ=40 실행 결과](log/finetune-last-id-6-2-20261125-051916/evaluation.csv)

## FVC2004 영역 자르기

기본 설정으로 빌드한 `IndustryAlgorithm.dll`은 `iType=3` 영상의 지문 영역을 자른 뒤 패딩과 리사이즈를 적용합니다. 전경이 충분히 검출되지 않으면 전체 프레임을 사용합니다. 평가 앱의 **알고리즘 DLL 선택**에서 빌드한 DLL을 지정합니다.

기존 전체 프레임 방식이 필요하면 별도 빌드 디렉터리에서 `-DFINGERPRINT_FVC2004_ROI=OFF`를 지정합니다. 두 빌드 모두 같은 사전학습 ONNX와 22:78 정합 가중치를 사용합니다. 영역 자르기를 활성화한 기준 벡터 테스트는 `build/models/parity/3.roi.*.f32`도 읽습니다. 빌드 준비 단계의 모델 내보내기 명령이 이 파일들을 생성합니다. [활성화 전 C++ DLL 비교](log/fvc2004-roi-validation-20261125/conclusion.txt)

## 평가 보고서

평가 앱의 **보고서 저장**은 인식 지표, 처리 실패, 처리 시간, 그래프와 비교별 결과를 HTML 한 파일에 기록합니다. [이전 평가 보고서](reports/fingerprint_evaluation.html)

이전 원본·가우시안 노이즈 실험에서 생성한 그래프는 `reports/assets/`에 있습니다. [점수 분포](reports/assets/score-distributions.svg) · [DET](reports/assets/det-curves.svg) · [임계값별 오류율](reports/assets/threshold-curves.svg)

| 원본 DB | 가중치 튜닝 전 DeepPrint EER |
| --- | ---: |
| FVC2000 | 2.50% |
| FVC2002 | 7.99% |
| FVC2004 | 12.27% |

표는 과제 8쪽의 교차점 계산법을 적용한 결과입니다. 노이즈 전용 실행의 `summary.csv`는 선형보간을 사용하므로 일부 EER 값이 다릅니다. FVC2004에서 DeepPrint의 오수락률을 0.1% 이하로 제한하면 관측 본인 거부율은 91.07%였습니다.

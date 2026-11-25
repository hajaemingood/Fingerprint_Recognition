# Fingerprint Recognition

## 프로젝트 목표

고급 객체지향 프로그래밍(SME3007) 과제로, 등록 지문과 비교 지문의 정합 점수를 반환하는 Windows용 C++17 DLL을 개발합니다. DLL은 KISA 지문인식 성능시험 인터페이스에 맞춰 다음 세 함수를 제공합니다.

| 함수 | 역할 |
| --- | --- |
| `KISA_Enroll` | 등록 영상 한 장에서 특징량을 추출해 등록 템플릿을 생성합니다. |
| `KISA_Attempt` | 비교 영상 한 장에서 특징량을 추출해 비교 템플릿을 생성합니다. |
| `KISA_Compare` | 두 템플릿을 비교해 0~1 범위의 정합 점수를 반환합니다. |

과제의 성능평가 목표는 FVC 2000·2002·2004 데이터로 동일인·타인 비교를 수행하고 FMR, FNMR, EER, 등록 실패율(FTE), 처리 시간, 템플릿 크기를 측정하는 것입니다. 현재 DLL은 16바이트 밝기 히스토그램으로 등록부터 비교까지의 흐름을 검증하는 초안입니다. 개선 ORB 기반 rBRIEF·LBP 특징 추출과 실제 FVC 성능평가는 후속 구현 범위입니다.

## 파일 구조

```text
Advanced_OOP/
├── CMakeLists.txt                   # DLL, 비교 앱, 테스트 빌드 정의
├── include/
│   ├── kisa_api.h                   # KISA 공개 함수 선언과 버퍼 규약
│   └── image_format.h               # 공용 iType별 입력 영상 크기
├── src/
│   ├── api/kisa_api.cpp             # DLL 진입점, 인자 검증, 오류 처리
│   ├── feature/
│   │   ├── feature_extractor.h/.cpp # 특징 추출
│   │   ├── fingerprint_features.h   # 특징량 자료형
│   │   └── template_manager.h/.cpp  # 템플릿 저장·복원
│   └── matching/
│       └── fingerprint_matcher.h/.cpp # 특징량 비교
├── app/
│   ├── main.cpp                     # Windows GUI 시작
│   ├── comparison_window.h/.cpp     # 두 파일 선택과 비교 점수 표시
│   └── fingerprint_image.h/.cpp     # FVC TIFF 픽셀 읽기
├── tests/test_pipeline.cpp          # DLL 전체 호출 흐름 테스트
├── data/fvc/                       # 비교 앱에서 바로 선택할 FVC TIFF
│   ├── FVC2000_DB3_B/               # iType 1
│   ├── FVC2002_DB3_B/               # iType 2
│   └── FVC2004_DB1_B/               # iType 3
├── reference/                      # 과제 PDF, KISA 안내서, 논문, 원본 FVC ZIP
└── build/Release/                  # VS Code 빌드 결과
    ├── IndustryAlgorithm.dll
    ├── fingerprint_compare.exe
    └── test_pipeline.exe
```

DLL에는 파일 경로 대신 디코딩된 8비트 회색조 픽셀 배열을 전달합니다. 과제용 FVC 영상은 `iType` 1·2·3, 안내서의 센서 영상은 5·7·8·9를 사용합니다.

## 팀별 작업 범위

| 경로 | 담당 범위 |
| --- | --- |
| `app/` | 평가 프로그램 팀. TIFF 디코딩, 파일 선택, 점수 표시, 향후 평가 지표 집계 |
| `src/` | 알고리즘 DLL 팀. 특징 추출, 템플릿 저장·복원, 매칭, KISA 함수 구현 |
| `include/` | 공동 인터페이스. KISA 함수 선언, 버퍼 규약, `iType`별 영상 크기 |
| `data/fvc/`, `tests/` | 공유 평가 데이터와 DLL 연동 테스트 |

평가 앱은 `include/`의 공용 헤더로 컴파일하고, 빌드된 DLL의 가져오기 라이브러리에 링크합니다. 실행 시 `IndustryAlgorithm.dll`을 불러옵니다. 템플릿은 앱에서 해석하지 않고, 등록·추출 함수가 채운 버퍼를 비교 함수에 전달합니다. 두 팀에 영향을 주는 함수 시그니처나 입력 규격 변경은 `include/`에서 합의합니다.

## FVC 데이터

`data/fvc/`에는 비교 앱에서 바로 열 수 있는 TIFF 240장이 있습니다. 원본 파일명은 그대로이며, 원본 ZIP은 `reference/프로젝트 관련 자료/`에 있습니다.

| 폴더 | `iType` | 영상 크기 | 파일 수 |
| --- | --- | --- | --- |
| [FVC2000_DB3_B](data/fvc/FVC2000_DB3_B/) | 1 | 448 × 478 | 80 |
| [FVC2002_DB3_B](data/fvc/FVC2002_DB3_B/) | 2 | 300 × 300 | 80 |
| [FVC2004_DB1_B](data/fvc/FVC2004_DB1_B/) | 3 | 640 × 480 | 80 |

파일명 `101_2.tif`에서 `101`은 손가락 번호, `2`는 해당 손가락의 촬영 번호입니다. 손가락 번호는 같은 DB 안에서 비교합니다.

## 실행 방법

### Visual Studio 2022

1. Visual Studio에서 **파일 → 열기 → 폴더**로 이 저장소 폴더를 엽니다.
2. CMake 구성에서 **Visual Studio 2022 x64** (`vs2022-x64`)를 선택합니다.
3. 시작 대상으로 **fingerprint_compare.exe**를 선택하고 `F5`를 누릅니다.
4. 등록용·비교용 TIFF를 한 장씩 선택하면 두 미리보기가 표시됩니다. **비교**를 누르면 0~1 점수가 표시됩니다.

이미지를 불러올 때 TIFF 형식과 지원 크기를 검사합니다. 지원하지 않는 파일을 선택하면 오류를 알리고 기존 선택을 유지합니다. 점수는 기존 밝기 히스토그램 알고리즘의 초안 결과이며 동일인 여부를 판정하지 않습니다.

Visual Studio의 **C++를 사용한 데스크톱 개발** 구성 요소가 필요합니다. 솔루션으로 작업하려면 CMake가 있는 터미널에서 다음을 실행합니다.

```powershell
cmake --preset vs2022-x64
cmake --build --preset vs2022-release
ctest --preset vs2022-release
```

생성된 `build/vs2022/IndustryAlgorithm.sln`을 Visual Studio에서 열고 **Release / x64**를 선택합니다. UI 코드는 `app/comparison_window.cpp`에 있으며, 빌드한 실행 파일은 `build/vs2022/Release/fingerprint_compare.exe`입니다.

### 실제 지문 파일 비교

1. VS Code에서 저장소 루트를 엽니다. 기본 빌드 설정은 MSYS2 UCRT64의 `C:\msys64\ucrt64\bin\g++.exe`를 사용합니다.
2. `Ctrl+Shift+B`를 눌러 **Build fingerprint comparison app**을 실행합니다.
3. 저장소 루트의 터미널에서 아래 명령을 실행합니다.

   ```powershell
   .\build\Release\fingerprint_compare.exe
   ```

4. 창에서 **등록용 지문**과 **비교용 지문**의 **파일 선택** 버튼을 눌러 `data/fvc/`의 DB 폴더에서 TIFF를 한 장씩 고릅니다. **비교**를 누르면 초안 비교 점수가 표시됩니다.

비교 앱과 `IndustryAlgorithm.dll`은 같은 폴더에 둡니다. 예를 들어 [FVC2002_DB3_B](data/fvc/FVC2002_DB3_B/)에서 다음과 같이 파일을 골라 비교할 수 있습니다.

| 비교 예시 | 등록용 | 비교용 |
| --- | --- | --- |
| 같은 손가락의 다른 촬영본 | `101_1.tif` | `101_2.tif` |
| 다른 손가락 | `101_1.tif` | `102_1.tif` |

### DLL 자동 테스트

빌드 후 저장소 루트의 터미널에서 아래 명령을 실행합니다.

```powershell
.\build\Release\test_pipeline.exe
```

`Fingerprint pipeline passed`가 출력되면 등록·특징 추출·비교와 오류 처리 테스트를 통과한 것입니다. 테스트가 입력 영상을 직접 생성하므로 FVC 압축을 풀 필요가 없습니다. 디버깅할 때는 **Fingerprint pipeline (GDB)** 구성을 선택하고 `F5`를 누릅니다.

시험 도구에서 불러올 파일은 `build/Release/IndustryAlgorithm.dll`입니다.

### CMake 빌드

Windows C++ 컴파일러와 CMake가 필요합니다. 저장소 루트에서 아래 명령을 실행합니다.

```powershell
cmake -S . -B build/cmake
cmake --build build/cmake --config Release
ctest --test-dir build/cmake -C Release --output-on-failure
cmake --install build/cmake --config Release --prefix dist
```

설치된 비교 앱과 DLL은 `dist/`에 생성됩니다.

# Fingerprint Recognition

## 프로젝트 목표

고급 객체지향 프로그래밍(SME3007) 과제로, 등록 지문과 비교 지문의 정합 점수를 반환하는 Windows용 C++17 DLL을 개발합니다. DLL은 KISA 지문인식 성능시험 인터페이스에 맞춰 다음 세 함수를 제공합니다.

| 함수 | 역할 |
| --- | --- |
| `KISA_Enroll` | 등록 영상 한 장에서 특징량을 추출해 등록 템플릿을 생성합니다. |
| `KISA_Attempt` | 비교 영상 한 장에서 특징량을 추출해 비교 템플릿을 생성합니다. |
| `KISA_Compare` | 두 템플릿을 비교해 0~1 범위의 정합 점수를 반환합니다. |

과제의 성능평가 목표는 FVC 2000·2002·2004 데이터로 같은 손가락과 다른 손가락의 영상 쌍을 비교하고, FMR·FNMR·EER, 등록·특징 추출 실패율, 처리 시간, 템플릿 크기를 측정하는 것입니다. 기본 빌드는 비교 기준인 16바이트 히스토그램을 사용합니다. 별도 DeepPrint 빌드는 사전학습 `flx` 모델로 512차원 특징을 추출하며, 노이즈 평가기는 DLL 경로로 어느 빌드를 평가할지 선택합니다.

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
├── tools/
│   ├── deepprint/export_model.py    # 오프라인 ONNX 변환 및 기준 벡터 생성
│   ├── fvc/                         # 실험 도구의 TIFF 입출력과 데이터 목록
│   ├── noise/                       # C++ 가우시안 노이즈 생성기
│   └── evaluation/                  # C++ 일괄 비교 및 EER 집계
├── data/fvc/                       # 비교 앱에서 바로 선택할 FVC TIFF
│   ├── FVC2000_DB3_B/               # iType 1
│   ├── FVC2002_DB2_B/               # iType 2
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
| `tools/` | 모델 변환, 증강 데이터 생성, 명령줄 일괄 평가 |

평가 앱은 `include/`의 공용 헤더로 컴파일하고, 빌드된 DLL의 가져오기 라이브러리에 링크합니다. 실행 시 `IndustryAlgorithm.dll`을 불러옵니다. 템플릿은 앱에서 해석하지 않고, 등록·추출 함수가 채운 버퍼를 비교 함수에 전달합니다. 두 팀에 영향을 주는 함수 시그니처나 입력 규격 변경은 `include/`에서 합의합니다.

## FVC 데이터

`data/fvc/`에는 비교 앱에서 바로 열 수 있는 TIFF 240장이 있습니다. 원본 파일명은 그대로이며, 원본 ZIP은 `reference/프로젝트 관련 자료/`에 있습니다.

| 폴더 | `iType` | 영상 크기 | 파일 수 |
| --- | --- | --- | --- |
| [FVC2000_DB3_B](data/fvc/FVC2000_DB3_B/) | 1 | 448 × 478 | 80 |
| [FVC2002_DB2_B](data/fvc/FVC2002_DB2_B/) | 2 | 300 × 300 | 80 |
| [FVC2004_DB1_B](data/fvc/FVC2004_DB1_B/) | 3 | 640 × 480 | 80 |

파일명 `101_2.tif`에서 `101`은 손가락 번호, `2`는 해당 손가락의 촬영 번호입니다. 손가락 번호는 같은 DB 안에서 비교합니다.

저장소의 `FVC2002_DB2_B/` 폴더에 있는 80장은 동봉된 `FVC2002_DB3_B.zip`의 TIFF와 바이트 단위로 일치합니다. 일괄 평가 CSV에는 저장소의 실제 폴더명이 기록됩니다.

## 실행 방법

### 실제 지문 파일 비교

1. VS Code에서 저장소 루트를 엽니다. 기본 빌드 설정은 MSYS2 UCRT64의 `C:\msys64\ucrt64\bin\g++.exe`를 사용합니다.
2. `Ctrl+Shift+B`를 눌러 **Build fingerprint comparison app**을 실행합니다.
3. 저장소 루트의 터미널에서 아래 명령을 실행합니다.

   ```powershell
   .\build\Release\fingerprint_compare.exe
   ```

4. 창에서 **등록용 지문**과 **비교용 지문**의 **파일 선택** 버튼을 눌러 `data/fvc/`의 DB 폴더에서 TIFF를 한 장씩 고릅니다. **비교**를 누르면 초안 비교 점수가 표시됩니다.

비교 앱과 `IndustryAlgorithm.dll`은 같은 폴더에 둡니다. 예를 들어 [FVC2002_DB2_B](data/fvc/FVC2002_DB2_B/)에서 다음과 같이 파일을 골라 비교할 수 있습니다.

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

## DeepPrint DLL과 노이즈 평가

DeepPrint DLL은 FVC 회색조 픽셀을 흰색 패딩과 안티앨리어싱 리사이즈로 `299×299`에 맞춘 뒤, ONNX 모델에서 `flx`의 FVC 이진화와 추론을 수행합니다. 질감 256개와 minutiae 256개를 합친 벡터는 형식 헤더를 포함해 2,056바이트입니다. 비교 시 코사인 유사도를 `0~1`로 변환하고 소수점 셋째 자리까지 반환합니다. KISA 공개 함수 세 개의 시그니처는 기존과 동일합니다.

### 최초 빌드 준비

아래는 Windows x64, CMake, MSYS2 UCRT64의 `g++`와 `mingw32-make`, 일반 CPython 3.12가 설치된 환경에서 저장소 루트의 PowerShell로 실행하는 예시입니다. Python은 모델 변환과 기준 벡터 생성에 사용합니다.

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

```powershell
$repo = (Get-Location).Path
cmake -S . -B build/deepprint -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release `
  -DFINGERPRINT_BUILD_GUI=OFF -DFINGERPRINT_DEEPPRINT=ON `
  "-DFINGERPRINT_ONNXRUNTIME_ROOT=$repo/build/dependencies/onnxruntime" `
  "-DFINGERPRINT_DEEPPRINT_MODEL=$repo/build/models/deepprint_texminu_512.onnx" `
  "-DFINGERPRINT_DEEPPRINT_REFERENCE_DIR=$repo/build/models/parity"
cmake --build build/deepprint --parallel
ctest --test-dir build/deepprint --output-on-failure
```

`build/deepprint/`의 `IndustryAlgorithm.dll`, `deepprint_texminu_512.onnx`, `onnxruntime.dll`을 함께 둡니다. DLL은 자신의 폴더에서 모델과 런타임을 찾고, 첫 추출부터 프로세스가 끝날 때까지 추론 세션을 재사용합니다.

최초 CMake 구성이 끝나면 VS Code의 **Tasks: Run Task → Build DeepPrint DLL and noise tools**로 이 빌드를 다시 실행할 수 있습니다.

### 노이즈 TIFF 생성 및 일괄 평가

각 실행의 출력 폴더는 새 경로를 지정합니다. 아래 예시는 기준 시드 `42`와 8비트 픽셀 기준 가우시안 노이즈 `σ=2,5,10`을 사용합니다.

```powershell
.\build\deepprint\augment_fvc.exe data/fvc build/augmentation/fvc-seed42 42 2 5 10
.\build\deepprint\evaluate_noise.exe data/fvc build/augmentation/fvc-seed42 build/deepprint/IndustryAlgorithm.dll build/evaluation/deepprint-seed42
```

`build/augmentation/fvc-seed42/manifest.csv`가 이미 있으면 같은 증강본을 재사용할 수 있습니다. 평가만 반복할 때는 두 번째 명령의 마지막 인자에 새 결과 폴더를 지정합니다.

- 증강기는 원본 크기에서 픽셀 노이즈를 더하고 `0~255`로 제한합니다. DB명, 파일명, 강도로 영상별 시드를 만들고, 손실 없는 회색조 TIFF 720장과 `manifest.csv`를 저장합니다. 동일한 실행 파일과 시드에서는 동일한 픽셀이 생성됩니다. `σ=0` 조건은 목록에서 원본 TIFF를 가리킵니다.
- 평가기는 원본 등록 템플릿을 재사용하고, DB별로 정렬한 영상의 모든 서로 다른 쌍을 각 강도에서 비교합니다. 등록 영상은 원본이고 비교 영상에만 노이즈가 들어갑니다. 결과는 `pairs.csv`와 `summary.csv`에 기록됩니다. `eer`, `fte`, `fta`는 0~1 비율이고, EER은 점수를 얻은 쌍으로 계산합니다. 실패 건수와 평균 처리 시간은 별도 열에서 확인할 수 있습니다.
- 히스토그램과 비교할 때는 `FINGERPRINT_DEEPPRINT=OFF`로 별도 빌드한 DLL을 같은 평가기에 전달합니다. 현재 FVC는 평가에 사용하며, 학습용 데이터가 확보되면 손가락 ID로 먼저 분할한 뒤 학습 영상에만 증강을 적용합니다.

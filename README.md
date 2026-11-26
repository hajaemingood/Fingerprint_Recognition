# Fingerprint Recognition

고급 객체지향 프로그래밍(SME3007) 팀 프로젝트입니다. Windows용 C++17 DLL은 지문 템플릿을 만들고 정합 점수를 반환합니다. 평가 GUI는 FVC 영상으로 DLL을 실행해 인식 성능과 처리 성능을 측정하고 보고서를 생성합니다.

체크포인트 변환, 전처리·미세조정, KISA DLL, 평가 GUI와 보고서 생성은 **C++17**로 구현했습니다. 발표용 DLL은 사전학습 DeepPrint를 ONNX Runtime CPU로 실행하고, 연구용 학습기는 네이티브 LibTorch CPU로 가중치를 갱신합니다. 함수 호출도, ONNX 레이어 형상, 정합식과 학습 손실은 [알고리즘 기술 문서](src/README.md)에 있습니다.

## 최종 시연 구성

현재 `progess`의 발표용 패키지는 `build/cpp-validation/presentation/`에 있습니다. 이 PC에서 저장소 루트의 PowerShell로 실행한 뒤 **01 실험 설정 → 알고리즘 DLL 선택…**에서 같은 폴더의 `IndustryAlgorithm.dll`을 지정합니다.

```powershell
.\build\cpp-validation\presentation\fingerprint_compare.exe
```

코드를 수정한 뒤에는 아래 명령으로 DLL과 GUI를 다시 빌드해 발표 폴더에 반영합니다. 이 명령은 이 PC에 구성된 `build/cpp-validation`의 CMake 캐시를 사용합니다. 설치할 파일을 사용 중이면 발표용 GUI를 먼저 닫습니다. 새 클론에서는 [DeepPrint 최초 빌드 준비](#deepprint-최초-빌드-준비)부터 진행합니다.

```powershell
$cmake = '.\build\dependencies\cmake\bin\cmake.exe'
& $cmake --build build/cpp-validation --target IndustryAlgorithm fingerprint_compare --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'DLL 또는 GUI 빌드 실패' }
& $cmake --install build/cpp-validation --prefix build/cpp-validation/presentation
if ($LASTEXITCODE -ne 0) { throw '발표 폴더 복사 실패' }
.\build\cpp-validation\presentation\fingerprint_compare.exe
```

다른 Windows x64 PC에서 실행할 때는 아래 네 파일을 같은 폴더에 두고, 평가할 DB 폴더도 준비합니다.

| 파일 | 역할 |
| --- | --- |
| `fingerprint_compare.exe` | 실험 설정, DLL 실행, 지표 계산과 보고서 저장 |
| `IndustryAlgorithm.dll` | KISA C ABI를 구현한 DeepPrint 지문 비교 DLL |
| `deepprint_texminu_512.onnx` | 질감 256차원과 특징점 256차원 임베딩을 추출하는 모델 |
| `onnxruntime.dll` | Windows x64 CPU 추론 런타임 |

### 모델에서 평가까지

1. C++ 변환기로 사전학습 DeepPrint 체크포인트를 ONNX로 내보내고, 독립 기준 벡터와 KISA 점수로 추론 결과를 검증합니다.
2. DLL은 FVC2000의 전체 영상, FVC2002의 `3×3` 양방향 필터 적용 영상(밝기 차이 σ=12), FVC2004의 전경 ROI(각 방향 여백 20%)를 흰색으로 패딩해 299×299로 맞춥니다. 공통 CNN에서 질감·특징점 분기로 나뉘고, 각각의 256차원 벡터가 512차원 템플릿이 됩니다. [전처리와 모델 구조](src/README.md#4-deepprint-model-architecture)
3. GUI가 `KISA_Enroll`과 `KISA_Attempt`로 두 템플릿을 얻으면 `KISA_Compare`가 분기별 코사인 점수를 각각 0~1로 변환하고 질감·특징점 비율 **22:78**로 합산합니다. GUI는 DLL이 소수점 셋째 자리로 반환한 점수에 판정 기준을 적용하고 오류율, 시간, 실패를 기록합니다. [정합 점수 계산](src/README.md#7-fingerprint-matching)

### 최종 DLL 검증 결과

FVC 원본 240장으로 DB마다 **본인 280쌍과 타인 2,880쌍**을 비교했습니다. 전체 **9,480쌍에서 처리 실패는 0건**이었고, C++로 내보낸 ONNX와 기준 DLL의 반올림 점수가 모든 쌍에서 일치했습니다. 표의 FMR·FNMR은 각 DB의 **EER 추정 임계값**에서 측정했습니다. [이전 전처리 결과](log/fvc2004-roi-deployment-20261125/original_metrics.csv) · [최종 KISA 대조 CSV](log/cpp-port-verification/native-parity.csv)

| DB | 전처리 변경 전 EER | 시연 DLL EER | EER 임계값 | 해당 지점 FMR / FNMR | EER ≤ 3% |
| --- | ---: | ---: | ---: | ---: | --- |
| FVC2000 | 2.64% | **2.64%** | 0.868 | 2.78% / 2.50% | 달성 |
| FVC2002 | 8.03% | **6.48%** | 0.788 | 6.53% / 6.43% | 미달 |
| FVC2004 | 5.69% | **4.78%** | 0.846 | 5.28% / 4.29% | 미달 |

GUI 기본 규칙은 DB당 **160쌍(본인 70·타인 90)**입니다. 위 표는 `deepprint_research verify`에 DB당 3,160쌍의 고정 목록을 전달해 재현할 수 있습니다. GUI의 **불러오기**는 GUI에서 저장한 `enroll,attempt,relation` 형식의 목록을 읽습니다. [CLI용 고정 비교 쌍](log/fvc2004-roi-deployment-20261125/app-run/logs/run-1795556427179-31364-1/pairs.csv) · [C++ 평가 명령](#c-연구-도구)

### 미세조정 후보와 시연 모델의 차이

C++에서 A(두 투영층), B(질감 마지막 Inception C와 투영층), C(특징점 마지막 Conv와 투영층)를 각각 12스텝 학습했습니다. DB마다 손가락 ID를 **학습 4개·검증 3개·최종 확인 3개**로 나눴습니다. 검증에서 선택된 조합은 **A의 4스텝 체크포인트, 질감·특징점 0:100**입니다. 아래 수치는 모두 같은 세 DB에서 측정한 EER입니다. [분할 목록](log/fvc-branch-tuning-20261126/split.csv) · [후보 선택](log/cpp-port-verification/branch-selection.csv) · [후보 KISA 결과](log/cpp-port-verification/branch-kisa-parity.csv)

| 모델과 평가 범위 | DB당 비교 수 | FVC2000 | FVC2002 | FVC2004 |
| --- | ---: | ---: | ---: | ---: |
| 시연 모델 22:78 · 전체 원본 | 3,160쌍 | 2.64% | 6.48% | 4.78% |
| 후보 A 0:100 · 전체 원본 | 3,160쌍 | 3.66% | 6.30% | 5.65% |
| 시연 모델 22:78 · 보류 ID만 | 276쌍 | 1.38% | 11.68% | 4.72% |
| 후보 A 0:100 · 보류 ID만 | 276쌍 | 2.23% | 11.94% | 1.64% |

전체 원본에서 후보 A는 FVC2002의 EER을 낮췄지만 FVC2000·FVC2004에서는 오류가 늘었습니다. 보류 ID에서도 세 DB에 일관된 개선이 없어 시연에는 **사전학습 모델과 22:78**을 채택했습니다. 별도의 증강 실험은 원본과 변형을 섞어 학습하고 원본으로 최종 확인했습니다. [학습과 비율 변경을 분리한 4조건 비교](src/README.md#12-experimental-results-and-limitations) · [증강 실험 기록](log/fvc-augmentation-tuning-20261126/conclusion.txt)

## 새 PC에서 처음부터 실행 (Visual Studio 2022·2026 공통)

`build/`의 DLL·모델·런타임은 Git에 포함되지 않으므로, 클론 직후에는 아래 절차로 한 번 준비합니다.
`ui-*`, `deepprint-release` 프리셋은 Visual Studio 버전 이름에 묶이지 않는 Ninja + MSVC 구성입니다.
기존 `vs2022-*` 프리셋은 VS 2022 전용으로 그대로 유지합니다.

**필요한 것:** Visual Studio 2022 이상의 **C++를 사용한 데스크톱 개발** 워크로드(Windows용 C++ CMake 도구 포함), CMake 3.20 이상, Ninja

1. **모델과 ONNX Runtime 준비** — x64 Developer PowerShell에서 저장소 루트로 이동해 [DeepPrint 최초 빌드 준비](#deepprint-최초-빌드-준비)의 명령을 그대로 실행합니다.
   완료되면 `build/models/deepprint_texminu_512.onnx`, `build/dependencies/onnxruntime/`가 생깁니다.
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
모델 기준 벡터는 `tests/fixtures/deepprint/`에 저장하고 CTest로 실제 C++ 추론과 대조합니다.

## 평가 앱 사용

| 단계 | 조작과 결과 | 평가에서의 의미 |
| --- | --- | --- |
| **01 실험 설정** | DB와 x64 DLL을 지정합니다. 등록·본인·타인 규칙과 동시 비교 수(1~16)를 설정하고 **새 비교 목록 만들기**를 누릅니다. 저장한 목록은 **불러오기**로 재사용할 수 있습니다. | 비교 대상과 정답을 먼저 확정합니다. 같은 손가락의 다른 촬영본은 본인, 다른 손가락의 촬영본은 타인 비교입니다. 동일 이미지의 자기 비교는 제외합니다. |
| **02 비교 실행 · 결과** | **비교 실행**을 누르면 앱이 `Enroll → Attempt → Compare` 순서로 KISA 함수를 호출합니다. 비교별로 실제 관계, 점수, 판정·처리 상태와 시간을 확인합니다. **전체 저장**은 비교 쌍과 실행 순서를 CSV로 저장합니다. | 어떤 파일에서 오인식·미인식 또는 처리 실패가 나왔는지 추적할 수 있는 쌍별 기록입니다. 실행이 시작되면 03 화면으로 이동합니다. |
| **03 인식 성능** | 본인·타인 점수 분포, 임계값별 오류율, DET 곡선을 확인합니다. **개별 비교 판정 기준** 슬라이더로 기준을 바꾸면 현재 FMR·FNMR이 갱신됩니다. | 본인과 타인의 점수를 얼마나 잘 구분하는지 봅니다. EER과 그 추정 임계값은 앱이 전체 점수에서 자동 계산합니다. |
| **04 처리 성능** | 전체 경과 시간, 성공 처리량, 평균·P95 비교 지연, 이미지 읽기·등록 준비와 비교에 걸린 시간, 병렬 작업 현황을 확인합니다. | 인식률과 별도로 실행 비용을 측정합니다. P95는 성공 비교의 약 95%가 그 시간 이내에 끝났음을 뜻합니다. 전체 경과 시간에는 준비 단계도 포함됩니다. |
| **05 평가 요약** | **핵심 결과 / 상세 지표 / 그래프**에서 오류율, 실패 건수, API 호출 시간, 템플릿 크기와 메모리를 확인하고 **보고서 저장**을 누릅니다. | 동일한 실행의 정확도와 처리 성능을 비교 가능한 문서로 남깁니다. 임계값 그래프에서 EER·FMR100·FMR1000·ZeroFMR·ZeroFNMR을 실제 측정 지점에 연결해 표시합니다. |

### 지표 읽기

- **FMR**은 타인 비교를 본인으로 받아들인 비율, **FNMR**은 본인 비교를 거부한 비율이며 GUI의 기본 집계에는 본인 비교의 처리 실패도 포함합니다. 앱 시작 시 개별 판정 임계값은 **0.850**입니다. 본인·타인 쌍 모두 같은 임계값을 적용하며 점수가 정확히 0.850이면 수락합니다.
- **EER**은 FMR과 FNMR이 같아지는 지점의 오류율입니다. GUI는 완료된 쌍의 관측 점수들을 임계값으로 살펴봅니다. 정확히 같아지는 지점이 없으면 교차 전후 두 점 중 평균 오류율이 낮은 쪽을 선택합니다. 슬라이더를 움직이면 개별 판정과 현재 FMR·FNMR이 바뀌고, 동일한 비교 목록과 점수의 EER은 유지됩니다. **EER\***은 성공한 비교만 집계합니다. [GUI 지표 계산](app/fingerprint_image.cpp)
- **FMR100 / FMR1000**은 FMR이 각각 `1% / 0.1%` 이하일 때 가장 낮은 FNMR입니다. **ZeroFMR**은 FMR이 0일 때 가장 낮은 FNMR, **ZeroFNMR**은 FNMR이 0일 때 가장 낮은 FMR입니다.
- **FTE**는 등록 단계의 실패율입니다. **REJgenuine / REJimpostor**는 실제 본인·타인 매칭 시도에서의 실패율입니다. 상세 지표의 `KISA_Compare` 시간은 특징 추출 시간을 제외하며, RAM은 앱·모델·캐시 전체의 Working Set 관측값입니다.

### 보고서와 실행 환경

보고서에 넣을 항목을 선택하고 EER 집계 방식을 **처리 실패 포함 / 성공한 비교만 / 두 방식 모두** 중에서 고릅니다. 문서 형식은 UTF-8 HTML 또는 Microsoft Word에서 열 수 있는 RTF입니다. 그래프를 포함하면 문서 옆의 `<문서 파일명>-graphs/`에도 SVG 세 장을 저장합니다. 브라우저의 인쇄 명령(Ctrl+P)으로 전체 문서를 PDF로 저장할 수 있습니다.

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

앱은 매 비교마다 테스트 이미지의 특징을 추출합니다. CSV의 원래 DB 경로를 찾을 수 없으면 같은 이름의 DB 폴더를 먼저 선택한 뒤 불러옵니다. 평가할 DLL은 UI에서 따로 선택합니다.

진행 중이거나 중단된 평가에는 완료 쌍 기준의 잠정 지표를 표시합니다. 보고서는 처리 실패와 미완료 행을 보존합니다. 현재 앱은 DLL을 GUI 프로세스에서 실행하므로 **비교 중지**는 진행 중인 비교가 반환된 뒤 적용됩니다. 과제가 요구하는 DLL 타임아웃·크래시 감지와 알고리즘 강제 종료는 보완할 항목입니다.

---


## 프로젝트 목표

| 사용 목적 | 빌드 또는 실행 경로 |
| --- | --- |
| Windows 성능 평가 | `fingerprint_compare.exe`에서 DLL, FVC DB, 비교 규칙을 선택하고 HTML 또는 RTF 보고서 저장 |
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
│   ├── README.md                     # 알고리즘, 호출 관계, 학습 및 검증 결과
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
│   ├── deepprint/                   # C++ ONNX 변환기, 전처리·미세조정·검증 도구
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
│   ├── fixtures/deepprint/           # 전처리 및 추론 결과를 대조하는 독립 기준 벡터
│   ├── test_research.cpp             # 연구 지표와 ONNX 직렬화 테스트
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
│   ├── fingerprint_evaluation.html   # 이전 실행의 HTML 보고서
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
| `app/` | Windows 평가 앱. 시험 조건 입력, DLL 호출, 비교 목록·보고서 출력 |
| `src/` | 알고리즘 DLL 팀. 특징 추출, 템플릿 저장·복원, 매칭, KISA 함수 구현 |
| `include/` | 공동 인터페이스. KISA 함수 선언, 버퍼 규약, `iType`별 영상 크기 |
| `data/fvc/`, `tests/` | 공유 평가 데이터와 DLL 연동 테스트 |
| `tools/` | DeepPrint 모델 변환, TIFF 노이즈 생성, KISA DLL 일괄 평가 |
| `reports/` | 이전 평가 HTML과 실험 SVG 그래프 |

평가 앱은 선택한 x64 DLL을 임시 폴더에 복사한 뒤 앱 프로세스에서 동적으로 불러옵니다. 명령줄 평가기는 실행 인자로 받은 DLL을 사용합니다. 두 평가기는 공개 KISA 함수로 템플릿을 만들고 비교합니다. 평가 팀은 `FINGERPRINT_DEEPPRINT=OFF`(`ui-release` 프리셋)로 앱을 독립적으로 빌드할 수 있습니다.

## 구형 빌드와 평가 기록

발표용 실행 파일과 DLL은 `build/cpp-validation/presentation/`에 함께 있습니다. `build/evaluator/`는 VS Code 기본 빌드 및 기존 기준 모델을 사용한 비교 실험 경로입니다. 로컬의 이전 빌드는 `build/_archive/`에, 교체 전 DLL은 `build/rollback/`에 보관합니다. `build/`는 Git에서 제외하며 최종 수치와 비교 조건은 저장소에서 확인할 수 있습니다. [최종 검증 CSV](log/cpp-port-verification/) · [전체 실험 기록](log/)

구형 히스토그램 DLL과 평가 기록의 위치·해시는 보관 로그에 기록되어 있습니다. [보관 로그](archive/archive-log.csv)

## FVC 데이터

`data/fvc/`에는 DB별 손가락 10개를 각각 8번 촬영한 원본 TIFF가 총 **240장** 있습니다. 기본 강건성 실험은 이 240장에 노이즈·회전·밝기 변형을 각각 적용해 **증강본 2,880장**을 생성합니다. 위 최종 DLL 표는 원본 영상끼리 비교한 결과입니다. [강건성 평가 조건](#회전밝기를-포함한-강건성-평가)

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
    '.\build\dependencies\cmake\bin\cmake.exe'
}
```

같은 터미널에서 DLL과 평가 앱을 빌드합니다.

```powershell
& $cmake -S . -B build/evaluator -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe `
  -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/mingw32-make.exe `
  -DFINGERPRINT_BUILD_GUI=ON -DFINGERPRINT_DEEPPRINT=ON -DFINGERPRINT_FVC2004_ROI=ON `
  '-DFINGERPRINT_TEXTURE_WEIGHT:STRING=0.22' `
  -DFINGERPRINT_DEEPPRINT_REFERENCE_DIR=tests/fixtures/deepprint
& $cmake --build build/evaluator --parallel 4
```

이 VS Code·MinGW 구성의 산출물은 `build/evaluator/`에 생성됩니다. 발표용 폴더를 갱신할 때는 [최종 시연 구성](#최종-시연-구성)의 `build/cpp-validation` 빌드·설치 명령을 사용합니다.

| 산출물 | 역할 |
| --- | --- |
| `fingerprint_compare.exe` | 다섯 페이지 평가 앱 |
| `IndustryAlgorithm.dll` | DeepPrint의 KISA 함수 구현과 FVC2002·FVC2004 전처리 |
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

### 자동 테스트

위의 PowerShell 빌드 명령을 실행한 터미널에서 CTest를 실행합니다.

```powershell
& (Join-Path (Split-Path -Parent $cmake) 'ctest.exe') --test-dir build/evaluator --output-on-failure
```

Visual Studio 프리셋으로 빌드했다면 `ctest --preset deepprint-release`를 실행합니다.

`noise_and_metrics`는 증강과 평가 지표를 검사하고, `deepprint_reference`는 독립 기준 벡터와 C++ 추론 결과를 대조합니다. 연구 도구를 켜면 `research_metrics_and_onnx`도 등록됩니다. 현재 발표용 빌드 구성에서 세 검사를 실행하려면 아래 명령을 사용합니다.

```powershell
.\build\dependencies\cmake\bin\ctest.exe --test-dir build/cpp-validation --output-on-failure
```

## DeepPrint DLL과 노이즈 평가

DeepPrint DLL은 FVC 회색조 픽셀을 흰색 패딩과 안티앨리어싱 리사이즈로 `299×299`에 맞춘 뒤, ONNX 모델에서 `flx`의 FVC 이진화와 추론을 수행합니다. 질감 256개와 minutiae 256개를 합친 벡터는 형식 헤더를 포함해 2,056바이트입니다. 비교 시 두 분기의 코사인 점수를 각각 `0~1`로 변환한 뒤 질감 0.22와 특징점 0.78의 가중합을 소수점 셋째 자리까지 반환합니다. [정합 가중치 정의](src/matching/fingerprint_matcher.h)

### DeepPrint 최초 빌드 준비

저장소 루트의 x64 Developer PowerShell에서 실행합니다. 체크포인트를 읽어 ONNX를 생성하는 프로그램은 C++17 실행 파일 `export_deepprint`입니다. 아래 명령은 새 클론 기준입니다.

```powershell
New-Item -ItemType Directory -Force build/dependencies | Out-Null
curl.exe -fL 'https://drive.usercontent.google.com/download?id=1R9v73hFBuy0iihz7v8eDlBcGrMDmQ9TH&export=download&confirm=t' -o build/dependencies/best_model.pyt
curl.exe -fL 'https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/1.20.1/microsoft.ml.onnxruntime.1.20.1.nupkg' -o build/dependencies/onnxruntime.zip
Expand-Archive build/dependencies/onnxruntime.zip -DestinationPath build/dependencies/onnxruntime
cmake -S . -B build/cpp-research -G Ninja -DCMAKE_BUILD_TYPE=Release -DFINGERPRINT_BUILD_RESEARCH=ON -DFINGERPRINT_BUILD_GUI=OFF
cmake --build build/cpp-research --target export_deepprint deepprint_research
.\build\cpp-research\export_deepprint.exe build/dependencies/best_model.pyt tools/deepprint/architecture build/models/deepprint_texminu_512.onnx
```

기존 모델을 보관한 PC에서는 변환기의 마지막 인수에 새 파일명을 지정하세요. DLL을 구성할 때 `FINGERPRINT_DEEPPRINT_MODEL`로 사용할 파일을 지정할 수 있습니다. MSYS2에서는 위의 Ninja 대신 DLL 빌드 예시의 MinGW 생성기·컴파일러를 사용합니다.

새 클론에서 위 모델로 시연용 네 파일을 묶으려면 같은 터미널에서 실행합니다.

```powershell
cmake -S . -B build/cpp-validation -G Ninja -DCMAKE_BUILD_TYPE=Release -DFINGERPRINT_DEEPPRINT=ON -DFINGERPRINT_BUILD_GUI=ON -DFINGERPRINT_BUILD_RESEARCH=ON -DFINGERPRINT_FVC2004_ROI=ON '-DFINGERPRINT_TEXTURE_WEIGHT:STRING=0.22' -DFINGERPRINT_DEEPPRINT_REFERENCE_DIR=tests/fixtures/deepprint
cmake --build build/cpp-validation
ctest --test-dir build/cpp-validation --output-on-failure
cmake --install build/cpp-validation --prefix build/cpp-validation/presentation
```

변환기는 공개 체크포인트의 SHA-256을 검사하고, 저장소에 포함된 고정 추론 그래프에 학습 텐서 394개를 채웁니다. 합성곱과 추론용 BatchNorm을 결합하고 질감·특징점 출력을 연결합니다. 그래프 파일에는 연산 구조와 고정 필터가 들어 있습니다. CTest는 FVC 원본을 직접 읽어 기존 독립 기준 벡터와 대조합니다. 소수점 연산 순서에 따라 ONNX 파일 해시가 달라질 수 있습니다. [변환기](tools/deepprint/export_model.cpp) · [그래프와 파라미터 배치](tools/deepprint/architecture/) · [기준 벡터](tests/fixtures/deepprint/) · [flx 소스 및 라이선스](https://github.com/tim-rohwedder/fixed-length-fingerprint-extractors/tree/7accfca1f33b9b42bfd220e43cd5bc13b4a7fa13) · [공개 체크포인트](https://drive.google.com/file/d/1R9v73hFBuy0iihz7v8eDlBcGrMDmQ9TH/view)

### C++ 연구 도구

모델 변환·임베딩 캐시·전처리·ROI 비교는 위 연구 빌드로 실행합니다. 미세조정에는 LibTorch CPU 2.5.1과 TorchSharp 0.105.0의 네이티브 C 호출 인터페이스를 사용합니다. 아래 명령은 학습용 DLL을 준비한 뒤 `deepprint_train.exe` 옆에 복사합니다. 평가 GUI의 배포 구성은 네 파일입니다.

```powershell
cmake -P tools/deepprint/prepare_training.cmake
$native = (Resolve-Path build/dependencies/torch-native/runtimes/win-x64/native).Path
cmake -S . -B build/cpp-research "-DFINGERPRINT_TORCH_NATIVE_DIR=$native"
cmake --build build/cpp-research --target deepprint_train deepprint_research
$research = '.\build\cpp-research\deepprint_research.exe'
$train = '.\build\cpp-research\deepprint_train.exe'
$model = 'build/cpp-validation/presentation/deepprint_texminu_512.onnx'
$dll = 'build/cpp-validation/presentation/IndustryAlgorithm.dll'
$pairs = 'log/fvc2004-roi-deployment-20261125/app-run/logs/run-1795556427179-31364-1/pairs.csv'
$idSplit = 'log/finetune-noise40-20261124-175440/split.csv'
$captureSplit = 'log/finetune-last-id-sigma30-mean-20261125-053138/split.csv'
$heldFolds = 'log/unseen-adaptation-20261125-055540/development_split.csv'
$screenSplit = 'log/weight-tuning-20261124-164133/dll-verification/split.csv'
& $research verify data/fvc $pairs $dll $model build/cpp-results/native.csv
& $research screen $model data/fvc $pairs $screenSplit build/cpp-results/preprocessing
& $research align $model data/fvc $pairs $screenSplit build/cpp-results/alignment $dll
& $research roi build/evaluation/fvc2004-roi-validation/standard/pairs.csv build/evaluation/fvc2004-roi-validation/roi/pairs.csv $idSplit $captureSplit build/cpp-results/roi
```

아래 실험은 저장된 손가락 분할을 재사용합니다. `branches`는 A·B·C를 같은 기준 모델에서 시작하고, `augmentation`은 보류 ID를 제외한 세 내부 폴드에서 증강·학습률·투영층·정합 비율을 선택합니다. 선택을 기록한 뒤 보류 ID와 전체 목록의 EER을 각각 계산합니다. 결과 폴더는 실행마다 새 경로를 지정합니다. 원본 평가 결과는 위의 최종 DLL 검증 표를 참조하세요. C++ 학습기는 고정 시드의 표준 난수 생성기를 사용하므로 과거 학습과 표본 순서 및 후보 가중치가 달라질 수 있습니다.

```powershell
& $train branches $model data/fvc $pairs log/fvc-branch-tuning-20261126/split.csv build/cpp-results/branches
& $train augmentation $model data/fvc $pairs log/fvc-augmentation-tuning-20261126/folds.csv build/cpp-results/augmentation
```

기존 기준 모델(`build/evaluator/deepprint_texminu_512.onnx`)로 학습부터 실험용 DLL 빌드, KISA 대조까지 한 번에 실행하려면 저장소 루트에서 `cmake -DSTUDY=branches -P tools/deepprint/replay.cmake` 또는 `cmake -DSTUDY=augmentation -P tools/deepprint/replay.cmake`를 사용합니다. 이미 학습한 경로는 `-DVERIFY_ONLY=ON -DOUTPUT=<결과 폴더>`로 지정합니다.

각 결과 폴더의 `selection.csv`에서 질감 비중을 읽습니다. 실험용 DLL을 새 빌드 폴더에서 `-DFINGERPRINT_TEXTURE_WEIGHT=<선택한 비중>`, `-DFINGERPRINT_DEEPPRINT_MODEL=<후보 ONNX 절대 경로>`로 빌드한 뒤 `deepprint_research verify`의 마지막 인수에도 같은 비중을 지정합니다.


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

캐시 도구는 실제 증강 TIFF와 `manifest.csv`를 읽고 촬영본별 특징·파일 순서·처리 시간을 NPZ에 저장합니다. 동일 모델의 기존 `fvc-noise40-cache`도 읽습니다. 위의 C++ 연구 도구 절에서 정의한 PowerShell 변수를 사용합니다. 아래 입력 폴더는 강건성 증강과 두 시드의 추가 노이즈 영상이 준비된 경우의 예시입니다.

```powershell
$cache = 'build/cpp-results/cache'
& $research cache $model data/fvc $cache 40 pilots build/augmentation/fvc-robustness-verify build/augmentation/fvc-high-noise-seed42 build/augmentation/fvc-high-noise-seed43
& $train heads $model $cache $idSplit $captureSplit $heldFolds build/cpp-results/heads
& $train last-id $model $cache $idSplit $captureSplit $heldFolds build/cpp-results/last-id
& $train unseen $model $cache $idSplit $captureSplit $heldFolds build/cpp-results/unseen
```

`heads`는 개발 ID 5개로 교차검증한 뒤 남은 5개로 평가합니다. `last-id`는 동일 ID의 새 촬영본과 가장 큰 ID의 새 등록을 각각 평가합니다. `unseen`은 보류 손가락 폴드에서 업데이트 강도와 증강을 선택합니다. [C++ 학습 및 검증](tools/deepprint/legacy_study.cpp) · [과거 σ=30 실험](log/finetune-last-id-sigma30-mean-20261125-053138/evaluation.csv)


## FVC2002와 FVC2004 전처리

기본 설정으로 빌드한 `IndustryAlgorithm.dll`은 FVC2002(`iType=2`)에 3×3 양방향 필터를 적용합니다.
공간 σ는 1픽셀, 밝기 차이 σ는 12입니다. FVC2004(`iType=3`)는 지문 전경에 각 방향 20% 여백을 둔 영역을 사용합니다.
전경이 충분히 검출되지 않으면 전체 프레임을 사용합니다. 평가 앱의 **알고리즘 DLL 선택**에서 빌드한 DLL을 지정하고,
이전 빌드에서 저장한 등록·비교 템플릿은 업데이트한 DLL로 다시 생성합니다.

FVC2004에 전체 프레임을 사용하려면 별도 빌드 디렉터리에서 `-DFINGERPRINT_FVC2004_ROI=OFF`를 지정합니다.
이 빌드는 같은 사전학습 ONNX와 22:78 정합 가중치를 사용합니다. 독립 테스트 자료에는
`tests/fixtures/deepprint/2.bilateral12.*.f32`와 `3.roi20.*.f32` 기준 벡터가 포함되어 있습니다.
[전처리 후보 비교](log/fvc-preprocessing-screen-20261126/conclusion.txt)

## 평가 보고서

평가 앱의 **보고서 저장**은 실행한 DB, 비교 규칙, 완료 쌍 수와 지표를 함께 기록합니다. 예를 들어 FVC2002의 GUI 보고서에서 100쌍으로 측정한 EER은 **6.19%**(추정 임계값 0.807)이고, 발표 표에서 전체 원본 3,160쌍으로 측정한 EER은 **6.48%**(0.788)입니다. [GUI 보고서 예시](reports/fingerprint-report-20261126-153655.html) · [최종 전체 원본 결과](log/cpp-port-verification/native-parity.csv)

`reports/fingerprint_evaluation.html`에는 DB별 전처리를 갱신하기 전 결과가 보관돼 있습니다. [이전 평가 보고서](reports/fingerprint_evaluation.html)

이전 원본·가우시안 노이즈 실험에서 생성한 그래프는 `reports/assets/`에 있습니다. [점수 분포](reports/assets/score-distributions.svg) · [DET](reports/assets/det-curves.svg) · [임계값별 오류율](reports/assets/threshold-curves.svg)

| 원본 DB | 초기 DeepPrint EER · 가중치 튜닝 전 |
| --- | ---: |
| FVC2000 | 2.50% |
| FVC2002 | 7.99% |
| FVC2004 | 12.27% |

초기 표와 최종 검증 표는 과제 8쪽의 교차점 계산법을 사용합니다. 노이즈 전용 실행의 `summary.csv`는 선형보간을 사용해 일부 EER 값이 다릅니다. 초기 FVC2004 평가에서 FMR을 0.1% 이하로 제한했을 때 관측 FNMR은 91.07%였습니다.

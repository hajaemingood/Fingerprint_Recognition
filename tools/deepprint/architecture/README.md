# DeepPrint TexMinu 추론 그래프

이 디렉터리는 C++ 변환기의 입력 데이터입니다. 원본은 flx의 `DeepPrint_TexMinu`와 FVC 이진화 설정(능선 폭 1.8)입니다. 고정한 upstream 커밋은 `7accfca1f33b9b42bfd220e43cd5bc13b4a7fa13`입니다. [원본 소스](https://github.com/tim-rohwedder/fixed-length-fingerprint-extractors/tree/7accfca1f33b9b42bfd220e43cd5bc13b4a7fa13) · [원본 라이선스](LICENSE)

- `texminu.graph`는 ONNX opset 17 ModelProto입니다. 기존 검증 모델에서 학습 텐서 394개의 raw_data를 비우고 연산, 형상, 이진화 필터, 리사이즈 계수를 보존했습니다.
- `parameters.tsv`는 체크포인트의 압축되지 않은 float32 저장 위치를 바이트 오프셋으로 기록합니다. `copy`는 직접 복사하고, `kernel`과 `bias`는 해당 합성곱 및 추론 모드 BatchNorm의 파라미터를 결합합니다. 고정한 체크포인트의 SHA-256은 `2683a04427bacd54adc00cfdc97474625b1e11e5a9e6672c5129f033018f8a28`입니다.
- C++ 변환기는 체크포인트 해시, 텐서 개수, 차원, 읽기 길이를 확인합니다. 출력은 `image [1,1,299,299]`를 입력으로 받아 `embedding [1,512]`를 반환하는 독립 ONNX 파일입니다. 훈련 후보는 선택한 텐서를 이 그래프에 다시 기록합니다.

# 18. Phase 4 — WIC 이미지 디코더 (체커보드 → 실제 텍스처) 🖼️

- **날짜**: 2026-05-18
- **관련 커밋**: (이 단계 커밋)
- **소요 시간**: 약 20분
- **단계**: Phase 4 자산 파이프라인 (1/3)

---

## 1. 목표

8×8 체커보드 코드 생성 알베도를 폐기하고, JPG/PNG 등 실제 이미지 자산을 WIC 로 RGBA8 디코드해 Texture 에 업로드.

## 2. 사전 컨텍스트

직전까지 11단계의 8×8 체커보드 픽셀을 main.cpp 에서 직접 생성. 17단계 FBX 도입으로 Dragon 메시는 로드되지만 알베도는 여전히 체커보드. *실제 자산 파이프라인의 마지막 1차 조각*이 텍스처 디코더.

## 3. 결정과 트레이드오프

### 18-1. WIC (Windows Imaging Component) — 외부 의존 0
- **결정**: Windows SDK 내장 WIC API 사용.
- **후보**:
  - A) stb_image (single-header, public domain) — 자체 완결 단일 파일.
  - B) WIC — Windows SDK 의 일부, OS 내장 코덱.
  - C) DirectXTex (Microsoft 공식, DDS 특화).
- **선택 이유**: B — FBX SDK 도입(17단계)으로 옵션 A 원칙은 이미 한 번 깨졌지만, *이번엔 OS 표준 인터페이스* 라 외부 의존 추가 없음. JPG/PNG/BMP/TIFF/DDS 등 다양한 포맷 지원.
- **포기한 것**: 비-Windows 이식성. 본 프로젝트는 Windows 전용이라 무관.

### 18-2. 결과는 항상 RGBA8 정규화
- **결정**: `IWICFormatConverter` 로 원본 포맷(JPG의 YCbCr, PNG 의 8bpp 등) 무관 `GUID_WICPixelFormat32bppRGBA` 로 강제 변환.
- **이유**: Texture 클래스가 RGBA8 픽셀만 받음 (11단계). 호출자가 포맷을 모를 자유 + Texture 단순화 양쪽.
- **포기한 것**: HDR/16bpp 포맷의 정밀도 — 본 단계 시각화에 무의미.

### 18-3. CoInitialize 는 함수 내부에서 처리
- **결정**: `image_loader::LoadImage` 가 호출 시 `CoInitializeEx(STA)` 호출. S_FALSE/RPC_E_CHANGED_MODE 는 idempotent 통과.
- **후보**: 메인에서 1회 명시 초기화.
- **선택 이유**: 호출자가 WIC 의존성을 *몰라야* 모듈 캡슐화 완성. 단일 스레드 메인 루프 가정.
- **포기한 것**: 멀티스레드 자산 로딩. 그 시점엔 별도 초기화 정책.

### 18-4. ImageData 는 plain struct + vector
- **결정**: `struct ImageData { vector<uint8> pixels; uint32 width; height; }`. 동적 클래스 아님.
- **이유**: 데이터의 일시 운반체. 호출 측에서 즉시 Texture 에 넘기고 폐기. 라이프타임 추상화 불필요.

## 4. 작업 내용

### 4-1. ImageLoader 모듈
- 위치: [Engine/render/ImageLoader.h](../Engine/render/ImageLoader.h), [.cpp](../Engine/render/ImageLoader.cpp)
- 흐름:
  1. `CoInitializeEx(APARTMENTTHREADED)` — 멱등.
  2. `CoCreateInstance(WICImagingFactory)`.
  3. `factory->CreateDecoderFromFilename(...)` — 절대 경로 (UTF-16).
  4. `decoder->GetFrame(0)` — 단일 프레임 (멀티 프레임 GIF/TIFF 는 본 단계 X).
  5. `factory->CreateFormatConverter` + `Initialize(frame, RGBA8)`.
  6. `converter->GetSize` + `CopyPixels(rowPitch = w*4, bufferSize)`.
- `#pragma comment(lib, "windowscodecs.lib")` 로 의존 라이브러리 자체 선언 (vcxproj 변경 불필요).

### 4-2. main.cpp 교체
체커보드 33줄 코드를 4줄로 압축:
```cpp
const std::wstring texDir   = engine::render::fbx_loader::DefaultFbxDir() + L"..\\Texture\\";
const std::wstring texPath  = texDir + L"Leather.jpg";
const engine::render::ImageData albedoImg =
    engine::render::image_loader::LoadImage(texPath.c_str());
engine::render::Texture albedoTex(
    device, commandQueue, *cmdLists[0],
    albedoImg.pixels.data(), albedoImg.width, albedoImg.height);
```

`fbx_loader::DefaultFbxDir()` 가 `exe/Resources/FBX/` 반환 — `..\Texture\` 로 형제 폴더 접근. 임시 단순화 (다음 단계의 머티리얼 sub-draw 에서 머티리얼별 자동 매핑).

## 5. 마주친 문제와 해결 ⚠ 필수 — 빠짐없이 기록

### 문제 1 — CoInitialize 멱등 처리
- **문제**: 다중 호출 시 `RPC_E_CHANGED_MODE` 또는 `S_FALSE` 반환 가능.
- **해결**: 둘 다 정상 처리 — `FAILED(hr) && hr != RPC_E_CHANGED_MODE` 만 throw.
- **교훈**: COM 초기화의 idempotent 패턴 표준 처리.

### 문제 2 — 본 단계는 단일 텍스처 적용
- **문제**: Dragon.fbx 는 다중 머티리얼 (각자 diffuse 텍스처 보유 가능). 본 단계 main 은 Leather.jpg 하나만 적용.
- **현 상황**: 모든 face 가 같은 알베도. 의도된 단순화 (sub-draw 시스템은 다음 단계).
- **교훈**: 자산 파이프라인은 *디코더 → 머티리얼 매핑 → sub-draw* 3 레이어. 1차 디코더만 완성.

## 6. 결과 / 검증

- **빌드**: Debug + Release ExitCode 0.
- **런타임 로그**: `[render] Image loaded (RGBA8 1024x1024, 4194304 bytes): ...Leather.jpg` (가짜 수치, 실제 사용자 확인).
- **시각 기대**: Dragon 메시 표면에 Leather 텍스처 매핑. 8×8 격자 대신 자연스러운 가죽 질감.

## 7. AI 협업 메모

- WIC API 가 COM 인터페이스 6개 사용 — ComPtr 로 라이프타임 자동 정리. 한 곳에서도 raw IUnknown 처리 없음.
- `#pragma comment(lib, ...)` 로 vcxproj 변경 없이 의존 라이브러리 선언. windowscodecs.lib 는 OS 표준이라 lib path 추가 불필요.

## 8. 다음 단계

- **머티리얼 sub-draw 시스템** — FbxLoader 가 머티리얼별 sub-mesh 분리, Material 클래스 + 머티리얼별 SRV, main 이 sub-mesh 마다 SetGraphicsRootDescriptorTable.
- **스키닝 + 애니메이션** — 셰이더 본 팔레트 cbuffer + Skeleton/AnimClip/Animator + FbxLoader 스키닝 확장.

후속:
- mipmap 자동 생성 (현재 mip 1개).
- BC1/BC3 압축 텍스처 (DDS).
- 큐브맵 (skybox).

## 9. PPT 재료로 쓸 만한 포인트

- "WIC 디코더 — Windows SDK 내장 + 외부 의존 0 + 다중 포맷 자동 처리"
- "체커보드 33줄 → 실제 자산 4줄 — 자산 파이프라인 도입의 시각적 효과"
- "Format 정규화 (원본 → RGBA8) 가 Texture 클래스 단순성을 보존"

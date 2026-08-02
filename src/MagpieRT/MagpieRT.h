// MagpieRT v2 — C ABI wrapper around Magpie.Core for external host applications.
//
// Design goals:
//   * Stable C ABI: plain functions + versioned POD struct, no C++ types across
//     the boundary. The host loads this DLL with LoadLibrary/GetProcAddress.
//   * Superset of the legacy MagpieRT 0.8.1 surface (Initialize/Run), plus the
//     capabilities 0.8.1 lacked: Stop(), state query, windowed toggle.
//
// This project is GPL-3.0, same as Magpie.
#pragma once

#include <windows.h>
#include <stdint.h>

#ifdef MAGPIERT_EXPORTS
#define MAGPIERT_API extern "C" __declspec(dllexport)
#else
#define MAGPIERT_API extern "C" __declspec(dllimport)
#endif

// Mirrors Magpie::CaptureMethod
enum MagpieRT_CaptureMethod {
	MagpieRT_Capture_GraphicsCapture = 0,
	MagpieRT_Capture_DesktopDuplication = 1,
	MagpieRT_Capture_GDI = 2,
	MagpieRT_Capture_DwmSharedSurface = 3,
};

// Mirrors Magpie::ScalingState
enum MagpieRT_State {
	MagpieRT_State_Idle = 0,
	MagpieRT_State_Scaling = 1,
	MagpieRT_State_Waiting = 2,
};

enum MagpieRT_Flags {
	MagpieRT_Flag_NoCursor = 0x1,                     // hide cursor while scaling
	MagpieRT_Flag_AdjustCursorSpeed = 0x2,
	MagpieRT_Flag_SimulateExclusiveFullscreen = 0x8,  // values match legacy 0.8.1 masks
	MagpieRT_Flag_3DGameMode = 0x100,
	MagpieRT_Flag_WindowedMode = 0x10000,             // new in v2
	MagpieRT_Flag_DisableEffectCache = 0x400,
};

// Effect chain presets built from the bundled effects directory. Presets
// without a built-in sharpness parameter get an FSR_RCAS pass appended when
// sharpness > 0 (except Nearest, where sharpening defeats the point).
enum MagpieRT_Effect {
	MagpieRT_Effect_FSR = 0,       // FSR_EASU + FSR_RCAS(sharpness)
	MagpieRT_Effect_Lanczos = 1,   // Lanczos + optional RCAS
	MagpieRT_Effect_SGSR = 2,      // Snapdragon GSR + optional RCAS
	MagpieRT_Effect_NIS = 3,       // NVIDIA Image Scaling (sharpness)
	MagpieRT_Effect_CAS = 4,       // AMD CAS scaling (sharpness)
	MagpieRT_Effect_Anime4K = 5,   // Anime4K_Upscale_S (2x) + Bicubic + optional RCAS
	MagpieRT_Effect_Nearest = 6,   // nearest neighbour, for pixel-art titles
	MagpieRT_Effect_CRTGeom = 7,   // curved-tube retro CRT (scanlines, phosphor mask)
	MagpieRT_Effect_COUNT,
};

struct MagpieRT_StartParams {
	uint32_t structSize;        // = sizeof(MagpieRT_StartParams), ABI guard
	HWND hwndSrc;
	// Effect chain: currently the FSR pair. sharpness in [0,1] feeds FSR_RCAS.
	float sharpness;
	int captureMethod;          // MagpieRT_CaptureMethod
	float maxFrameRate;         // <=0: unlimited
	float minFrameRate;         // <=0: default
	uint32_t flags;             // MagpieRT_Flags
	float cursorScaling;        // <=0: same as source
	int cursorInterpolationMode; // 0 nearest, 1 bilinear
	int graphicsAdapterIdx;     // -1: default
	int effectPreset;           // MagpieRT_Effect; out-of-range falls back to FSR
};

// logDir: directory for magpie-rt.log; NULL for current directory.
MAGPIERT_API void MagpieRT_Initialize(const wchar_t* logDir);

// Starts scaling. Returns 0 on success, otherwise a Magpie::ScalingError value
// (>0) or -1 for invalid arguments. Asynchronous: poll MagpieRT_GetState().
MAGPIERT_API int MagpieRT_Start(const MagpieRT_StartParams* params);

MAGPIERT_API void MagpieRT_Stop(void);

MAGPIERT_API int MagpieRT_GetState(void);

MAGPIERT_API void MagpieRT_ToggleScaling(BOOL windowedMode);

// Scaling runs asynchronously; a session that fails after MagpieRT_Start
// returned 0 reports its Magpie::ScalingError here (0 = no error). Reset by
// the next MagpieRT_Start call.
MAGPIERT_API int MagpieRT_GetLastError(void);

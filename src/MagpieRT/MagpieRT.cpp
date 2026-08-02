#include "pch.h"
#include "MagpieRT.h"

#include <ScalingRuntime.h>
#include <ScalingOptions.h>
#include <Logger.h>

using namespace Magpie;

namespace {

ScalingRuntime* g_runtime = nullptr;

ScalingRuntime& Runtime() {
	if (!g_runtime) {
		g_runtime = new ScalingRuntime();
	}
	return *g_runtime;
}

} // namespace

void MagpieRT_Initialize(const wchar_t* logDir) {
	std::filesystem::path logPath = logDir ? logDir : L".";
	logPath /= L"magpie-rt.log";
	Logger::Get().Initialize(spdlog::level::info, logPath.native().c_str(), 100000, 2);
}

int MagpieRT_Start(const MagpieRT_StartParams* params) {
	if (!params || params->structSize < sizeof(MagpieRT_StartParams) ||
		!IsWindow(params->hwndSrc)) {
		return -1;
	}

	ScalingOptions options;

	// FSR 链: EASU 铺满目标, RCAS 锐化。与旧版 0.8.1 效果语义对齐。
	EffectOption easu;
	easu.name = "FSR\\FSR_EASU";
	easu.scalingType = ScalingType::Fill;
	EffectOption rcas;
	rcas.name = "FSR\\FSR_RCAS";
	float sharpness = params->sharpness;
	if (sharpness < 0.0f) sharpness = 0.0f;
	if (sharpness > 1.0f) sharpness = 1.0f;
	rcas.parameters["sharpness"] = sharpness;
	options.effects = { std::move(easu), std::move(rcas) };

	if (params->captureMethod >= 0 &&
		params->captureMethod < (int)CaptureMethod::COUNT) {
		options.captureMethod = (CaptureMethod)params->captureMethod;
	}

	if (params->maxFrameRate > 0.0f) {
		options.maxFrameRate = params->maxFrameRate;
	}
	if (params->minFrameRate > 0.0f) {
		options.minFrameRate = params->minFrameRate;
	}

	options.flags = 0;
	if (params->flags & MagpieRT_Flag_AdjustCursorSpeed) {
		options.flags |= ScalingFlags::AdjustCursorSpeed;
	}
	if (params->flags & MagpieRT_Flag_SimulateExclusiveFullscreen) {
		options.flags |= ScalingFlags::SimulateExclusiveFullscreen;
	}
	if (params->flags & MagpieRT_Flag_3DGameMode) {
		options.flags |= ScalingFlags::Is3DGameMode;
	}
	if (params->flags & MagpieRT_Flag_WindowedMode) {
		options.flags |= ScalingFlags::WindowedMode;
	}
	if (params->flags & MagpieRT_Flag_DisableEffectCache) {
		options.flags |= ScalingFlags::DisableEffectCache;
	}

	// 旧版 NoCursor 语义: 缩放期间不绘制光标 -> 光标缩放为 0 不可表达,
	// 新版通过 autoHideCursorDelay=0 近似(立即隐藏, 移动时短暂可见)。
	if (params->flags & MagpieRT_Flag_NoCursor) {
		options.autoHideCursorDelay = 0.0f;
	}

	if (params->cursorScaling > 0.0f) {
		options.cursorScaling = params->cursorScaling;
	}
	if (params->cursorInterpolationMode == 1) {
		options.cursorInterpolationMode = CursorInterpolationMode::Bilinear;
	}
	if (params->graphicsAdapterIdx >= 0) {
		options.graphicsCardId.idx = params->graphicsAdapterIdx;
	}

	// 宿主自己没有工具栏交互, 默认关闭, 避免游戏画面上多出 UI。
	options.fullscreenInitialToolbarState = ToolbarState::Off;
	options.windowedInitialToolbarState = ToolbarState::Off;

	const bool ok = Runtime().Start(
		params->hwndSrc, std::move(options),
		/*force*/ (params->flags & MagpieRT_Flag_SimulateExclusiveFullscreen) != 0);
	return ok ? 0 : (int)ScalingError::ScalingFailedGeneral;
}

void MagpieRT_Stop(void) {
	if (g_runtime) {
		g_runtime->Stop();
	}
}

int MagpieRT_GetState(void) {
	return g_runtime ? (int)g_runtime->State() : MagpieRT_State_Idle;
}

void MagpieRT_ToggleScaling(BOOL windowedMode) {
	Runtime().ToggleScaling(windowedMode != FALSE);
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_DETACH && g_runtime) {
		// 进程退出路径不做完整析构(缩放线程 join 可能死锁), 交给系统回收
		g_runtime = nullptr;
	}
	return TRUE;
}

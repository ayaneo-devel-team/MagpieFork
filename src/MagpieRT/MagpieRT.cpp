#include "pch.h"
#include "MagpieRT.h"

#include <ScalingRuntime.h>
#include <ScalingOptions.h>
#include <Logger.h>

#include <atomic>

using namespace Magpie;

namespace {

ScalingRuntime* g_runtime = nullptr;
std::atomic<int> g_lastError{ 0 };

ScalingRuntime& Runtime() {
	if (!g_runtime) {
		g_runtime = new ScalingRuntime();
	}
	return *g_runtime;
}

// ScalingOptions 的回调是裸函数指针且默认 nullptr, ScalingWindow 在
// Release 下不做判空, 宿主必须全部提供, 否则启动失败路径会调用空指针。
void HostShowToast(HWND, std::wstring_view) noexcept {}

void HostShowError(HWND, ScalingError error) noexcept {
	g_lastError = (int)error;
	Logger::Get().Error(fmt::format("scaling session failed, error: {}", (int)error));
	Logger::Get().Flush();
}

void HostSave(const ScalingOptions&, HWND) noexcept {}

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

	float sharpness = params->sharpness;
	if (sharpness < 0.0f) sharpness = 0.0f;
	if (sharpness > 1.0f) sharpness = 1.0f;

	int preset = params->effectPreset;
	if (preset < 0 || preset >= MagpieRT_Effect_COUNT) {
		preset = MagpieRT_Effect_FSR;
	}

	// 铺满目标的主效果; 自带 sharpness 参数的直接吃锐化值
	auto makeFill = [](const char* name) {
		EffectOption e;
		e.name = name;
		e.scalingType = ScalingType::Fill;
		return e;
	};
	// 主效果没有锐化参数时补一道 RCAS(sharpness>0 才加)
	auto appendRcas = [&](std::vector<EffectOption>& effects) {
		if (sharpness > 0.0f) {
			EffectOption rcas;
			rcas.name = "FSR\\FSR_RCAS";
			rcas.parameters["sharpness"] = sharpness;
			effects.push_back(std::move(rcas));
		}
	};

	switch (preset) {
	case MagpieRT_Effect_Lanczos:
	{
		options.effects = { makeFill("Lanczos") };
		appendRcas(options.effects);
		break;
	}
	case MagpieRT_Effect_SGSR:
	{
		options.effects = { makeFill("SGSR") };
		appendRcas(options.effects);
		break;
	}
	case MagpieRT_Effect_NIS:
	{
		EffectOption nis = makeFill("NIS\\NIS");
		nis.parameters["sharpness"] = sharpness;
		options.effects = { std::move(nis) };
		break;
	}
	case MagpieRT_Effect_CAS:
	{
		EffectOption cas = makeFill("CAS\\CAS_Scaling");
		cas.parameters["sharpness"] = sharpness;
		options.effects = { std::move(cas) };
		break;
	}
	case MagpieRT_Effect_Anime4K:
	{
		// Anime4K_Upscale_S 固定 2x 输出, 再用 Bicubic 铺满目标
		EffectOption anime;
		anime.name = "Anime4K\\Anime4K_Upscale_S";
		options.effects = { std::move(anime), makeFill("Bicubic") };
		appendRcas(options.effects);
		break;
	}
	case MagpieRT_Effect_Nearest:
	{
		// 像素风整数放大, 锐化只会破坏硬边缘, 不追加 RCAS
		options.effects = { makeFill("Nearest") };
		break;
	}
	case MagpieRT_Effect_FSR:
	default:
	{
		EffectOption rcas;
		rcas.name = "FSR\\FSR_RCAS";
		rcas.parameters["sharpness"] = sharpness;
		options.effects = { makeFill("FSR\\FSR_EASU"), std::move(rcas) };
		break;
	}
	}

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

	options.showToast = HostShowToast;
	options.showError = HostShowError;
	options.save = HostSave;
	// 截图功能不经宿主暴露, 但成员不允许为空
	options.screenshotsDir = L".";

	g_lastError = 0;
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

int MagpieRT_GetLastError(void) {
	return g_lastError;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_DETACH && g_runtime) {
		// 进程退出路径不做完整析构(缩放线程 join 可能死锁), 交给系统回收
		g_runtime = nullptr;
	}
	return TRUE;
}

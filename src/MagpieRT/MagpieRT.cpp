#include "pch.h"
#include "MagpieRT.h"

#include <ScalingRuntime.h>
#include <ScalingOptions.h>
#include <Logger.h>
#include <StrHelper.h>

#include <atomic>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>

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

std::vector<std::wstring> SplitW(std::wstring_view text, wchar_t sep) {
	std::vector<std::wstring> parts;
	size_t begin = 0;
	while (begin <= text.size()) {
		size_t end = text.find(sep, begin);
		if (end == std::wstring_view::npos) {
			parts.emplace_back(text.substr(begin));
			break;
		}
		parts.emplace_back(text.substr(begin, end - begin));
		begin = end + 1;
	}
	return parts;
}

float ToFloat(const std::wstring& s, float fallback) {
	try {
		return s.empty() ? fallback : std::stof(s);
	} catch (...) {
		return fallback;
	}
}

// 解析 optionsText(格式见 MagpieRT.h)。effect 行存在时整链替换 effectPreset。
void ApplyOptionsText(ScalingOptions& options, const wchar_t* text) {
	if (!text || !*text) {
		return;
	}

	std::vector<EffectOption> chain;
	for (const std::wstring& line : SplitW(text, L'\n')) {
		if (line.empty()) {
			continue;
		}
		std::vector<std::wstring> f = SplitW(line, L'|');
		const std::wstring& kind = f[0];

		if (kind == L"effect" && f.size() >= 2 && !f[1].empty()) {
			EffectOption e;
			e.name = StrHelper::UTF16ToUTF8(f[1]);
			if (f.size() >= 3) {
				int st = (int)ToFloat(f[2], 3.0f);
				if (st >= 0 && st <= 3) {
					e.scalingType = (ScalingType)st;
				}
			}
			if (f.size() >= 5) {
				e.scale = { ToFloat(f[3], 1.0f), ToFloat(f[4], 1.0f) };
			}
			if (f.size() >= 6 && !f[5].empty()) {
				for (const std::wstring& kv : SplitW(f[5], L',')) {
					size_t eq = kv.find(L'=');
					if (eq != std::wstring::npos && eq > 0) {
						e.parameters[StrHelper::UTF16ToUTF8(kv.substr(0, eq))] =
							ToFloat(kv.substr(eq + 1), 0.0f);
					}
				}
			}
			chain.push_back(std::move(e));
		} else if (kind == L"cropping" && f.size() >= 5) {
			options.cropping = { ToFloat(f[1], 0.0f), ToFloat(f[2], 0.0f),
			                     ToFloat(f[3], 0.0f), ToFloat(f[4], 0.0f) };
		} else if (kind == L"dupframe" && f.size() >= 2) {
			int mode = (int)ToFloat(f[1], 1.0f);
			if (mode >= 0 && mode <= 2) {
				options.duplicateFrameDetectionMode = (DuplicateFrameDetectionMode)mode;
			}
		} else if (kind == L"windowedscale" && f.size() >= 2) {
			float factor = ToFloat(f[1], 0.0f);
			if (factor >= 0.0f) {
				options.initialWindowedScaleFactor = factor;
			}
		}
	}

	if (!chain.empty()) {
		options.effects = std::move(chain);
	}
}

// 枚举 effectsDir 下 .hlsl 头部声明的参数元数据, 供宿主生成设置界面
std::wstring& ListEffectsBuffer() {
	static std::wstring buffer;
	return buffer;
}

void ParseEffectFile(const std::filesystem::path& file, const std::wstring& relName, std::wstring& out) {
	std::ifstream stream(file);
	if (!stream) {
		return;
	}

	// 声明块前允许有描述注释, 在前若干行内找效果标记
	std::string line;
	bool isEffect = false;
	for (int i = 0; i < 20 && std::getline(stream, line); ++i) {
		if (line.find("//!MAGPIE EFFECT") != std::string::npos) {
			isEffect = true;
			break;
		}
	}
	if (!isEffect) {
		return;
	}

	std::wstring entry = relName;
	// 参数块: //!PARAMETER + LABEL/DEFAULT/MIN/MAX/STEP, 随后的变量声明行是参数名
	bool inParam = false;
	std::string label, def, minV, maxV, step;
	int lineCount = 0;
	while (std::getline(stream, line) && ++lineCount < 300) {
		auto value = [&](const char* prefix) -> std::string {
			size_t len = strlen(prefix);
			if (line.compare(0, len, prefix) != 0) {
				return {};
			}
			size_t begin = line.find_first_not_of(' ', len);
			return begin == std::string::npos ? std::string() : line.substr(begin);
		};

		if (line.rfind("//!PASS", 0) == 0) {
			break; // 参数都在 PASS 之前
		}
		if (line.rfind("//!PARAMETER", 0) == 0) {
			inParam = true;
			label = def = minV = maxV = step = {};
			continue;
		}
		if (!inParam) {
			continue;
		}
		if (std::string v = value("//!LABEL"); !v.empty()) { label = v; continue; }
		if (std::string v = value("//!DEFAULT"); !v.empty()) { def = v; continue; }
		if (std::string v = value("//!MIN"); !v.empty()) { minV = v; continue; }
		if (std::string v = value("//!MAX"); !v.empty()) { maxV = v; continue; }
		if (std::string v = value("//!STEP"); !v.empty()) { step = v; continue; }
		if (line.rfind("//!", 0) == 0) {
			continue; // 其他声明(如紧随的下一个 PARAMETER 前缀行)
		}

		// 参数块后的第一个非声明行应是 "float name;" 变量声明
		size_t typeEnd = line.find("float ");
		if (typeEnd != std::string::npos) {
			size_t nameBegin = typeEnd + 6;
			size_t nameEnd = line.find(';', nameBegin);
			if (nameEnd != std::string::npos) {
				std::string name = line.substr(nameBegin, nameEnd - nameBegin);
				while (!name.empty() && name.back() == ' ') name.pop_back();
				std::string meta = name + ":" + label + ":" + def + ":" + minV + ":" + maxV + ":" + step;
				// 元数据里的分隔符不允许出现在字段内, label 若含冒号会破坏格式, 替换掉
				entry += L"|" + StrHelper::UTF8ToUTF16(meta);
			}
		}
		inParam = false;
	}

	out += entry;
	out += L"\n";
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
	case MagpieRT_Effect_CRTGeom:
	{
		// 球面显像管复古效果(默认曲率开启); 锐化会破坏扫描线与荫罩质感, 不追加 RCAS
		options.effects = { makeFill("CRT\\CRT_Geom") };
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

	// 扩展选项(自定义效果链/裁剪/重复帧检测/初始窗口化倍率)
	if (params->structSize >= sizeof(MagpieRT_StartParams)) {
		ApplyOptionsText(options, params->optionsText);
	}

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

const wchar_t* MagpieRT_ListEffects(const wchar_t* effectsDir) {
	if (!effectsDir || !*effectsDir) {
		return nullptr;
	}

	std::error_code ec;
	std::filesystem::path root(effectsDir);
	if (!std::filesystem::is_directory(root, ec)) {
		return nullptr;
	}

	std::wstring& buffer = ListEffectsBuffer();
	buffer.clear();

	for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
	     it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
		if (ec || !it->is_regular_file(ec)) {
			continue;
		}
		const std::filesystem::path& p = it->path();
		if (p.extension() != L".hlsl") {
			continue;
		}
		std::wstring rel = std::filesystem::relative(p, root, ec).replace_extension().native();
		if (ec) {
			continue;
		}
		ParseEffectFile(p, rel, buffer);
	}

	return buffer.c_str();
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_DETACH && g_runtime) {
		// 进程退出路径不做完整析构(缩放线程 join 可能死锁), 交给系统回收
		g_runtime = nullptr;
	}
	return TRUE;
}

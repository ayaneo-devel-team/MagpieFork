# MagpieRT

面向外部宿主程序的 C ABI 封装 DLL，包装 `Magpie.Core` 的 `ScalingRuntime`。
接口契约见 `MagpieRT.h`：`MagpieRT_Initialize / Start / Stop / GetState /
ToggleScaling`，参数为带 `structSize` 版本守卫的 POD 结构。

与旧版（基于 Magpie 0.8.1 Runtime 的同类封装，导出 `Initialize/Run`）的差异：

- 新增 `Stop` 与状态查询——宿主可以把缩放做成可开关。
- 新增窗口化缩放切换（`ToggleScaling`）。
- 效果链目前固定为 `FSR\FSR_EASU`（Fill）+ `FSR\FSR_RCAS`（锐化可调），
  后续可扩展为完整的效果链参数。

## 构建

依赖与 Magpie 主工程相同（VS2022 17.10+、C++/WinRT NuGet、Conan）。先按
上游文档完成一次解决方案的依赖准备（生成 `obj/.../_ConanDeps`），再：

```powershell
msbuild src/MagpieRT/MagpieRT.vcxproj -p:Configuration=Release -p:Platform=x64
```

产物与 `src/Effects/` 一同分发给宿主。本项目与 Magpie 同为 GPL-3.0。

## 待验证

- 首次完整构建（Conan 依赖 + FxCompile 着色器）尚未在 CI 跑通。
- 效果着色器在新版由 `EffectCompiler` 运行期编译，查找路径相对进程工作
  目录还是模块目录，需在宿主进程实测后决定分发布局。

# divaps4 — 《歌姬计划 X HD》画面偏暗与 AVC 播放崩溃修复说明

本文说明两个问题的成因、修复思路与改动清单。基线为 `shadps4-emu/shadPS4` @ `0614ce3`，
已包含上游 PR #3420（`ir: Perform degamma in shader when sampler sets force_degamma`）。

---

## 一、现象与排除过程

| 场景 | 表现 |
| --- | --- |
| 主标题 | 整体偏暗 |
| 游戏内菜单 | 正常 |
| PV 播放 | 偏暗 |
| 不同歌曲 | 明暗不一致 |

先排除两个常见方向：

1. **不是输出端 gamma 问题。** 内置 Ctrl+F10 / 画面滤镜作用在最终呈现阶段，是一条全局幂
   函数曲线。而实测"亮部与暗部变暗程度不同"——说明误差不是 `out = in^k` 形式，而是发生在
   管线**中间**（采样之后、光照/混合之前），后续的乘法又把它放大了。任何输出端曲线都无法
   反解。
2. **不是 PR #3420 没生效。** 该 PR 的 `ApplyForceDegamma` 在本基线里已经存在
   （`resource_patching_pass.cpp`），但它只覆盖"guest 声明 `nfmt == Unorm` 且 sampler 置了
   `force_degamma`"这一条路径。

真正的成因在**纹理缓存的格式别名**上。

---

## 二、根因：sRGB 语义随分配历史漂移

### 2.1 三个事实

**事实 A —— Vulkan 允许 sRGB 与其线性版本互相别名。**

`src/video_core/texture_cache/host_compatibility.cpp` 的 `FORMAT_TABLE` 把
`eR8G8B8A8Unorm` 和 `eR8G8B8A8Srgb` 都归进同一个 `CompatibilityClass::_32BIT`，于是

```cpp
bool IsVulkanFormatCompatible(vk::Format base, vk::Format view) {
    return (base_comp & view_comp) == view_comp;   // Unorm <-> Srgb 互相成立
}
```

`ImageInfo::IsCompatible()` 用的就是它，所以一个 sRGB 图像和一个线性图像会被判定为**同一块
资源**，缓存直接复用。

**事实 B —— 图像的实际格式由"谁先分配它"决定。**

`Image` 的 `vk::Image` 只在创建时定格式，来源可能是颜色缓冲（`ImageInfo(ColorBuffer)`）、
也可能是第一个采样它的 T#（`ImageInfo(Image, ImageResource)`）。

**事实 C —— view 的格式由"当前读它的描述符"决定。**

`ImageViewInfo(AmdGpu::Image, ImageResource)`：

```cpp
format = Vulkan::LiverpoolToVK::SurfaceFormat(dfmt, nfmt);   // 跟随 T# 的 nfmt
```

三者叠加的结果：**同一个表面，被谁先摸到、被谁后读，决定了它是否多解一次 gamma。**

### 2.2 为什么症状正好是"暗、且亮暗程度不同"

设内容以线性写入（CB 的 `nfmt == Unorm`，shader 输出原样落盘），随后被一个 `nfmt == Srgb`
的 T# 采样。此时：

- 硬件在 view 上做了一次 sRGB 解码：`0.5 → 0.214`，`1.0 → 1.0`，`0.05 → 0.0039`。
- 中间调被压低约 2.4 次幂，高光几乎不动。

这就是"亮部与暗部变暗程度不同"的来源，也解释了为什么菜单正常（走的是另一组表面，创建时
恰好是 sRGB，只解一次）、标题偏暗、不同歌曲明暗不一（RT 池复用顺序不同）。

### 2.3 为什么 `readback_linear_images` 能"修好"却又很慢

打开后，`FindRenderTarget` / `FindTexture` 会把每个线性 RT 塞进 `download_images`，
`DownloadImageMemory()` 把 GPU 内容回读到 guest 内存，随后 `DeleteImage()` 销毁该图像。
下一次采样时图像被**重建**，格式取自采样者的描述符——别名被打断，画面就对了。

代价有两层：

1. 每个 RT 每帧一次完整 GPU→CPU 拷贝。
2. 更致命的是 `DownloadImageMemory()` 在 `Sync` 模式下**逐张** `scheduler.Finish()`。
   `ProcessDownloadImages()` 从 `Rasterizer::OnSubmit()` 调用，于是一帧内对十几个 1080p
   RT 各做一次**整条管线 flush + 阻塞**。这不是带宽问题，是同步问题——所以帧率是断崖式下跌。

---

## 三、修复思路

### 3.1 核心不变式

> **图像内容怎么编码，由写它的人决定；读它的人只能解释，不能重新定义。**

落地为一条单向钳制：

```
若 view 请求 sRGB 解码，但图像内容不是 gamma 编码的 → 该 view 降级为线性格式，不解码。
```

反向（内容编码、读者要原始字节）**保持不动**——guest 完全可能自己要原始值，然后在 shader
里通过 `sampler.force_degamma` 自己解（PR #3420 的路径）。这样任何一次采样都**至多解码一次**，
不会出现双重解码。

### 3.2 为什么这样是"零成本"的

- 不做任何回读，不需要 GPU→CPU 拷贝。
- 不增加内存，不额外分配图像（别名照旧复用）。
- 不增加 draw call / pass。
- view 本来就有按 `ImageViewInfo` 的缓存（`Image::FindView`），格式变化只影响 view 的
  缓存键，不改变热路径的复杂度。
- shader 侧不需要新的分支：解码决策仍由描述符（spec constant）决定，运行时无分支。

### 3.3 顺手修掉的两个真实缺陷

1. **PR #3420 的死代码**：`converted` 算完被丢弃，`ApplyForceDegamma` 作用在原始 texel 上。
   `NumberConversion::None` 时两者等价，所以一直没暴露；`SrgbToNorm` / `UnormToUbnorm` 会
   静默丢失转换。同时给整数/深度格式加了跳过（它们没有 gamma 语义，不该走 `pow`）。
2. **回读的同步粒度**：同一次 drain 中改为"先记录全部拷贝 → 只 flush 一次 → 统一做 host 回写"。
   但这里还存在按 guest PM4 提交触发 drain 的频率问题（见后续性能更新），单纯批处理并不足以消除掉帧。

---

## 四、改动清单

### 4.1 改名（`a16ca72`）

| 文件 | 改动 |
| --- | --- |
| `CMakeLists.txt` | `project(shadPS4)` → `project(divaps4)`；新增 `OUTPUT_NAME divaps4` |
| `src/emulator.cpp` | 四处窗口标题 `shadPS4 v{}` → `divaps4 v{}` |
| `src/dist/shadps4.rc` | ProductName / InternalName / OriginalFilename / FileDescription |
| `src/common/discord_rpc_handler.cpp` | Rich Presence 文案 |
| `src/core/libraries/system/userservice.cpp` | 用户名默认值 |

CMake target 仍叫 `shadps4`，避免改动 20 多处 `target_link_libraries` 与 CI 脚本；
**用户数据目录保持 `shadPS4` 不变**，已有配置/存档/奖杯可直接沿用。

### 4.2 着色器解码修正（`931be15`）

`src/shader_recompiler/ir/passes/resource_patching_pass.cpp` — `PatchImageSampleArgs()`

```cpp
auto converted = ApplyReadNumberConversionVec4(ir, texel, image.GetNumberConversion());
if (sampler.force_degamma && image.GetNumberFmt() != AmdGpu::NumberFormat::Srgb &&
    !AmdGpu::IsInteger(image.GetNumberFmt()) && !inst_info.is_depth) {
    converted = ApplyForceDegamma(ir, converted);   // 原来是 ir, texel
}
```

### 4.3 sRGB 语义钳制（`b70705c`）

| 文件 | 改动 |
| --- | --- |
| `host_compatibility.h/.cpp` | 新增 `IsSrgbFormat()`、`ToggleSrgb()`，覆盖 R8/RG8/RGB8/RGBA8 的 R/B 两种排列、`A8B8G8R8SrgbPack32`、BC1/BC2/BC3/BC7；浮点/整数/深度/BC4/BC5/BC6 原样返回 |
| `image.h` | 新增 `bool srgb_encoded`，语义是"当前内容的编码方式"，不是"读者的期望" |
| `image.cpp` | 构造时 `srgb_encoded = IsSrgbFormat(info.pixel_format)` |
| `texture_cache.cpp` | `FindRenderTarget()` 用 CB 格式刷新 `srgb_encoded`；`FindTexture()` 对采样 view 做单向钳制 |
| `texture_cache.h/.cpp` | `DownloadImageMemory()` 增加可选的延迟回写出参；`ProcessDownloadImages()` 同批次只停顿一次，并可用 trace 输出图片数/字节数/耗时 |
| `renderer_vulkan/vk_rasterizer.cpp` | 不再在每个 guest PM4 submit 后同步排空回读队列；改在 guest-visible fence 之前排空，合并多个 submit 的重复回读 |

关键代码（`FindTexture`）：

```cpp
ImageViewInfo view_info = desc.view_info;
if (IsSrgbFormat(view_info.format) && !image.srgb_encoded) {
    view_info.format = ToggleSrgb(view_info.format, false);
}
return image.FindView(view_info);
```

### 4.4 AVC 崩溃修复（`a3fc9fa`）

| 文件 | 缺陷 | 修复 |
| --- | --- | --- |
| `videodec/video_utils.cpp` | `ASSERT(((pitch*height*3)/2) <= max_size)`，而 `max_size` 是**由帧自身推导**的 `fb_size`，恒真；release 下等于没有检查 | 改为传入目标缓冲区**真实容量**，装不下就拒绝并返回 `false` |
| 同上 | `cy = (height/2) - 1`，`height < 2` 时无符号下溢 | 前置 `height < 2` 直接拒绝 |
| 同上 | `data[0]` / `data[1]` 为空时直接 `memcpy` | 空指针与非法尺寸检查 |
| `videodec/vdeccore.cpp` | `av_frame_get_buffer` 返回值被忽略；`sws_getCachedContext` 返回 null 仍交给 `sws_scale`（10bit AVC 等） | 两者都检查，失败释放并返回 `nullptr` |
| 同上 | `avcodec_find_decoder` / `avcodec_alloc_context3` / `avcodec_open2` 只用 `ASSERT` | release 下会拿空 codec 进 `avcodec_open2`；改为返回错误码 |
| 同上 | `CopyNV12Data` 调用传入 `fb_size`（自证式上界） | 传 `dest_capacity`（guest 缓冲 / slot / fallback 的真实大小） |
| 同上 | `SceVdecCoreHwPictureInfo` 写在 `allocated_fb_size - sizeof(meta)`，容量刚好时覆盖画面末尾几行 | 条件改为 `dest_capacity >= fb_size + sizeof(meta)` |

无法产出或装不下的帧现在按"解码错误"丢弃并记日志——这正是 guest 对损坏 AU 已有的处理路径，
不会再整进程崩掉。

---

## 五、验证步骤

1. **构建**：与上游一致（CMake + Vulkan SDK + FFmpeg），产物为 `divaps4.exe`。
2. **亮度**：先确认 `readbackLinearImages = true` 时主标题、菜单和 PV 亮度正常；若 `false` 仍暗，
   在 `[GPU]` 设置中保持开启，等待按需回读修复通过实测。
3. **回归**：回读现在只在 guest-visible fence 处处理，不再在每次 PM4 提交时处理；通过
   `[GPU] traceGammaFormats = true` 可在 `log/shad_log.txt` 查看格式钳制是否命中，以及每批回读的
   图像数、字节量和耗时。
4. **AVC**：播放含 PV 的曲目，重点验证有分辨率切换或 10bit 的流；日志里若出现
   `CopyNV12Data: frame ... needs ... but the frame buffer only has ...`，说明流确实超出了
   guest 声明的缓冲，应改为在 `sceVdecCoreQueryFrameBufferInfo` 侧按实际分辨率上报。

---

## 六、已知边界

- 钳制是**单向**的：内容编码而读者要原始字节时保持原样。若某个游戏确实依赖"线性内容 + sRGB
  读取 + 期望解码"的组合，画面会显得偏亮而非偏暗，此时需要用 `force_degamma` 描述符，
  而不是放开钳制。
- `srgb_encoded` 是图像级状态，同一图像在 sRGB / 线性两种 CB 之间来回切换时会跟随最近一次
  写入。这对 DIVA X 足够；若后续发现某作在同一帧内交替写入两种编码，需要把该标志下放到
  subresource 粒度。
- 本次改动无法在本机做端到端验证（需要 PS4 固件与游戏本体）。逻辑依据是代码路径与 Vulkan
  格式别名规则，属于"可静态论证、需实机确认"的修复；若某场景仍偏暗，下一步应在
  `FindTexture()` 里打印 `(image.srgb_encoded, view 请求格式, 实际 view 格式, T# nfmt)`，
  即可定位到具体表面。

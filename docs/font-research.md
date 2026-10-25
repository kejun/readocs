# 终端中文字体与 Emoji 调研

日期：2026-10-25。对象为当前 PaperDIY / ESP32-S3、540×960 墨水屏、16 MiB Flash、8 MiB PSRAM，驱动固定为 M5GFX `02107b8`。下文保留替换前的源码核对、官方资料调研和主机转换测量。

同日已完成 0.2.5：Noto Sans SC 与独立黑白 Emoji 使用原生 16 / 24 / 32 像素资源，随应用内嵌，已烧录并验证局刷、分页和真实轻睡。最终覆盖、授权及可复现构建见[字库说明](../firmware/device/fonts/README.md)，实机记录见[刷新策略](device-refresh.md#025-字体更新验证2026-10-25)。最终采用应用内嵌以保证代码与资源一同更新，无需写入预留字体分区；下文第 2 条是调研阶段的候选方案。

收到 0.2.5 中文字形偏细的反馈后，0.2.6 改为 Noto Sans SC Medium（500 字重），继续使用原生 1 位字形与同一刷新策略。[Regular / Medium / Bold 的三档字号对照](images/font-weight-native-2026-10-25.png)来自固定源的实际转换器输出，属于主机预览。逐字去除空白边缘后，三档字号共 126 个中英数字样本的实际 BFF 解码位图与源转换器位图完全一致；未发现这些样本存在笔画解码损坏。同日用户针对终端 24 号预览页确认“已明显改善，粗细合适”，因此保持 Medium 500 为默认字重。

## 结论

优先试用简体思源黑体对应的 **Noto Sans SC Regular**，为 16、24、32 像素分别生成原生字形，快速局刷使用原生 1 位字库。它适合界面、文章标题及中英混排，字形风格较统一。正文偏好楷体时，可试用 **霞鹜文楷 GB 屏幕阅读版**；偏好印刷书页风格时，可试用 **思源宋体对应的 Noto Serif SC**。这些可读性判断属于本次主机字形对照后的建议，最终粗细和残影需要在屏幕上比较。

Emoji 使用独立的 **Noto Emoji 黑白字形**及序列映射。更换中文字体不能同时补齐所有 Emoji。先保留现有快速局刷、电池轻睡和 P2 清理策略，再比较新字形；不以改成每次整屏灰阶刷新来换取字体效果。

## 替换前的 0.2.4 显示问题

- [reader.cpp](../firmware/device/main/reader.cpp) 的 `font()` 实际选择 `efontCN_16`、`efontCN_24` 和标题用的 `efontCN_24_b`，是中文点阵字库。32 号正文使用 24 号字形放大到 4/3 倍，没有原生 32 像素字形。非整数倍放大会造成笔画粗细和间距不均。
- 编译链接图中，这三套字体占 1,433,229 字节，约 1.37 MiB。其点阵及字形设计与现代屏幕中文字体不同，显示效果不应归因于误用了英文字体。
- 固定版本的 M5GFX 能解码四字节 UTF-8，并提供 `setEmojiCallback()`，但业务固件没有注册该回调或提供 Emoji 字库。普通 `IFont` 的度量/绘制接口仍采用 16 位码点；正文排版对超出 `U+FFFF` 的字符使用缺字度量。
- 排版会将部分组合字符和 ZWJ 聚为一组以避免中途换行，但仍按单码点求宽、绘制；这不等于能合成 Emoji。国旗、肤色和职业等组合需要进一步识别完整序列。

接口依据：[固定版本字体接口](https://github.com/m5stack/M5GFX/blob/02107b828acb3eb782fccd747863638d64c876d7/src/lgfx/v1/lgfx_fonts.hpp)、[UTF-8 与 Emoji 回调](https://github.com/m5stack/M5GFX/blob/02107b828acb3eb782fccd747863638d64c876d7/src/lgfx/v1/LGFXBase.cpp)。

## 候选字体

| 候选 | 适用方向 | 在当前屏幕上需要检查的地方 |
|---|---|---|
| Noto Sans SC / 简体思源黑体风格 | 首选；界面、标题、技术文章和中英混排 | 原生 24 / 32 像素下的笔画粗细；小字可对比 Regular 与 Medium |
| Noto Serif SC / 简体思源宋体风格 | 长篇正文、偏印刷书页的风格 | 24 像素横细竖粗的笔画能否稳定显示，优先比较 32 像素 |
| 霞鹜文楷 GB 屏幕阅读版 | 偏自然手写风格的正文 | 楷体笔形属于阅读偏好；需比较快速波形下的细笔画 |

Noto CJK 提供简体地区版本，应明确选用 SC，避免直接选日文字形版本。[Noto CJK 官方说明](https://github.com/notofonts/noto-cjk/blob/main/README.md)、[Sans 下载说明](https://github.com/notofonts/noto-cjk/blob/main/Sans/README.md)、[Serif 下载说明](https://github.com/notofonts/noto-cjk/blob/main/Serif/README.md)。

霞鹜文楷屏幕阅读版将原版较粗的字重作为常规字重，调整了屏幕使用的度量；本次取得 v1.530 的 `LXGWWenKaiGBScreen.ttf`，GB 版以陆标字形为主。其官方定位是 PC / Android 屏幕阅读，不能直接当作本墨水屏的实机验收。[作者说明](https://github.com/lxgw/LxgwWenKai-Screen)、[v1.530 发布说明](https://github.com/lxgw/LxgwWenKai-Screen/releases/tag/v1.530)。

以上字体源均附 OFL 1.1 授权说明；正式分发转换字库时须随资源保留对应授权和来源。

## 字形对照

下面的原生 1 位候选字形直接来自本次转换器输出，当前字体从固定版本 efont 点阵解码，均以 32 像素比较。图为主机预览，没有模拟屏幕反射率、物理残影或温度。

![快速局刷的原生字形对照](images/font-fast-native-2026-10-25.png)

完整对照包括 16 / 24 / 32 像素、16 灰阶模拟、灰阶经过 fast Bayer 处理后的结果及黑白 Emoji 示例。矢量字体部分由 Pillow / FreeType 栅格化，字体转换器的最终像素可能不同。

![完整字体与灰阶对照](images/font-comparison-2026-10-25.png)

## 为什么快速局刷要单独选字形

当前 `Panel_EPD` 在 fast 模式将灰阶按 4×4 Bayer 阈值转换为黑白，文字边缘的浅灰像素会形成点状纹理。仅把字体替换为抗锯齿灰阶字体，可能出现整刷看起来平滑、局刷却碎点化的现象。上图显示了这一软件像素差异，不能据此推断实机残影。

首轮建议使用针对目标像素大小直接生成的 1 位字形，保留当前 `epd_fast` 矩形路径；若需要灰阶字体，再单独比较 2 / 4 位资源、粗细与波形。屏幕没有 RGB 子像素布局，不使用 LCD 子像素抗锯齿。刷新验收遵守[终端按键与刷新策略](device-refresh.md)。

## 存储可行性：实际转换数据

[分区表](../firmware/device/partitions.csv) 已预留 2 MiB `fonts` 分区，目前业务代码没有从该分区加载字体。每个应用槽为 4 MiB；当前 0.2.4 应用约 2.89 MiB。完整字体文件不宜直接打包进当前字体区，更不能将它们全部展开到 RAM。

本次下载的单个源文件分别约为：Noto Sans SC 7.95 MiB、Noto Serif SC 11.09 MiB、霞鹜文楷 GB 屏幕版 25.23 MiB。以下是离线转换后的实际二进制大小，不是用 TTF 文件大小估算：

| 单一字体、16 + 24 + 32 三套原生字形 | 1 位压缩 BFF | 4 位压缩 BFF |
|---|---:|---:|
| Noto Sans SC Regular | 1.52 MiB | 3.95 MiB |
| Noto Serif SC Regular | 1.57 MiB | 4.05 MiB |
| 霞鹜文楷 GB 屏幕版 | 1.51 MiB | 4.55 MiB |

Noto Sans SC 的 2 位版本合计约 2.30 MiB，也超出单独的 2 MiB 字体区。三种字体各自的 1 位版本均能容纳；这不代表能把三种完整字体同时放入该区。

测量条件：`lv_font_conv@1.5.3`，BFF 二进制、默认 RLE/XOR 压缩、默认轻度 hinting、关闭 kerning、不使用 LCD 子像素。每套要求同一个 7,543 字符集合：GB2312 全集、ASCII 和额外的项目符号/省略号/分隔线；其中包含 6,763 个 GB2312 汉字。原始文件和转换产物 SHA-256、逐字号大小见[测量数据](font-research-data.json)。该集合用于公平比较，不能称为完整中文覆盖，也不包含 Emoji、额外字重或全部生僻字。

M5GFX 固定版本已有 BFF 加载与绘制实现及 `ft_lvgl` 示例，不必因此引入整套 LVGL。当前名为 `ft_ttf` 的运行时分支实际使用 BFF 解析器，不能将其当作通用 TTF / OTF 栅格器。转换工具及格式说明见 [lv_font_conv 官方仓库](https://github.com/lvgl/lv_font_conv)；M5GFX 入口见[固定版本示例](https://github.com/m5stack/M5GFX/blob/02107b828acb3eb782fccd747863638d64c876d7/examples/Basic/LvglFont/LvglFont.ino)。本次未验证新包在终端上的加载、解码速度或峰值内存。

## Emoji 方案

优先使用 [Noto Emoji 官方黑白字体](https://github.com/googlefonts/noto-emoji#monochrome-font)。本次取得的变量字体约 1.89 MiB，但不建议直接在终端运行完整变量字体，而应离线生成适合 16 / 24 / 32 像素的黑白字形及映射。

先覆盖常用单字符和符号，如 😀、👍、🔥、📚、✅、⚠️、❤️；再按完整序列处理 👩‍💻、带肤色人物和国旗，不能将每个码点都画成一个占位格。Unicode 对修饰符、地区指示符和 ZWJ 序列有明确结构定义，见 [Unicode Emoji 标准](https://www.unicode.org/reports/tr51/)。现有单码点回调可以作为部分入口，但完整序列还需业务层统一解析。

字形绘制与排版宽度必须使用同一份映射；缺失字形要给出稳定可见的回退，不能静默丢字符。正式字符集还应检查扩展汉字，非 BMP 汉字同样不能依赖现有 16 位普通字体接口自动解决。

## 建议的下一步

1. 首轮只接入一种主字体，优先 Noto Sans SC，原生 16 / 24 / 32 像素，增加有边界的 Emoji 黑白子集。最终字集和资源总量需另行确定。
2. 使用已有字体区，缓存字体索引与有限数量的字形，避免每行重新加载字体、每画一个字都从 SPIFFS 打开/寻址读取；继续复用排版的字形度量缓存。资源索引、缓存内存及额外字重计入预算。
3. 字体身份和资源版本加入分页/显示基准失效条件，使用已有块 ID 与偏移恢复阅读位置；修改字体后重新分页，不能复用旧页码或旧帧。
4. 先做中英文、中文标点、大小字号、混合 Emoji 和缺字回归，再对比实机 fast / text / quality、连续局刷清理、粗细、残影、布局耗时及电池按键体验。保持已验证的刷新和轻睡策略。

调研阶段仅新增此文档、主机预览和测量数据；后续已在同日完成 0.2.5 字体替换，见页首链接。

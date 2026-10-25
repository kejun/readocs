# Noto Sans SC 与黑白 Emoji 资源

这些资源随 0.2.6 应用一起内嵌，使用现有 4 MiB 应用槽更新；`fonts` 预留分区、分区表、设置和正文缓存不需要写入。移除旧 efont 后，新资源共 2,160,757 字节，当前完整应用仍能放入原应用槽。资源与渲染代码保持同一应用版本，回退应用也会一起回退字库。

- 中文 / Latin：Noto Sans SC Medium（500 字重），GB2312 全集、ASCII 与额外界面符号，共 7,543 个字符，原生 16 / 24 / 32 像素、1 位压缩 BFF。0.2.5 的 Regular 收到偏细反馈后，比较原生 Regular / Medium / Bold 样张，改用中等字重；正文不是在运行时重复描边。
- 标题：使用同字号原生字形的一像素加粗，度量与实际绘制同步增加一像素；Emoji 不重复加粗。
- Emoji：Noto Emoji 黑白版，主机用 HarfBuzz 识别 Unicode 17.0 Emoji 测试集，去除不显示的变体选择符后，只收录实际合成为单字形的序列。共 5,051 个规范/简写序列，映射到 1,732 种字形，分别生成 16 / 24 / 32 像素资源。
- 内部私用区编号仅用于显示，不修改快照正文、哈希或阅读锚点。肤色、性别等变体遵循源黑白字体的设计，部分变体共用字形。
- 该字体仍有 155 个完全限定序列未覆盖；未知 Emoji、未包含的扩展汉字或无效字符显示可见方框。不是全部 Unicode 字符覆盖。

`manifest.json` 记录全部资源大小、SHA-256、源文件及工具版本；`revision.hpp` 与其绑定。原字体版权说明见 `NOTICE.txt`，OFL 与 Unicode 数据授权位于本目录。

## 重建

已提交可直接构建的资源，普通 ESP-IDF 构建不需要字体工具或联网。重建资源时建立独立 Python 虚拟环境，安装 `requirements.txt`，并在独立目录安装 `lv_font_conv@1.5.3`。然后从仓库根目录运行：

```sh
python firmware/device/fonts/build_fonts.py \
  --source-dir artifacts/font-sources \
  --converter artifacts/font-tools/node_modules/lv_font_conv \
  --fetch
```

脚本只按 `sources.lock.json` 中的固定提交 / Unicode 版本下载，校验 SHA-256；已有源文件时可省略 `--fetch` 离线重建。Node.js 路径可通过 `--node` 指定，`--output` 可输出到独立目录比对，避免先覆盖发布资源。工具版本不匹配会停止。当前已验证独立重建的七个二进制资源逐字节一致。

## 渲染与验证

`ReaderFont` 共用同一份中文字库、Emoji 字库和可见缺字回退提供测宽及绘制。`text_clusters.hpp` 统一识别 ZWJ、肤色、国旗、键帽及组合字符，列表裁剪和正文换行使用完整组的边界；内部显示串经过映射，原始 UTF-8 和按码点计数的阅读偏移保持不变。

字库二进制直接映射自应用 Flash，字体对象与索引在交互期间保留，分页沿用有界字形度量缓存；没有每字 SPIFFS 打开/读取。字号不再使用 24→32 放大。字体修订号加入显示基准及阅读位置，字体变更后按原文块和偏移重新分页，不复用旧字体的页码。

运行 `tests/test_fonts.py`、`tests/test_reader_layout.py` 和 `tests/test_reader_refresh.py`：验证实际 M5GFX BFF 解码、测宽与实际绘制步进、黑白像素、粗体、缺字回退、组合 Emoji 边界、完整正文分页和原有局刷策略。另运行输入、轻睡与睡眠回归，遵守[刷新维护基线](../../../docs/device-refresh.md)。

菜单「字号」有中文、英数和 Emoji 样张，可用 A 切换三种原生字号，B 应用。

<p align="center">
  <img src="logo.svg" alt="Readocs logo" width="112" />
</p>

# Readocs

把飞书文档和 RSS / Atom 同步到 M5Stack PaperDIY，离线翻阅。

当前提供本地代理、管理页和 ESP32-S3 开发固件。需求基线见 [docs/spec.md](docs/spec.md)，实现和验证进度见 [docs/implementation.md](docs/implementation.md)。

## 启动管理页

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -e '.[dev,device]'
.venv/bin/readocs
```

打开 <http://127.0.0.1:8765>。另开终端运行 `.venv/bin/readocs --show-key`，用本机生成的管理密钥登录。密钥保存在 `.readocs/admin.key`，不会写入代码。

在「飞书应用」填写 App ID 和 App Secret，先测试凭据或直接保存。按「如何授权」开通只读权限、登记回调 URL，再点击「授权飞书账号」。系统递归同步授权账号云盘和知识库中的全部可访问 docx，无需逐篇填写链接。应用凭据验证成功后才替换已有配置，目录和正文权限由后台单独验证。可在「内容」添加最多两个公开 RSS 2.0 或 Atom 1.0 地址。

RSS / Atom 源最大 16 MiB，每篇正文最多 64 KiB；超长文章保留段落前缀并明确标注截断，可通过原文链接阅读全文。

代理服务支持电脑已配置的 HTTP 出站代理（`HTTP_PROXY` / `HTTPS_PROXY`，包括小写变量和系统代理设置），遵守 `NO_PROXY`。HTTPS 目标仍校验证书；代理连接固定到经过公网地址校验的目标 IP。修改代理设置后需重启服务。

飞书目录和已同步正文每 3 小时自动检查一次，首次尚未下载的正文继续补齐；需要及时获取变化时可手动刷新。临时超时会自动重试，持续失败从 30 秒开始退避，手动刷新优先恢复失败文档；旧正文始终保留。具体规则和排查方法见[飞书同步恢复](docs/feishu-sync-recovery.md)。

终端「菜单 → 检查更新」或来源页长按 A 会先触发全部飞书、RSS / Atom 来源更新，再自动接续下载；来源页底部显示进度与结果。过程与维护约束见[检查更新链路](docs/manual-sync.md)。

网页和设备的文章列表均按日期倒序。飞书优先使用标题中明确的年月日，其次使用文档创建时间、更新时间；RSS / Atom 使用上游提供的文章时间。没有有效日期的文章排在最后，同日顺序保持稳定，抓取或检查时间不影响排序。

App Secret 通过本机密钥加密后保存在 SQLite；只返回“已设置”，不回读 Secret 或飞书 token。正常使用不需要手填或更新飞书 token。用户 OAuth 和 refresh_token 也只保存在代理。仅凭应用凭据只能枚举应用身份的目录；个人文档需要账号授权。单独分享且未归入可枚举目录的对象受飞书 API 限制，表格与附件暂不转换。`.readocs` 含私有内容与密钥，请完整备份到受控位置，不要提交到仓库。此开发版本未提供整机物理读取保护。

## 让设备连接代理

设备与代理需在可互访网络中。本机 HTTP 模式只监听回环地址；局域网使用 HTTPS：

```sh
.venv/bin/readocs --host 0.0.0.0 --https
```

开发时可同时保留本机管理页和设备 HTTPS 端口：

```sh
.venv/bin/readocs --device-port 8766
```

此模式的管理页仍是 `http://127.0.0.1:8765`，设备代理为 `https://电脑的局域网IP:8766`；同一进程共享后台任务和数据，不需要浏览器访问开发证书页面。

首次生成的开发证书覆盖本机当前局域网 IP、localhost 和回环地址。实际域名可以通过 `--name your-hostname` 在首次生成时加入。已有证书不自动替换；换代理地址前核对证书 SAN，设备将正常校验证书、主机名与时间。部署可使用正式域名与受信任证书。

管理页「设备」生成五分钟、一次性配对码。设备先完成 Wi-Fi 配网，再通过 USB 传入代理地址、配对码与本地开发证书：

```sh
.venv/bin/python scripts/device.py bind --server https://电脑的局域网IP:8766 --ca .readocs/tls.crt
.venv/bin/python scripts/device.py check
.venv/bin/python scripts/device.py status
```

配对码从终端交互输入。设备仅保存独立设备凭据，撤销设备不会更换飞书凭据。

## 通过 Cloudflare Tunnel 连接

设备与主机可以使用不同网络。主机启动设备 HTTPS 服务：

```sh
.venv/bin/readocs --device-port 8766
```

本机使用 Cloudflare Tunnel 和设备专用域名。已发布应用路由如下（示例中省略具体隧道 ID 和主机名，请替换为你的值）：

| 设置 | 值 |
|---|---|
| 主机名 | `your-device-host.example` |
| 路径 | `^/v1/.*$` |
| 服务 URL | `https://127.0.0.1:8766` |
| Origin Server Name | `localhost` |
| CA Pool | 主机上 `.readocs/tls.crt` 的绝对路径 |

保留源站 TLS 验证。若隧道由 Cloudflare 控制台管理，路由应在控制台修改；本地 `config.yml` 会被云端配置覆盖。管理页继续在本机 8765 端口使用，公网路由只匹配设备协议。

路由生效后，在本机管理页生成配对码，再用公网地址重新绑定设备：

```sh
.venv/bin/python scripts/device.py bind --server https://your-device-host.example
.venv/bin/python scripts/device.py check
```

公网地址使用固件内置的受信任根证书，不传 `--ca`。配对成功后设备会保存新地址，Wi-Fi、缓存和阅读位置保留。主机上的代理和 cloudflared 都需持续运行。

如需复用其他应用的域名，可启动时增加 `--device-prefix /readocs`，只发布 `^/readocs/v1/.*$`，并让设备绑定 `https://你的域名/readocs`。该路由需放在同域名通配路由之前。

## 设备配网

首次没有网络时开启带随机密码的配网热点。也可在设备菜单选择「配网」，或同时按住 A、B 三秒，释放后按 B 确认。手机连接屏幕上的热点，打开 `http://192.168.4.1`，扫描或手动填写 2.4 GHz Wi-Fi。

最多保存五组网络。候选网络通过关联和 DHCP 后才保存；失败保留旧密码与缓存。第六组需要明确选择替换对象。0.2.8 修复双栈连接被误拒绝的问题，页面可显示连接、认证、DHCP 和保存结果，并允许失败后重试；连接并保存成功后等待网页确认，或最多留出 20 秒，再关闭热点。五分钟无操作超时关闭，后台状态轮询不延长时限。换 Wi-Fi 不清除代理绑定、内容或阅读位置。实现及验收边界见[配网改进记录](docs/wifi-provisioning-improvements.md)。

USB 恢复入口：

```sh
.venv/bin/python scripts/device.py setup
.venv/bin/python scripts/device.py setup-cancel
.venv/bin/python scripts/device.py wifi --ssid 网络名称
```

密码在终端隐藏输入，不使用命令行参数。工具自动识别唯一的 PaperDIY USB 串口，有多个设备时通过 `--port` 指定；为等待前面的 SD 同步任务，命令总等待上限为 180 秒。USB 配置工具需要独占串口；运行前关闭串口监视器，完成后可重新打开。

## 编译固件

硬件固定为 PaperDIY / ESP32-S3、16 MiB Flash、8 MiB Octal PSRAM，使用硬件诊断验证过的 M5Unified / M5GFX 版本。

0.2.6 使用 Noto Sans SC Medium（500 字重）与独立 Noto Emoji 黑白字库，提供原生 16 / 24 / 32 像素字号，保留快速局刷与电池轻睡。资源、覆盖范围和可复现构建见[字库说明](firmware/device/fonts/README.md)，选型依据见[字体调研](docs/font-research.md)。菜单「字号」可预览中文和 Emoji。

0.2.7 支持 SD 扩展缓存：启动后识别可写卡，同步时检查实际剩余空间，空间允许时下载全部已就绪正文，按批次保存进度并自动接续。未装卡或挂载失败时保留片内缓存策略，不会自动格式化卡。容量、回退和校验边界见[SD 存储与同步说明](docs/device-storage.md)。

```sh
python3 firmware/device/fetch_components.py
# 激活 ESP-IDF 5.5.5 环境后：
cd firmware/device
idf.py build
```

`firmware/device` 是业务固件；`firmware/diagnostics` 保留硬件诊断。业务分区包含两个 4 MiB 应用槽、4 MiB 正文缓存和独立设置 NVS；与原厂/诊断布局不同。迁移前必须取得完整 16 MiB Flash 备份、校验备份、记录恢复命令。仅有此前的前 2 MiB 备份不足以覆盖新布局的所有写入。不要直接覆盖原厂分区后再尝试补备份。

开发固件始终校验 HTTPS，不会自动擦除原厂 NVS。仅对确认全擦除的新缓存分区做首次格式化；已存在但损坏的缓存保留供恢复。签名 OTA、加密发布流程和实测低功耗属于后续发布阶段，不在普通构建中烧写 eFuse。

根目录 `cover.jpg` 在构建时嵌入固件。菜单「关机」会先显示这张全屏封面，再通过 M5PM1 断电；替换封面后重新编译即可，不需要文件系统或 SD 卡。运行中固件读取 M5PM1 的按钮状态识别电源键双击，先显示封面再断电；深睡/关机前会恢复 PM1 的硬件按键配置，因此在深睡状态下双击仍由 PM1 直接断电并保留当前画面。运行中单击电源键不再触发 PM1 复位，避免双击的第一次单击先把设备重启。

开机显示来源列表，先选择「飞书文档」或某个 RSS / Atom 订阅，再查看该来源的文章；各来源分别保留文章选择位置。来源列表中 A 下一项、A+B 短按上一项、B 打开、按住 A 检查更新、按住 B 打开菜单。文章列表每屏 8 篇，A 下一项、A+B 短按上一项、B 打开、按住 A 翻到下一屏、按住 B 返回来源列表。阅读中 A 下一页、B 上一页、按住 A 检查更新、按住 B 返回所属来源的文章列表；返回后仍选中刚才的文章，再次打开恢复阅读页码。A+B 持续三秒仍是配网确认。菜单中 A 移动、B 确认、按住 B 返回；字号支持 16 / 24 / 32。网络和波形操作不占用独立按键采样任务。

刷新和电池交互等待以[终端按键与刷新策略](docs/device-refresh.md)为维护基线：连续操作保留快速局刷，15 秒交互窗口内空闲时轻睡，之后深睡。长时间闲置后首次唤醒需要整页重建基准。修改相关代码前应运行该文档指定的回归。

## 检查

```sh
.venv/bin/python -m pytest -q
node --check proxy/readocs/static/app.js
```

自动测试使用飞书响应替身，不依赖真实密钥。包含令牌并发刷新、修订隔离、失败保留配置与内容、未下载正文的连续调度、分块完整性、撤销与越权、RSS 身份与清洗、DNS 连接时地址检查，以及实际固件按键状态机的主机编译与边界测试。审查回归测试还覆盖 RSS 长期保留和下载租约、配置与预览竞态、设备缓存策略、阅读位置迁移、休眠前按键及 USB 命令结果匹配。此前真实设备已通过 HTTPS 配对、5 篇正文缓存、Wi-Fi 停止后的翻页与软件重启恢复；记录见实现进度。真实飞书账号已完成 OAuth，发现 449 篇 docx，并验证了真实正文预览；其余正文由后台继续抓取。真实令牌到期刷新、权限撤销、完全断电恢复与设备功耗仍需验收。

终端 0.2.2 已通过 449 条真实飞书目录同步、按需正文下载、返回文章列表、恢复第 2 页及软件重启后的绑定与缓存保留验证。

设备目录按页获取并保存中间进度，正文按需下载。代理全量同步不套用 RSS 的 50 条目录配额；设备本地目录索引预算为 512 KiB，超限会明确提示并保留旧版。页面更新后需同步重启代理进程，避免磁盘上的新页面连接仍在运行的旧 Python 路由。

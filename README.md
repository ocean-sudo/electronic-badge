# USB 图片徽章

面向 **Waveshare ESP32-S3-Touch-AMOLED-1.75C** 的圆形图片徽章固件与 HTTPS Web Serial 控制台。支持的桌面浏览器从静态页面直接访问 USB 串口；不需要 PWA、Python 串口桥、本机原生助手或云端图片服务。

睡眠行为依供电状态处理：USB/VBUS（包括充电器）或电源状态未知时，关闭显示与 IMU 并保持 USB 控制在线；电池供电时，手动执行 SLEEP 或 USB 熄屏后拔掉 USB 转为电池供电，都会在当前操作完成、输入释放并通过 PMIC 就绪检查后调用 AXP2101 PMIC 软件关机，不进入 ESP32 深度睡眠，也不依赖 GPIO3 的 EXT0 唤醒。PMIC 关机后 USB 控制断开，按住 POWER 约 2 秒冷启动；触屏不能唤醒关机设备。电池闲置超时仍按既有策略请求 PMIC 软件关机，默认电池超时为 0（关闭自动关机）。USB/未知供电熄屏仍保持控制在线。实际耗电变化尚未实测。

> 控制台截图使用原创合成图片，不连接真实设备。它展示实际网页的离线裁切界面；截图上的水印说明其为演示素材。

![真实网页控制台的离线裁切演示，合成图片，未连接设备](docs/assets/console-desktop.webp)

[打开托管控制台](https://electronic-badge-project.github.io/electronic-badge/console/) · [查看手机尺寸控制台截图](docs/assets/console-mobile.webp) · [中文项目介绍页](https://electronic-badge-project.github.io/electronic-badge/)

## 功能

- 在浏览器中选择、拖放或粘贴 JPG、PNG、WebP 图片，调整圆形裁切框并预览。选择图片不会自动上传；浏览器将成品编码为 466×466 JPEG，`Canvas toBlob` 编码质量参数为 `0.85`，文件大小依图像内容而异。WebP 由浏览器解码后转为 JPEG，ESP32-S3 端使用 ROM 软件 TJpgDec 解码 JPEG，不支持设备端 WebP 解码。
- 浏览已保存图片并读取设备 JPEG 成品；可切图、覆盖、删除、调节亮度、设备菜单、动画和熄屏。图片 ID 为 0～2147483646，未指定 ID 时由设备分配最小可用 ID，数量取决于 LittleFS 可用容量。
- 自动切图由固件执行，关闭网页或拔掉 USB 后仍可继续。随机顺序默认关闭；开启时随机袋以每轮遍历全部有效图片。上一张按播放历史回退，下一张优先重放前进历史。RAM 中最多保存 6 项（当前及前 5 张），设备重启（包括 PMIC 关机后的冷启动）后历史清空；PMIC 关机不会保留轮播中的临时当前图片，已保存的设置仍保留，随机播放轮次重新开始。
- USB 闲置超时关闭屏幕并保持控制在线；电池闲置超时和电池供电时的手动 SLEEP 均由 AXP2101 PMIC 软件关机，不进入 ESP32 深睡。USB 熄屏后拔线转为电池供电也会请求 PMIC 关机。两种超时分开设置，0 表示关闭；默认 USB 15 秒、电池 0。POWER 长按启动时间设为 2 秒；网页与设备菜单报告电池百分比及 PMU 实际充电状态，USB 已连接不等于正在充电。
- 图片转场支持直接切换、淡变、水平推入与低成本径向涟漪；转场进行中再次切图时，旧图/起点取最后一次成功呈现的画面。motion=2/3 切图时，新图从当前自动旋转角或重力角开始显示，自动旋转时钟不中断。涟漪使用较硬的径向边缘换取较低渲染成本。
- 图片存储带完整性 CRC；上传与回读另校验传输 CRC。传输失败不会替换原图。

## 电脑端：HTTPS Web Serial 控制台

需要支持 Web Serial 的桌面 Chromium 系浏览器或 Firefox 151+、支持数据传输的 USB 线，以及操作系统授予当前用户的串口访问权限。打开：

<https://electronic-badge-project.github.io/electronic-badge/console/>

1. 连接徽章 USB 数据线，点击“连接徽章”。
2. 在浏览器设备选择器中选择徽章串口。项目没有采用未经核实的 USB VID/PID 过滤器，因此不会隐藏其他候选设备；请核对后选择。
3. 页面以 115200 baud 打开串口，读取 `STATUS` 和带 CRC 的图片目录；状态成功后才启用设备控制。
4. 断开前等待当前传输结束，再点击“断开连接”。浏览器曾授予的端口可在下次点击连接时重用；网页不会在没有用户操作时弹出权限选择器。

页面是纯静态 HTML/CSS/JavaScript。图片裁切、JPEG 编码、二进制分帧和 CRC 校验都在浏览器内完成；图片通过 Web Serial 直接传给固件，不经过项目服务器。上传遵循固件的 `READY`、每 4096 字节一条 `ACK`、最终 `OK <slot>` 流程；下载按 `DATA <length> <crc>` 宣告的精确字节数读取并验证尾部应答。

### 浏览器固件更新（仅 APP 分区）

先在本仓库源码构建：<code>pio run -e picture-badge</code>，使用 <code>.pio/build/picture-badge/firmware.bin</code>。控制台仅用于你审查过源码后自行构建的本机文件；文件选择器、ESP 镜像头 / 段长度与本地 SHA-256 不能认证来源。本页没有固定版本 Release 或可信哈希清单，不提供经过签名验证的发布固件；不要选择来源不明的 .bin。

断开普通控制连接后，按住 BOOT、按下并释放 RESET/EN，再松开 BOOT，手动进入 ESP32-S3 ROM 下载模式；在页面勾选“已进入 ROM 下载模式”和“确认覆盖 APP”后再刷写。页面使用固定版本 esptool-js 0.7.0 / Web Serial，以 no_reset 连接，只将单个应用镜像写入 0x10000，最大 0x300000 字节（应用分区结束 0x310000），并显式设置 eraseAll=false。这不是首次空白设备的初始化流程，不写 bootloader、分区表、NVS 或 LittleFS；安全启动、闪存加密或禁用 ROM 下载模式的设备不保证可用。刷写结束由用户松开 BOOT 并按 RESET/EN，然后点击页面重新连接并读取 STATUS。页面不会尝试自动切换原生 USB 到 ROM 模式。
此分区表只有一个 `factory` APP 分区，没有 OTA 备用槽或自动回滚。刷写会覆盖唯一可启动应用；刷写期间断电、USB 断连、传输错误或镜像不兼容可能使设备无法启动。恢复可能需要重新手动进入 ROM 下载模式并用正确固件重刷；不要把此功能当作安全的 OTA 更新，也不要在重要/生产设备上试验未知镜像。虽然 `eraseAll=false` 且地址范围排除 LittleFS，esptool 仍须擦除待写 APP 扇区。

### 串口权限与 origin 边界

只在信任的 HTTPS 站点授权串口。Web Serial 权限绑定 **origin（协议 + 主机名 + 端口）**，不是项目路径；同一 `*.github.io` 主机名下的其他仓库页面可能同源，因此同 origin 脚本可能枚举已授权端口。需要把徽章权限与其他 Pages 项目严格隔离时，应为控制台使用专用自定义子域。浏览器站点设置可撤销权限；操作系统仍可能另有串口用户/组权限要求，不要为此直接以 root 运行浏览器。

浏览器不支持 `navigator.serial`、页面不是安全上下文、组织策略禁用串口或系统拒绝权限时，控制台会保持禁用并显示原因。手机浏览器不是支持目标。关闭页面不会停止固件中已经启用的轮播或自动熄屏设置。

不要把私人照片、备份、访问令牌、私人域名或设备序列号贴到公共聊天、issue 或远端 AI 服务。控制台不需要 `.env`，也不会请求账户或云端凭据。

## 固件构建

需要 Git、Python、PlatformIO Core 和网络访问。`platformio.ini` 固定 pioarduino 平台 `55.03.312-1`；构建依赖不随本仓库分发的 Waveshare 官方库，固定提交为 `6d19f7e16fb9a3be219e9eed43ca9eb56c88d01c`：

```sh
python -m pip install platformio
mkdir -p vendor
git clone --filter=blob:none --sparse https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C.git vendor/waveshare
git -C vendor/waveshare sparse-checkout set examples/arduino
git -C vendor/waveshare checkout --detach 6d19f7e16fb9a3be219e9eed43ca9eb56c88d01c
pio run -e picture-badge
```

这些命令中的 git clone 适用于 vendor/waveshare 尚不存在的干净克隆。若已有该目录，应先检查并保留本地改动，不要覆盖。PlatformIO 首次构建会下载平台及工具链，固件输出为 .pio/build/picture-badge/firmware.bin。

### 全新空白设备的首次初始化（会重写引导相关分区）

**仅用于首次初始化、且确认没有需要保留数据的全新空白设备。** PlatformIO 的标准 `upload` 会写入 bootloader、分区表及应用分区；不能把它用于已有设备升级。先关闭占用串口的浏览器控制台或其他程序，再按实际串口执行。文件系统步骤还要求项目根目录的本机 `data/` 不存在：若该目录已存在，先人工检查并确认其中没有任何文件，再跳过 `mkdir data`；目录非空时停止，不删除或覆盖内容，也不运行 `uploadfs`。

```sh
pio run -e picture-badge --target upload --upload-port /dev/ttyACM0
mkdir data &&
    pio run -e picture-badge --target uploadfs --upload-port /dev/ttyACM0
```

`data/` 是电脑上的项目目录，PlatformIO 用它生成文件系统镜像；不是固件在 LittleFS 内建立的目录。这里仅将确认空的本机目录打包为初始空文件系统。`uploadfs` 会重写设备文件系统；绝不可对含有要保留图片的设备执行，也不能混入已有设备升级。

### v1 图片迁移与已有设备升级

v1 使用 RGB565 图片存储格式；v2 不会读取或自动迁移这些旧图片。**在升级前，先用仍可访问旧格式的 v1 固件和电脑端工具导出旧图，离线保存并核验备份，再转换为 v2 所需的 466×466 JPEG。** 保留备份，直到确认已在 v2 控制台重新导入并正常显示所有需要的图片。不要假设刷入 v2 后旧格式图片仍可读取。

已有设备升级只写 APP 分区。先完成以上需要的旧图片导出和转换；备份重要设置与文件，关闭控制台页面并确认浏览器已释放串口，再写入 v2 APP。项目依赖安装后可通过 python -m esptool 调用 esptool；若当前环境未安装，可先执行 python -m pip install esptool。**`0x10000` 是该板卡构建分区表中的应用偏移；写入前必须再次核实固件、板卡及串口。**

```sh
python -m esptool --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write-flash 0x10000 .pio/build/picture-badge/firmware.bin
```

APP-only 刷写不会改写 LittleFS、NVS、bootloader 或分区表。已有设备升级禁止使用 PlatformIO 整体 upload、uploadfs、整片擦除，或用旧整片镜像覆盖当前设备。固件以 `LittleFS.begin(false)` 挂载，不会自动格式化；不要以格式化来排查挂载问题。
也可用浏览器控制台替代上面的 `python -m esptool` 命令进行已有设备 APP 更新：先按上文执行 `pio run -e picture-badge`，在 HTTPS 控制台选择本机 `.pio/build/picture-badge/firmware.bin`。仅选择从已审查源码自行构建的文件；文件名、镜像结构检查和页面显示的本地 SHA-256 都不能认证来源，项目没有固定 Release 或可信哈希清单。断开普通控制连接后，按住 BOOT、按下并释放 RESET/EN、再松开 BOOT，手动进入 ESP32-S3 ROM 下载模式；勾选页面的 ROM 模式及覆盖 APP 确认后点击“仅写入 APP 固件”，如弹出串口选择器则选择徽章。完成后松开 BOOT、按 RESET/EN 启动，再在页面重新连接并读取 STATUS。
仅在启动刷写时，页面才需要联网从 `unpkg.com` 加载固定版本 `esptool-js 0.7.0` JavaScript 模块；远端代码会在页面内执行。只对你信任的页面及其外部依赖授权串口。本流程不会将固件或照片上传到 unpkg。
这仍会覆盖唯一的 `factory` APP 分区；没有 OTA 备用槽或自动回滚。刷写中断电、断连、传输失败或镜像不兼容可能使设备无法启动；恢复时重新进入 ROM 下载模式并使用确认正确的 APP 固件重刷。`eraseAll=false` 只表示不请求整片擦除；esptool 仍会擦除正在写入的 APP 扇区。刷写目标范围不含 LittleFS、NVS、bootloader 或分区表；不要将此流程用于首次空白设备初始化。

## 内存与实测边界

两张 RGB565 源帧、两张输出帧及一张 USB/菜单工作帧合计约 **2.07 MiB**，另有 **4 KiB** JPEG 解码工作区；这些数值不包含固件其他运行时内存开销。已保存的 42 张自然照片读取、CRC 校验、JPEG 解码和 RGB 转换约 228–284 ms/张；直接显示并确认约 301–356 ms/张，包含显示阶段而非上传时延。此为该测试样本的实际观测值，随图像与 I/O 变化，不是其他设备的性能保证，也不是 WebP 设备解码结果。

<a id="ai-deployment"></a>

## 推荐方式：AI 辅助部署，而非一键部署

先人工审阅 AI 为你的电脑环境生成的命令。**不要把 `.env`、私人照片或备份、token、私人域名或设备序列号提供给公共聊天、issue 或远端 AI。** 只提供 README、`platformio.ini`、系统信息和脱敏报错；任何私有值在本机填入。若本机代理需读取秘密，不得回显或提交这些值。

可复制以下提示词；不要附加本机秘密或私人素材：

```text
只检查此仓库的 README、`platformio.ini`、浏览器控制台源码，以及我提供的操作系统和脱敏 USB 错误。不要读取或索取 `.env`、照片、备份、token、私人域名或设备序列号；不要回显或提交任何本机秘密。控制台应保持纯 HTTPS 静态 Web Serial 页面；请给出逐条可审阅的固定版 vendor 依赖获取、固件构建及浏览器连接步骤，并说明每条命令的影响。
已有设备升级时，只可把本仓库构建的 firmware.bin 写入 ESP32-S3 APP 分区 `0x10000`；保留 LittleFS、NVS、bootloader 和分区表。禁止整体 upload、uploadfs、整片擦除、自动格式化和恢复旧整片备份。v1 RGB565 图片不会被 v2 读取；提示我先在 v1 下导出、核验备份、转成 466×466 JPEG 后，再进行升级和导入。
只有我明确确认是全新空白设备后，才解释首次标准 upload 及 uploadfs 文件系统初始化；初始化不能混入现有设备升级步骤。控制台必须保持纯 HTTPS 静态 Web Serial 页面，不增加 PWA、原生助手、公网隧道或串口桥。无法确认硬件、分区、串口或数据状态时先停下询问，不执行破坏性操作。
```

## GitHub Pages 与人工同步

生产 Pages <https://electronic-badge-project.github.io/electronic-badge/> 由独立组织仓库 [electronic-badge-project/electronic-badge](https://github.com/electronic-badge-project/electronic-badge) 托管。**提交本源码仓库不会自动部署生产 Pages。** 本仓库 `docs/` 是待审核、可整体人工同步的 Pages 内容；其中 `docs/console/index.html` 是控制台唯一源码和生产入口，介绍页使用相对链接 `console/`，同步后仍位于同一项目 Pages 路径。发布者必须审核后把 `docs/` 内容人工同步到独立仓库并由其部署流程发布。

`web/index.html` 仅把源码仓库中的旧入口重定向到 `docs/console/`，不复制控制台逻辑，也不是生产 Pages 所依赖的路径。源码目录：`src/`（固件）、`docs/console/`（Web Serial 控制台）、`docs/`（可同步 Pages 内容）及 `tests/`（原生逻辑测试）。英文补充介绍：[readme_en.md](readme_en.md)。
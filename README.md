# USB 图片徽章

面向 **Waveshare ESP32-S3-Touch-AMOLED-1.75C** 的圆形图片徽章固件与本地网页控制台。电脑端 Python 服务通过 USB 串口向设备发送控制命令；GitHub Pages 仅展示静态项目介绍，图片处理、串口桥接和设备控制都必须在连接设备的电脑本机运行。

设备端“熄屏”只关闭显示，不代表 ESP32 深度睡眠、设备断电或保证降低全部功耗。USB 控制和实际充电状态不以熄屏状态推断。

> 控制台截图使用原创合成图片，不连接真实设备。它展示实际网页的离线裁切界面；截图上的水印说明其为演示素材。

![真实网页控制台的离线裁切演示，合成图片，未连接设备](docs/assets/console-desktop.webp)

[查看手机尺寸控制台截图](docs/assets/console-mobile.webp) · [中文项目介绍页](https://electronic-badge-project.github.io/electronic-badge/)

## 功能

- 在浏览器中选择、拖放或粘贴 JPG、PNG、WebP 图片，调整圆形裁切框并预览。选择图片不会自动上传；浏览器将成品编码为 466×466 JPEG，`Canvas toBlob` 编码质量参数为 `0.85`，文件大小依图像内容而异。WebP 由浏览器解码后转为 JPEG，ESP32-S3 端使用 ROM 软件 TJpgDec 解码 JPEG，不支持设备端 WebP 解码。
- 浏览已保存图片并读取设备 JPEG 成品；可切图、覆盖、删除、调节亮度、设备菜单、动画和熄屏。图片 ID 为 0～2147483646，未指定 ID 时由设备分配最小可用 ID，数量取决于 LittleFS 可用容量。
- 自动切图由固件执行，电脑服务停止后仍可继续。随机顺序默认关闭；开启时随机袋以每轮遍历全部有效图片。上一张按播放历史回退，下一张优先重放前进历史。RAM 中最多保存 6 项（当前及前 5 张），重启后清空。
- USB 与电池分别设置闲置熄屏时间。网页与设备菜单报告电池百分比及 PMU 实际充电状态；USB 已连接不等于正在充电。
- 图片存储带完整性 CRC；上传与回读另校验传输 CRC。传输失败不会替换原图。

## 电脑端：安装与运行

需要 Python 3.10+、支持数据传输的 USB 线及当前用户的串口访问权限。Linux/macOS 在项目根目录执行：

```sh
python -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt
cp .env.example .env
python tools/serve.py
```

Windows PowerShell：

```powershell
py -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
Copy-Item .env.example .env
python tools/serve.py
```

打开 <http://127.0.0.1:8765/>。未指定串口时服务按 Espressif USB VID/PID 查找；检测到多个候选时需明确选择，例如：

```sh
python tools/serve.py --serial /dev/ttyACM0
python tools/serve.py --help
```

串口路径只是示例，使用本机实际端口。Linux 串口权限应按系统策略授予当前用户，不要为了运行控制台直接使用 root。关闭终端或按 Ctrl+C 停止本机服务，不会停止固件中的自动切图。

### 本机配置与优先级

服务从脚本所在项目目录读取本机 `.env`；不要提交它。可配置 BADGE_HOSTS（监听 IPv4 地址）、BADGE_ALLOW_HOSTS（额外 Host 名单）、BADGE_PORT 和 BADGE_SERIAL。未配置时默认监听 127.0.0.1、使用端口 8765 并自动查找串口。优先级为 **命令行选项 > 进程环境变量 > `.env` > 默认值**；各选项独立覆盖。重复指定 --host 或 --allow-host 会整体替换对应环境变量列表，而不是追加。

### 网络安全

服务没有登录认证；任何能访问服务的人都可控制设备。建议保留默认回环监听。只有理解风险且网络可信时，才在本机 `.env` 配置 LAN/Tailscale 监听及白名单；Host 白名单不配置 DNS，也不新增监听地址。LAN 上 HTTP 为明文；不要公网端口转发，也不要使用 Tailscale Funnel。Tailscale 隧道不能替代本机服务的认证。

不要将 `.env`、私人照片或备份、访问令牌、私人域名及设备序列号贴到公共聊天、issue 或远端 AI 服务。给 AI 只提供 `.env.example` 和脱敏报错；私有值只在本机填写。如果本机代理需要读取秘密，不得回显或提交。

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

**仅用于首次初始化、且确认没有需要保留数据的全新空白设备。** PlatformIO 的标准 `upload` 会写入 bootloader、分区表及应用分区；不能把它用于已有设备升级。停止任何占用串口的电脑服务后，按实际串口执行。文件系统步骤还要求项目根目录的本机 `data/` 不存在：若该目录已存在，先人工检查并确认其中没有任何文件，再跳过 `mkdir data`；目录非空时停止，不删除或覆盖内容，也不运行 `uploadfs`。

```sh
pio run -e picture-badge --target upload --upload-port /dev/ttyACM0
mkdir data &&
    pio run -e picture-badge --target uploadfs --upload-port /dev/ttyACM0
```

`data/` 是电脑上的项目目录，PlatformIO 用它生成文件系统镜像；不是固件在 LittleFS 内建立的目录。这里仅将确认空的本机目录打包为初始空文件系统。`uploadfs` 会重写设备文件系统；绝不可对含有要保留图片的设备执行，也不能混入已有设备升级。

### v1 图片迁移与已有设备升级

v1 使用 RGB565 图片存储格式；v2 不会读取或自动迁移这些旧图片。**在升级前，先用仍可访问旧格式的 v1 固件和电脑端工具导出旧图，离线保存并核验备份，再转换为 v2 所需的 466×466 JPEG。** 保留备份，直到确认已在 v2 控制台重新导入并正常显示所有需要的图片。不要假设刷入 v2 后旧格式图片仍可读取。

已有设备升级只写 APP 分区。先完成以上需要的旧图片导出和转换；备份重要设置与文件，关闭电脑端服务，再写入 v2 APP。项目依赖安装后可通过 python -m esptool 调用 esptool；若当前环境未安装，可先执行 python -m pip install esptool。**`0x10000` 是该板卡构建分区表中的应用偏移；写入前必须再次核实固件、板卡及串口。**

```sh
python -m esptool --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write-flash 0x10000 .pio/build/picture-badge/firmware.bin
```

APP-only 刷写不会改写 LittleFS、NVS、bootloader 或分区表。已有设备升级禁止使用 PlatformIO 整体 upload、uploadfs、整片擦除，或用旧整片镜像覆盖当前设备。固件以 `LittleFS.begin(false)` 挂载，不会自动格式化；不要以格式化来排查挂载问题。

## 内存与实测边界

两张 RGB565 源帧、两张输出帧及一张 USB/菜单工作帧合计约 **2.07 MiB**，另有 **4 KiB** JPEG 解码工作区；这些数值不包含固件其他运行时内存开销。已保存的 42 张自然照片读取、CRC 校验、JPEG 解码和 RGB 转换约 228–284 ms/张；直接显示并确认约 301–356 ms/张，包含显示阶段而非上传时延。此为该测试样本的实际观测值，随图像与 I/O 变化，不是其他设备的性能保证，也不是 WebP 设备解码结果。

<a id="ai-deployment"></a>

## 推荐方式：AI 辅助部署，而非一键部署

先人工审阅 AI 为你的电脑环境生成的命令。**不要把 `.env`、私人照片或备份、token、私人域名或设备序列号提供给公共聊天、issue 或远端 AI。** 只提供 `.env.example`、系统信息和脱敏报错；任何私有值在本机填入。若本机代理需读取秘密，不得回显或提交这些值。

可复制以下提示词；不要附加本机秘密或私人素材：

```text
只检查此仓库的 README、`platformio.ini`、`.env.example`、requirements.txt，以及我提供的操作系统和脱敏 USB 错误。不要读取或索取 `.env`、照片、备份、token、私人域名或设备序列号；不要回显或提交任何本机秘密。请给出逐条可审阅的虚拟环境、固定版 vendor 依赖获取、构建及本机运行步骤，并说明每条命令的影响。
已有设备升级时，只可把本仓库构建的 firmware.bin 写入 ESP32-S3 APP 分区 `0x10000`；保留 LittleFS、NVS、bootloader 和分区表。禁止整体 upload、uploadfs、整片擦除、自动格式化和恢复旧整片备份。v1 RGB565 图片不会被 v2 读取；提示我先在 v1 下导出、核验备份、转成 466×466 JPEG 后，再进行升级和导入。
只有我明确确认是全新空白设备后，才解释首次标准 upload 及 uploadfs 文件系统初始化；初始化不能混入现有设备升级步骤。默认只监听 127.0.0.1。不要创建公网隧道、端口转发或无认证公开服务。无法确认硬件、分区、串口或数据状态时先停下询问，不执行破坏性操作。
```

## GitHub Pages

GitHub Pages 展示页 <https://electronic-badge-project.github.io/electronic-badge/> 由独立组织仓库 [electronic-badge-project/electronic-badge](https://github.com/electronic-badge-project/electronic-badge) 托管，仅提供**静态项目介绍**，不是在线 USB 控制台，也不会远程访问 USB 设备。图片处理、串口桥接及设备控制必须在用户本机运行。本源码仓库的 docs/ 是介绍页的规范素材来源；经审核的内容需手动同步到组织展示仓库，不会由本仓库自动发布。介绍页使用相对资源路径，以便在项目 Pages 路径下正常工作。

源码目录：src/（固件）、web/（本机控制台）、tools/（USB 服务）、tests/（原生逻辑测试）及 docs/（静态项目介绍素材）。英文补充介绍：[readme_en.md](readme_en.md)。
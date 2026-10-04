# USB 图片徽章

面向 **Waveshare ESP32-S3-Touch-AMOLED-1.75C** 的图片徽章固件和浏览器控制台。设备通过 USB 串口连接电脑；Python 服务提供网页与 HTTP 接口，固件独立负责显示、轮播、动画和自动熄屏。这里不包含个人网络配置、照片、设备备份或厂商源码副本。

## 功能

- 选择、拖放或粘贴 JPG/PNG/WebP 图片；拖动正方形裁剪框及四角缩放，支持触摸、方向键微调和 Shift 加速。圆形预览与上传使用同一裁剪区域，透明区域为黑色。选择图片不会自动上传。
- 上传 466×466 RGB565 图片（每张 434312 字节），最多 50 张，槽位为 0～49；支持覆盖、删除、切图、亮度、熄屏/唤醒，以及图片和菜单帧回读。传输失败不会替换原图。
- 图片、亮度和设备设置持久保存。短触切图、长按约 1 秒熄屏、短触唤醒；向上滑动或运行时短按 BOOT 打开菜单。网页可操作设备菜单。
- 固件内轮播默认关闭，间隔 2～3600 秒（默认 10 秒），关闭网页后仍能运行。菜单、熄屏、图片传输期间暂停轮播；自动切图不逐次写 Flash。
- USB 与电池供电分别设置自动熄屏：全新配置默认为 USB 闲置 15 秒、电池关闭；0 为关闭，其余为 5～86400 秒。已有保存设置不被默认值覆盖。触摸、按键和主动控制续时，轮播、动画、状态轮询和帧回读不续时。
- 转场为 `direct` / `fade` / `slide`，默认 `fade`；持续运动为 `off` / `shift` / `rotate` / `gravity`，默认 `rotate`（24 秒一圈）。旋转周期 8～120 秒；`shift` 每 7.5 秒微移 1～2 像素。菜单、熄屏、大块图片 I/O 暂停运动。
- `gravity` 需主动选择，使用 QMI8658 加速度计让图片底边沿重力在屏幕平面上的投影向下，设备菜单与触摸坐标不旋转。水平放置、异常或过期样本会冻结最后有效姿态并报告状态；传感器不可用时不会静默改写保存设置。

## 电脑端安装与运行

需要 Python 3.10+、支持数据传输的 USB 线，以及可访问设备串口的权限。以下命令在项目根目录执行（Linux/macOS shell）：

```sh
python -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt
cp .env.example .env
python tools/serve.py
```

Windows 可用 `.venv\Scripts\Activate.ps1` 激活环境，用 `Copy-Item .env.example .env` 复制模板。安装依赖为 `pyserial` 和 `python-dotenv`，版本范围见 `requirements.txt`。

默认打开 **http://127.0.0.1:8765/**。未指定串口时按 Espressif USB VID/PID 自动查找设备；这些是厂商产品标识，不是个人设备序列号。多个候选设备时必须明确选择，例如：

```sh
python tools/serve.py --serial /dev/ttyACM0
python tools/serve.py --help
```

`/dev/ttyACM0` 仅为示例，请使用本机实际端口。Linux 如报权限错误，应按系统的串口权限策略授权当前用户；不要为了运行控制台直接用 root。网页服务必须运行在连接 USB 设备的电脑上。关闭终端或 Ctrl+C 停止服务不会停止固件内轮播。

### `.env` 与命令行优先级

服务始终从脚本所在项目根目录读取 `.env`，不依赖启动时的工作目录；网页路径也相对于项目解析。`.env` 可保存自己的 IP、域名、端口和串口，不要提交它。

| 变量 | 含义 | 未配置/留空时 |
| --- | --- | --- |
| `BADGE_HOSTS` | 逗号分隔的监听 IPv4 地址 | `127.0.0.1` |
| `BADGE_ALLOW_HOSTS` | 逗号分隔的额外允许访问的主机名，不含协议或端口 | 无额外主机名 |
| `BADGE_PORT` | HTTP 监听端口 | 未配置时 `8765` |
| `BADGE_SERIAL` | 指定串口路径 | 自动检测 |

优先级为 **显式命令行选项 > 进程环境变量 > `.env` > 默认值**，每个选项独立覆盖。重复的 `--host` 会**整体替换** `BADGE_HOSTS` 列表；重复的 `--allow-host` 同样整体替换 `BADGE_ALLOW_HOSTS`，不是在环境列表上追加。`--port` 与 `--serial` 覆盖对应环境值。

例如，临时强制只监听回环地址、不使用环境中的域名列表：

```sh
BADGE_ALLOW_HOSTS= python tools/serve.py --host 127.0.0.1 --port 8765
```

如需可信局域网或受控 Tailscale 网络访问，请只在本地 `.env` 中填写电脑实际拥有的 IPv4 地址及主机名。`badge.lan`、`badge.example.ts.net` 只是主机名格式示例，不能直接当作可用配置。监听器共用同一个 USB 串口事务锁；允许的 Host 包含回环名、监听 IP 及额外白名单名。

### 网络安全

- `BADGE_ALLOW_HOSTS` / `--allow-host` 只是 HTTP Host 白名单，**不配置 DNS，也不新增监听地址**。访问端必须能把域名解析到实际监听 IP；Tailscale 客户端还需对应网络和访问策略授权。
- 服务有 Host/Origin 检查，但**没有登录认证**。可访问端口的人都能控制设备。局域网 HTTP 是明文；Tailscale 跨设备传输由其隧道加密，但 HTTP 本身不变。
- 不要做公网端口转发，也不要使用 Tailscale Funnel 发布此服务。未配置网络监听时只开放本机回环地址。
- 不可信网络保持默认回环监听，可从另一台电脑建立 SSH 隧道（用自己的 SSH 用户和电脑名替换示例）：

  ```sh
  ssh -N -L 8765:127.0.0.1:8765 user@badge.lan
  ```

  然后在客户端打开 http://127.0.0.1:8765/。

## 从干净克隆构建固件

需要 Git、Python/PlatformIO Core、网络访问。`platformio.ini` 固定使用 pioarduino `55.03.312-1` 平台（Arduino ESP32 3.3.12 / ESP-IDF 5.5.5）、ESP32-S3、32MB Flash 和 OPI PSRAM 配置。不要将它替换成 PlatformIO 默认 ESP32 平台。

厂商源码不随本仓库提交，也不是 Git 子模块；编译依赖 `vendor/waveshare/examples/arduino/libraries`，尤其是 `Mylibrary`。在项目根目录执行以下步骤可恢复所需的官方依赖：

```sh
python -m pip install platformio
mkdir -p vendor
git clone --filter=blob:none --sparse https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C.git vendor/waveshare
git -C vendor/waveshare sparse-checkout set examples/arduino
git -C vendor/waveshare checkout --detach 6d19f7e16fb9a3be219e9eed43ca9eb56c88d01c
pio run -e picture-badge
```

上述 `git clone` 用于尚不存在 `vendor/waveshare` 的干净克隆；已有本地厂商目录时先检查其修改，不要覆盖。固定提交应保持为 `6d19f7e16fb9a3be219e9eed43ca9eb56c88d01c`，不能用上游最新分支代替。PlatformIO 首次运行会下载工具链与平台包；`.env` 只用于电脑端服务，不影响固件构建。

构建输出位于 `.pio/build/picture-badge/`。若电脑端服务占用串口，**先停止服务**再刷写（下面端口为示例）：

```sh
pio run -e picture-badge --target upload --upload-port /dev/ttyACM0
```

**刷写前先备份当前设备。** 不要执行 `uploadfs`、整片擦除或用旧整片备份覆盖新照片。`partitions.csv` 包含图片文件系统；刷写命令不是“保证保留所有数据”的恢复流程。已有设备的应用更新曾只写 `0x10000` 起的应用分区，以保留分区表、图片分区及 NVS；任何手工恢复都必须核对自己的备份、地址与硬件配置。私人整片/应用备份和摘要保存在本地 `backups/`，不包含在 GitHub 克隆中。

## 主机回归测试

无需接设备，使用支持 C++17 的 `g++`；渲染测试还需 AddressSanitizer / UndefinedBehaviorSanitizer 支持：

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror -Isrc \
    tests/test_gravity_motion.cpp src/animation_renderer.cpp -o /tmp/badge-gravity-check
/tmp/badge-gravity-check

g++ -std=c++11 -O2 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc \
    tests/test_animation_renderer.cpp src/animation_renderer.cpp -o /tmp/badge-renderer-check
/tmp/badge-renderer-check
```

## 显示与验证边界

以下是开发时已有验证记录的摘要，**不是每次克隆都会自动执行的验收，也不是本次配置整理新做的设备测试**。原始本地证据位于 `verification/` 与 `backups/`，不会随发布提供。

- 浏览器已验证桌面鼠标、390px Chromium 触摸模拟、横/竖图切换、透明图片、裁剪边界与重置，以及原生文件拖放、图片/文字剪贴板、混合拖放和无效输入保留预览。上传后的 434312 字节回读曾与预览量化后的 RGB565 逐字节一致；未用实体手机验收触摸手感。
- 50 槽位扩容曾验证自动使用新增槽位、填满 50 张、最后槽位覆盖与重启保留、49→0 回绕和拒绝槽位 50；新增槽位回读逐字节一致，已有图片的摘要和设置保留。50 张约占 20.71MiB；28.94MiB 图片分区预留一张临时上传后约余 7.81MiB（未扣文件系统开销）。
- 轮播、USB 自动熄屏、菜单超时、唤醒、设置保存和网页操作曾经 USB 实机验证。电池/插拔切换和计时回绕有主机策略测试，但未实际拔线确认实体切换。熄屏不是深度休眠，也不会停止充电；AMOLED 长时间常亮仍有老化/烧屏风险。
- 动画保留两份原图、两份输出及独立 USB/菜单缓冲，约 2.1MiB PSRAM；旋转始终采样原图，使用 Q16 逆映射、RGB565 双线性采样和圆形行区间，无逐帧分配或读盘。GPIO13 LCD_TE 与 40MHz QSPI/两块 16KiB DMA 缓冲用于同步传输，未超过手册 50MHz 上限。
- 实机曾测得 60Hz TE 周期约 16.7ms、整帧传输约 24.6～24.8ms。最多目标 10fps，晚帧跳过、不排队；高频查询时渲染、同步、提交曾为 59～174ms，不承诺恒定帧率。传输超出安全扫描窗口或同步失败时会停止运动并报告 `animation_error`；仅靠“双缓冲/TE”不能证明不撕裂。
- 三种转场目标/中间帧、微移、完整旋转、8/120 秒速度、菜单/网页/重启保存曾实机验证。状态查询拖慢旋转的问题曾复现并修正；66 次高频查询下观测到完整一圈。轮播、旋转、状态查询和画面回读同开时，USB 闲置熄屏曾约 15.37 秒。
- `gravity` 使用 4g、低功耗 21Hz 加速度计（陀螺仪关闭）、EMA 0.25 和半度死区；0.15g/0.22g 投影迟滞以及异常/500ms 过期检测用于冻结不可靠姿态。轴向依据固定官方 Arduino 示例及 LVGL 显示补偿说明，顺时针渲染角为 `atan2(-y, x)`。
- `gravity` USB 实机曾观测约 179° 姿态；原图、三种转场完成帧与主机渲染逐字节一致。菜单/熄屏暂停采样，退出/唤醒恢复，重启保留设置；与轮播同开时约 15.253 秒自动熄屏。最终低通关闭配置下的推滑曾为 129ms；应用更新前后已有图片的摘要一致，未留下演示设置。
- 主机回归覆盖重力四基准方向、跨零、噪声、水平/异常/过期样本、推滑采样及渲染颜色/边界。未用实体执行器确认四个朝向或水平姿态，未注入传感器缺失/TE 故障；无摄像头，实体撕裂、真实触摸手感和电池续航仍未验收。

`GET /api/image` 回读已保存原图；`DISPLAYFRAME` / `GET /api/display-image` 返回最后成功提交的 RGB565 图片帧缓存及 CRC（仅图片显示时可读）。帧缓存不是屏幕显存或摄像头回读，不包含亮度淡变，不能证明实体屏幕无撕裂。网页快照为手动获取，不自动刷新。

## 发布时应保留与排除的内容

提交固件源码、网页、工具、测试、`platformio.ini`、`partitions.csv`、`requirements.txt`、`.env.example` 和本文档。`.gitignore` 排除本地 `.env` 及其变体、Python 虚拟环境/缓存、`.pio/`、`backups/`、`verification/`、`vendor/`；不要用 `git add -f` 绕过这些规则。

私人 `.env`、设备验证记录和备份仅保留在本地，不属于发布内容。旧开发历史已备份到仓库外，不包含在当前用于发布的干净初始历史中；仓库外备份也不随发布提供。

忽略规则**不会移除已经跟踪的文件，也不会清除 Git 历史**。后续发布前仍应检查当前索引和历史中是否有私人配置、备份或设备记录；需要停止跟踪时用索引移除并保留本地文件。不要把“工作区已整理”当作“历史中没有私人内容”的证明。

## 官方参考

- [Waveshare 官方源码](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75C)
- [硬件原理图](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75C/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf)
- [CO5300 数据手册](https://dl.espressif.com/AE/esp-iot-solution/CO5300_Datasheet_V0.00.pdf)
- [固定 pioarduino 平台包](https://github.com/pioarduino/platform-espressif32/releases/download/55.03.312-1/platform-espressif32.zip)

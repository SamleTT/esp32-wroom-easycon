# 诊断版固件 —— 打开串口日志，专门用来排查蓝牙/配对问题

## 1. 这是什么

功能和默认版 `../` **完全一样**（原协议走板载 USB、EasyCon 走 UART2），
唯一区别是 **打开了串口日志**：

| | 默认版 `../` / `easycon-on-usb` | **本诊断版** |
|---|---|---|
| `CONFIG_LOG_DEFAULT_LEVEL` | `0`（NONE，日志被编译掉） | `3`（INFO） |
| `CONFIG_LOG_BOOTLOADER_LEVEL` | `0` | `3` |
| `EASYCON_UART_SWAP` | `0` / `1` | **`0`**（EasyCon 在 UART2，日志独占 UART0） |
| 串口能看到什么 | 只有上电时 ROM 的几行引导信息 | **完整的蓝牙初始化 + 配对 + HID 握手过程** |
| app 镜像大小 | 519,776 字节 | 555,616 字节 |

**为什么非要这么干**：正式版 `CONFIG_LOG_DEFAULT_LEVEL_NONE=y`，所有 `ESP_LOGx` 都被编译器删掉了，
出问题时串口**完全安静**，只能靠猜。打开日志后，蓝牙走到哪一步就能直接看见。

> **就是这个诊断版抓到了本次的两个致命 bug**——见下面第 4 节。

`EASYCON_UART_SWAP=0` 是刻意的：这样日志独占 UART0，输出干净不打架。
代价是这个版本里 EasyCon 走 UART2（要外接 USB-TTL 才能用），**它只用来排查，不是日常用版本**。

---

## 2. 怎么用（三步）

**第一步：刷进去**

双击 `flash-merged.bat`，提示时输入串口号。

> 本机 ESP32 板载 CH340 当前是 **`COM7`**（换 USB 口会变，以设备管理器为准）。

**第二步：关掉 EasyCon**（它会占着串口，导致抓取报"拒绝访问"）

**第三步：抓日志**

1. 打开 Switch 主机，进入 **手柄 → 更换握把顺序**，保持该界面
2. 双击 `capture-log.bat` → 输入 `COM7` → 按任意键开始
3. 脚本会**自动硬复位 ESP32**（走正常运行模式），并从加电第一行开始抓
4. 90 秒后同目录生成 **`diag-log.txt`**

---

## 3. 日志怎么判

| 看到什么 | 说明 |
|---|---|
| 反复出现 `rst:0x` | 板子在**重启循环**（多半是崩溃复位） |
| `E (...) enable controller failed` | 蓝牙控制器没起来（配置问题） |
| `Guru Meditation ... InstrFetchProhibited` | 跳到 NULL 指针，通常是 HID 模块没编进去 |
| `phy: phy_version:` 出现 | 蓝牙控制器**真的启用了**（正常的标志） |
| `authentication success: Nintendo Switch` | 配对认证通过 |
| `connection_cb: connected to ..` | 蓝牙链路建立 |
| `intr_data_cb: replyxx` | Switch 发子命令、固件在应答，会话已建立 |

---

## 4. 这个版本当初抓到的两个 bug（已修）

### 修复前的日志

```
I (956) BTDM_INIT: BT controller compile version [484c057]
E (963) app_main: enable controller failed: ESP_ERR_INVALID_ARG      ← 蓝牙死了
```
（更早的版本连这行都到不了）

把控制器模式改对之后，又暴露第二个：

```
I (2154) app_main: setting hid parameters
Guru Meditation Error: Core 0 panic'ed (InstrFetchProhibited)        ← 跳到 NULL
```

### 修复后的日志（真机实测，Switch 已连上）

```
I (943) easycon: EasyCon ready: uart=0 baud=115200 ver=0x46 MEM_SIZE=1024
I (947) BTDM_INIT: BT controller compile version [484c057]
I (1066) phy: phy_version: 4100, 2a5dd04, ...                        ← 控制器启用成功
I (2146) app_main: setting hid parameters
I (2153) app_main: starting hid device
I (2155) application_cb: app is now registered!
I (2169) app_main: setting to connectable, discoverable
I (3112) tag: authentication success: Nintendo Switch
I (3143) connection_cb: connected to bc:89:a6:56:f9:87
I (3149) send_task: Sending hid reports on core 0
I (3212) intr_data_cb: reply02
...                                                                  稳定运行，无重启
```

### 修的是什么

| `sdkconfig` 开关 | 错 | 对 |
|---|---|---|
| `CONFIG_BTDM_CONTROLLER_MODE_*` | `BLE_ONLY` | **`BR_EDR_ONLY`** |
| `CONFIG_BT_HID_DEV_ENABLED` | 未设置（`n`） | **`y`** |

两个都是 **IDF v3.0 → fork master(v4.0-dev)** 升级后新增的符号，
旧 `sdkconfig` 里没有 → `olddefconfig` 补成了错误默认值。
详细机理见 `../FLASH.md` 顶部的「重大修复」章节。

---

## 5. 文件清单

| 文件 | 大小 | 说明 |
|---|---:|---|
| `OpenSwitchPad-merged-0x0.bin` | 621,152 B | **单文件合并固件（推荐）** |
| `OpenSwitchPad.bin` | 555,616 B | 应用程序 |
| `bootloader.bin` | 24,048 B | 二级引导（开了引导日志，比正式版大） |
| `partitions_singleapp.bin` | 3,072 B | 单 app 分区表 |
| `OpenSwitchPad.elf` | 4,982,672 B | 调试符号 |
| `flash-merged.bat` / `flash-all.bat` | — | 一键烧录（GBK 编码） |
| `capture-log.bat` / `capture_log.py` | — | 一键抓日志 |

SHA-256：

```
OpenSwitchPad-merged-0x0.bin  59c470ced58aa4ee7d8c85ccb3e9591962b4db26d8389a5c3fb4bd0d9d3483ef
OpenSwitchPad.bin             8bcae2ac193123659d03fda82c58ba0a828d64a83eab4e50a035f8ee8f08c958
bootloader.bin                df11f9d94976787638fb7b3430c3b1c1b3ad32fbc55dd606cefec4e683010e0c
partitions_singleapp.bin      7f00b6c042a89b15b0cac534f82ed988caf29278ff5700b0c511eb1b5bb7c820
OpenSwitchPad.elf             b73f984c0c05cd3223663740915bdc5d64297b5b46e83dc7b54556e224f07e65
```

---

## 6. 排查完记得刷回去

诊断版日志会持续占用 UART0，**不是日常用版本**。
排查完请刷回：

- 只想用 EasyCon → `../easycon-on-usb/`
- 想用 AutoSplatoon → `../`

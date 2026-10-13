# `easycon-on-usb` —— EasyCon 走板载 USB（只需一根线）

这是把 `easycon_config.h` 里 **`EASYCON_UART_SWAP` 改成 `1`** 重新编译的版本：

| | 默认版（上一层 `dist/`） | **本目录变体** |
|---|---|---|
| **EasyCon**（115200） | UART2，需外接 USB-TTL（GPIO19/26） | **UART0，板载 USB（GPIO1/3）** ✅ |
| 原 UARTSwitchCon 协议（19200） | UART0，板载 USB（GPIO1/3） | UART2，需外接 USB-TTL（GPIO19/26） |
| 需要的线 | 板载 USB + 一根 USB-TTL | **只要板载 USB 一根** |
| AutoSplatoon | 可同时用（插板载 USB） | 要额外 USB-TTL 才能用 |

**适用场景**：你手上只有开发板自带的那根 USB 线（没有额外的 USB-TTL 适配器）。

> 为什么需要这个变体：默认版把 EasyCon 放在 UART2，而 GPIO19/26 **不在板载 USB 上**，
> 必须自己接 USB-TTL。只有一根线时，用默认版是**连不上 EasyCon** 的。

---

## 🛑 重要：本目录旧版固件蓝牙是死的，请刷这一版

本目录**之前**那版固件有个致命问题：蓝牙控制器从未启动，所以**永远连不上 Switch**。
根因与修复详见上一层 `../FLASH.md` 顶部的「重大修复」章节。简述：

| `sdkconfig` 开关 | 旧（错） | 新（对） |
|---|---|---|
| `CONFIG_BTDM_CONTROLLER_MODE_*` | `BLE_ONLY` | **`BR_EDR_ONLY`** |
| `CONFIG_BT_HID_DEV_ENABLED` | 未设置（`n`） | **`y`** |

现已修复，并**真机验证成功连上 Switch**（日志出现 `authentication success: Nintendo Switch`
与 `connection_cb: connected to bc:89:a6:56:f9:87`）。**请重新刷本目录的新固件。**

---

## 文件清单

| 文件 | 大小 | 烧录偏移 |
|---|---:|---|
| **`OpenSwitchPad-merged-0x0.bin`** | **585,312 B** | **0x0（单文件，推荐）** |
| `OpenSwitchPad.bin` | 519,776 B | 0x10000 |
| `bootloader.bin` | 15,600 B | 0x1000 |
| `partitions_singleapp.bin` | 3,072 B | 0x8000 |
| `OpenSwitchPad.elf` | 4,900,860 B | 调试用 |

SHA-256：

```
OpenSwitchPad-merged-0x0.bin  96c5abde5a8d199619b687ada5642d76e64f6b084db075cee8fd5e588c60e32b
OpenSwitchPad.bin             f729fb81381f17fdbef29ed8546b1aeb363edf6134a9ed71c880b432a783a4ff
bootloader.bin                803230ea70a30336f4a6421a09a2912ddbabad06c90e71f3ca90ff421a69c5e6
partitions_singleapp.bin      7f00b6c042a89b15b0cac534f82ed988caf29278ff5700b0c511eb1b5bb7c820
OpenSwitchPad.elf             ec65ab242ba078bcc28f829b4185006008a4d2d387ea442474ec549a050b71cc
```

> 镜像内嵌编译时间戳，重新编译后哈希必然变化、大小相同，属正常。

---

## 怎么用

**1. 烧录**：双击本目录 `flash-merged.bat`，提示时输入 COM 口。

> 本机 ESP32 板载 CH340 当前是 **`COM7`**（换 USB 口会变，以设备管理器为准）。
> `flash-merged.bat` 会自动找到本机已装好的 esptool，**不需要 `pip install`**。

或者手敲：

```bat
"C:\esp32-build\pyenv\Scripts\python.exe" -m esptool --chip esp32 --port COM7 --baud 921600 ^
  --before default_reset --after hard_reset ^
  write_flash -z --flash_mode dio --flash_freq 40m --flash_size detect ^
  0x0 OpenSwitchPad-merged-0x0.bin
```

**2. 配对到 Switch**：主机上进入 **手柄 → 更换握把顺序**，保持该界面打开，然后复位/上电 ESP32。
（本固件模拟的是 Switch 1 的 **Pro Controller**；NS2 向下兼容，所以 NS2 也能连。）

**3. 连 EasyCon**：EasyCon 里选 **`COM7`**、波特率 **115200**、手柄类型 **Pro Controller**。

---

## 两个注意点

1. **本版本的原协议挪到了 UART2（GPIO19/26）**，板载 USB 上**没有**原协议——
   想在板载 USB 上跑 AutoSplatoon 请用默认版 `dist/`。
2. **正式版无串口日志**（被编译掉了），这是正常的。
   想看启动/配对过程，刷 `dist/diag/`，在板载 USB 上以 **115200** 查看。

---

协议细节、接线、排错见：`../FLASH.md` 与 `../../EASYCON_PORT.md`

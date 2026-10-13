# OpenSwitchPad 固件烧录说明（UARTSwitchCon + EasyCon 双协议）

本目录是**已经编译好的成品固件**，无需再装工具链即可烧录。
固件 = `nullstalgia/UARTSwitchCon`（ESP32，蓝牙手柄模拟）+ `EasyConNS` 的 MCU 串口协议与片上 ECS 脚本虚拟机。

---

## ⚠️⚠️ 重大修复（v2）：之前所有版本蓝牙都是死的

**如果你之前刷过本目录的旧固件，请务必重刷。** 旧固件的蓝牙**从未启动过**，
表现是"能握手、但怎么都连不上 Switch（NS1/NS2 都是）"。

根因是两个 `sdkconfig` 开关在从 ESP-IDF **v3.0 升到 fork 的 master（v4.0-dev）**时被补成了错误默认值：

| 开关 | 错误值（旧） | 正确值（现在） | 后果 |
|---|---|---|---|
| `CONFIG_BTDM_CONTROLLER_MODE_*` | `BLE_ONLY` | **`BR_EDR_ONLY`** | 控制器按纯 BLE 初始化，而程序请求 `ESP_BT_MODE_CLASSIC_BT` → 模式不匹配 → `esp_bt_controller_enable()` 返回 `ESP_ERR_INVALID_ARG` → **蓝牙控制器根本没启用** |
| `CONFIG_BT_HID_DEV_ENABLED` | 未设置（`n`） | **`y`** | fork 新增的 HID Device 开关。为 `n` 时 `bt_target.h` 不定义 `HID_DEV_INCLUDED`/`BTA_HD_INCLUDED`，整个 `btc_hd.cc` 编译成空壳 → 调 `esp_hid_device_register_app()` 时跳到 NULL → **崩溃重启** |

**为什么会错**：这两个符号在 IDF **v3.0 里不存在**（都是升级到 v4.0-dev 后新增的），
而工程的 `sdkconfig` 是 v3.0 时代生成的，里面没有它们。
非交互编译时 `olddefconfig` 必须补默认值——偏偏 `BTDM_CONTROLLER_MODE` 这个 choice
**没有写 `default`**，Kconfig 规则取第一个选项（BLE Only）；`BT_HID_DEV_ENABLED` 的默认是 `n`。

**修复已在 `sdkconfig` 里落实，且用真机日志逐级验证过**（见第 3 节）。
以后重新编译请**不要再动**这三行：

```
CONFIG_BTDM_CONTROLLER_MODE_BR_EDR_ONLY=y
CONFIG_CLASSIC_BT_ENABLED=y
CONFIG_BT_HID_DEV_ENABLED=y
```

> 顺便纠正本仓库 README 的一个错误：它说用 `release/v3.0`，那是错的——
> fork 的 `release/v3.0` 分支**没有 `esp_hidd_api.h`**（编不过）。
> 必须用 fork 的 **`master`**（`v4.0-dev-466-gc7ca88081`）。

---

## ⚠️ 先选版本：三个固件，区别只在两件事

| 目录 | EasyCon 走哪 | 原协议走哪 | 串口日志 | 需要几根线 |
|---|---|---|---|---|
| **`dist/`（本目录，默认）** | UART2 → **必须外接 USB-TTL**（GPIO19/26） | UART0 板载 USB | 关 | 板载 USB **+** 一根 USB-TTL |
| **`dist/easycon-on-usb/`** | **UART0 板载 USB** | UART2 → 外接 USB-TTL | 关 | **只要板载 USB 一根** |
| **`dist/diag/`** | UART2（同上） | UART0 板载 USB | **开（INFO）** | 同上；用于排查问题 |

**怎么选**：

- 只想用 EasyCon、手上只有开发板自带那根 USB 线 → 用 **`dist/easycon-on-usb/`**
- 想用 AutoSplatoon（原协议）走板载 USB → 用本目录 `dist/`
- 出问题了要抓日志 → 用 `dist/diag/`

> 三个版本的 UART 归属已用**反汇编核实**（不是"看起来一样"）：
> `easycon_init` 里 `uart_driver_install` 拿到的第一个参数，默认版/`diag` 是 `2`（UART2）+ `uart_set_pin(19, 26)`；
> `easycon-on-usb` 是 `0`（UART0）+ `PIN_NO_CHANGE`。

---

## 1. 文件清单（`dist/`，默认版）

| 文件 | 大小 | 烧录偏移 | 说明 |
|---|---:|---|---|
| `OpenSwitchPad-merged-0x0.bin` | 585,312 B | **0x0** | **单文件合并固件（推荐）** |
| `OpenSwitchPad.bin` | 519,776 B | 0x10000 | 应用程序 |
| `bootloader.bin` | 15,600 B | 0x1000 | 二级引导 |
| `partitions_singleapp.bin` | 3,072 B | 0x8000 | 单 app 分区表 |
| `OpenSwitchPad.elf` | 4,900,864 B | — | 带调试符号，`addr2line` 解析崩溃日志用 |

SHA-256：

```
OpenSwitchPad-merged-0x0.bin  99ed0203bf6882068a7a34115306a44d3a1b5250b4306b02ff75613952f5f584
OpenSwitchPad.bin             b4b02691ff65d816ed924e13ab295be17ad0e672accbb9824c3d94c8651da704
bootloader.bin                803230ea70a30336f4a6421a09a2912ddbabad06c90e71f3ca90ff421a69c5e6
partitions_singleapp.bin      7f00b6c042a89b15b0cac534f82ed988caf29278ff5700b0c511eb1b5bb7c820
OpenSwitchPad.elf             6ac0c7e5315742ec3be7efc303f7f5ab067336487973c30e43f67d87b0bbb346
```

> 合并固件已用 `esptool merge-bin` 交叉验证，**逐字节一致**。
> 注意：镜像内嵌编译时间戳（`CONFIG_APP_COMPILE_TIME_DATE=y`），**重新编译后的哈希必然不同、大小相同**，属正常。

**编译信息**：ESP-IDF `v4.0-dev-466-gc7ca88081`（`nullstalgia/esp-idf` master）
+ GNU Make 构建系统 + `xtensa-esp32-elf-gcc 5.2.0`。

---

## 2. 烧录（推荐：合并固件，一条命令）

### 2.1 烧录工具（本机已就绪，无需安装）

**本机已经装好了 esptool，不用做任何安装**：

```
C:\esp32-build\pyenv\Scripts\python.exe -m esptool     ← esptool 5.5.0
```

`flash-merged.bat` / `flash-all.bat` 会**自动探测**可用的 esptool，
按以下顺序尝试，命中即用：

1. 上面这个本机已装好的环境（优先）
2. PATH 上的 `esptool`
3. PATH 上的 `esptool.py`
4. `py -3 -m esptool`
5. `python -m esptool`

所以**直接双击脚本即可**，不需要先 `pip install`。

> 若换到别的电脑：先 `pip install esptool`（只烧录**不需要 ESP-IDF**），脚本同样能自动找到。

### 2.2 找到串口

设备管理器 → 端口(COM 和 LPT)。ESP32 开发板一般是 `USB-SERIAL CH340 (COM?)`，
下面用 `COM7` 举例，请替换成实际端口。

> 本机串口情况：`COM1` = 主板自带（无关）；`COM3` = `ATK N9 PRO dongle` 的串口接口（**不是 USB-TTL**）；
> ESP32 板载 CH340 当前是 **`COM7`**（插拔不同 USB 口会变，以设备管理器为准）。

### 2.3 烧录

```bat
esptool.py --chip esp32 --port COM7 --baud 921600 ^
  --before default_reset --after hard_reset ^
  write_flash -z --flash_mode dio --flash_freq 40m --flash_size detect ^
  0x0 OpenSwitchPad-merged-0x0.bin
```

或者直接双击本目录的 **`flash-merged.bat`**（会提示输入 COM 口，自动找 esptool）。

**本机不想用脚本的话，用这条等价命令**：

```bat
"C:\esp32-build\pyenv\Scripts\python.exe" -m esptool --chip esp32 --port COM7 --baud 921600 ^
  --before default_reset --after hard_reset ^
  write_flash -z --flash_mode dio --flash_freq 40m --flash_size detect ^
  0x0 OpenSwitchPad-merged-0x0.bin
```

> esptool 5.x 会提示 `--flash_mode` 等参数名已过时（新名字是 `--flash-mode`），
> **只是告警，功能正常**；写成下划线是为了兼容旧版 esptool。

> 合并固件从 `0x0` 一次写入，包含引导 + 分区表 + 应用，**不会漏刷分区表**。

### 2.4 备用：分三个文件烧

```bat
esptool.py --chip esp32 --port COM7 --baud 921600 ^
  --before default_reset --after hard_reset ^
  write_flash -z --flash_mode dio --flash_freq 40m --flash_size detect ^
  0x1000 bootloader.bin  0x8000 partitions_singleapp.bin  0x10000 OpenSwitchPad.bin
```

对应脚本：`flash-all.bat`。

### 2.5 先擦再刷（换版本、异常跑飞时用）

```bat
esptool.py --chip esp32 --port COM7 erase_flash
```

然后再执行上面任意一种烧录。

**必须的接线**（烧录时）：`TX↔RX` 交叉、`GND` 共地；
ESP32 进入下载模式一般是自动的，若不行就按住 `BOOT` 再点 `EN`。

### 2.6 备用：用乐鑫 Flash Download Tool 烧录

想用图形工具（如 `flash_download_tool_3.9.3`）也可以，**合并固件可以直接烧**。
启动时先选芯片 **ESP32**，然后按下表填：

| 项 | 填什么 |
|---|---|
| 文件 | `...\dist\easycon-on-usb\OpenSwitchPad-merged-0x0.bin` |
| 地址 | **`0x0`**（前面复选框要打勾） |
| SPI SPEED | `40MHz` |
| SPI MODE | `DIO` |
| Flash SIZE | `32Mbit`（=4MB） |
| COM | `COM7`（先关掉 EasyCon，它占着串口） |
| BAUD | `921600`（报错就降到 `460800`） |
| DoNotChgBin | **勾上**（保持镜像原样） |

点 `START` → 等 `FINISH` → 拔插或按 `EN` 复位。

**三条必须注意的**：

1. **只放这一个文件**。工具的默认行是它自带 `bin\bootloader.bin` + 地址 `0x0` ——
   **千万别这么烧**。ESP32 的 bootloader 必须在 `0x1000`，放 `0x0` 会直接变砖。
   我们的合并固件内部已经把 bootloader 摆在 `0x1000`，所以整体从 `0x0` 写是对的。
2. **不要用 `bin\boot_app0.bin`**。那是给 OTA 分区用的；本固件分区表是
   单 app（`factory` @ `0x10000`，1 MB），没有 OTA 分区，不需要它。
3. 合并镜像会顺带覆盖 `0x9000` 的 **NVS 分区**（镜像里这段是 `0xFF` 空白）。
   本固件把「伪 Nintendo 蓝牙 MAC」存在 NVS 里，所以**每次刷合并固件，
   板子的蓝牙地址都会重新随机生成**，Switch 需要重新配对一次。
   想保留地址就改用 §2.4 的分三文件烧法（不碰 NVS）。

---

## 3. 验证（真机跑通的实际日志）

> ⚠️ **正式版看不到日志是正常的**：`CONFIG_LOG_DEFAULT_LEVEL=0` 把所有 `ESP_LOGx` 编译掉了，
> 启动时**没有任何文字日志**（只有芯片 ROM 自己的 `rst:... boot:...` 几行）。
> 想看过程请刷 `dist/diag/`（日志开 INFO）。

修复后，串口日志是这样的（这是真机实测，Switch 已成功连上）：

```
I (943) easycon: EasyCon ready: uart=2 baud=115200 ver=0x46 MEM_SIZE=1024
I (947) BTDM_INIT: BT controller compile version [484c057]
I (1066) phy: phy_version: 4100, ...                    ← 出现这行 = 蓝牙控制器真的启用了
I (2146) app_main: setting hid parameters
I (2153) app_main: starting hid device
I (2155) application_cb: app is now registered!
I (2169) app_main: setting to connectable, discoverable  ← 开机即对外可发现
I (3112) tag: authentication success: Nintendo Switch    ← 配对成功
I (3143) connection_cb: connected to bc:89:a6:56:f9:87
I (3149) send_task: Sending hid reports on core 0
I (3212) intr_data_cb: reply02                           ← Switch 发来子命令，固件正常应答
I (3258) intr_data_cb: reply08
I (3453) intr_data_cb: reply04
...                                                      之后是一整套握手，稳定无重启
```

对比**修复前**（会看到这两句就停住/崩）：

```
E (963) app_main: enable controller failed: ESP_ERR_INVALID_ARG      ← 蓝牙没起来
I (2154) app_main: setting hid parameters
Guru Meditation Error: Core 0 panic'ed (InstrFetchProhibited)        ← 跳到 NULL，重启循环
```

### 3.1 无日志时的自检：原协议握手（板载 USB，19200）

用任意串口工具打开板载串口，波特率 **19200**，发送 `FF 33 CC`，期望回 **`FF CC 33`**。
收到即为同步成功、固件活着。

（`chocolate` 握手 `FF 44 EE` → 期望回 `FF EE 03`；`03` = PRO_CON，与 `CONTROLLER_TYPE` 一致。）

### 3.2 判断"是否已连上 Switch"

同步成功后，若已配对/已连接，固件会**每约 15ms 回一个 `0x90`**，表现为持续的 `90 90 90 ...`。

- 有 `0x90` 流 → 已连上 ✅
- 完全没有 → 还没配对，或 Switch 没开/不在旁边

### 3.3 EasyCon 自检（115200）

在 EasyCon 所在串口发心跳 `A5 A5 81`，期望回 **`80`**。

---

## 4. 接线与使用

### 4.1 两条 UART 各跑一套协议

| 版本 | 用途 | 串口 | 引脚 | 波特率 |
|---|---|---|---|---|
| 默认 / `diag` | 原协议（AutoSplatoon 等） | UART0 | GPIO1(TX)/GPIO3(RX)，板载 USB | 19200 |
| 默认 / `diag` | EasyCon | UART2 | **TX=GPIO19，RX=GPIO26** | 115200 |
| `easycon-on-usb` | EasyCon | UART0 | 板载 USB | 115200 |
| `easycon-on-usb` | 原协议 | UART2 | GPIO19/26 | 19200 |

默认版 EasyCon 接线：

```
USB-TTL 适配器            ESP32
  TX  ────────────────►  GPIO26  (EasyCon RX)
  RX  ◄────────────────  GPIO19  (EasyCon TX)
  GND ────────────────   GND
```

### 4.2 使用步骤

1. 把 ESP32 **配对到 Switch**：主机上进入 **手柄 → 更换握把顺序**，保持该界面打开。
   固件是"开机即可被发现"，所以**在该界面打开后再复位/上电 ESP32** 最稳。
   （模拟的 Pro Controller 没有 SYNC 键，靠自动广播。）
2. 连上后固件会自动转为"不可被发现"，开始每 ~15ms 发状态报文。
3. EasyCon 里选对应 COM 口、波特率 **115200**、手柄类型 **Pro Controller**。
4. EasyCon 发心跳 `A5 A5 81`，收到 `80` 即联机成功。
5. 也可以把 ECS 脚本烧进固件，由**片上虚拟机独立运行**（≤1024 B，掉电保存，上电自启）。

**切回 AutoSplatoon**：EasyCon 静默 **12 秒**后固件自动松手并把控制权交还原协议；也可以直接复位 ESP32。

---

## 5. 资源占用（默认版，日志关）

```
DRAM .data        11,432 B
DRAM .bss         42,744 B
Used static DRAM  54,176 B  (70,404 可用, 43.5%)
Used static IRAM  70,424 B  (60,648 可用, 53.7%)
Flash code       381,886 B
Flash rodata      55,924 B
Total image    ~ 519,666 B

app 分区容量 1 MB，已用 519,776 B (49.6%)
```

余量充足。（开日志的 `diag` 版会大一些，约 555 KB，仍在 1 MB 内。）

---

## 6. 排错

| 现象 | 排查方向 |
|---|---|
| **连不上 Switch（NS2/NS1 都是）** | **① 是不是刷了旧固件？** 旧版蓝牙从未启动。刷本目录/`easycon-on-usb` 的新固件。② 主机要**开机**（睡眠不行）；③ 必须走 **手柄 → 更换握把顺序** 并保持该界面打开；④ 先 `设置 → 手柄与感应器 → 断开手柄` 清掉旧配对记录再试 |
| EasyCon 连不上（一直转圈） | 波特率必须 **115200**；COM 口选对；用 `easycon-on-usb` 版时 EasyCon 才在板载 USB 上 |
| 按键全都不对 | EasyCon 里的**手柄类型**要和固件 `CONTROLLER_TYPE` 一致（默认 Pro） |
| 摇杆上下颠倒 | 改 `easycon_config.h` 的 `EASYCON_Y_FLIP` 为 `0` 重新编译 |
| 部分按键没反应 | 当前是 `JOYCON_L/JOYCON_R`，单 Joy-Con 物理上没那些键 → 改 `PRO_CON` |
| AutoSplatoon 连不上 | 默认版原协议在 UART0（板载 USB，19200）；用 `easycon-on-usb` 时会挪到 UART2，接线要 TX↔RX |
| Switch 上卡键 | 已由 12 秒静默看门狗兜底；想更快可调小 `EASYCON_IDLE_RELEASE_MS` |
| 刷完没反应 | 先 `erase_flash` 再刷；确认用的是合并固件或三个文件都刷了 |
| 想抓日志 | 刷 `dist/diag/`（swap=0 + 日志 INFO），板载 USB 115200 直接看 |

---

## 7. 重新编译

源码改动后要重新编译（完整 ESP-IDF 环境本机已装好）：

```bash
bash.exe --noprofile --norc /c/esp32-build/msys32/home/esp/build.sh
```

**编译前必须确认 `sdkconfig` 里这三行没被改回错误值**（见本文件顶部）：

```
CONFIG_BTDM_CONTROLLER_MODE_BR_EDR_ONLY=y
CONFIG_CLASSIC_BT_ENABLED=y
CONFIG_BT_HID_DEV_ENABLED=y
```

产物输出到 `../build/`，再按需打包到 `dist/`。

---

详细的协议对照、配置项说明、改动清单见：`../../EASYCON_PORT.md`

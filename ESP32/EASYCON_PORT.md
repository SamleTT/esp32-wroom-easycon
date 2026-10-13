# UARTSwitchCon × EasyCon 双协议固件移植说明

> 给原本「只能被 AutoSplatoon / UARTSwitchCon 上位机驱动」的 UARTSwitchCon ESP32 固件，
> 加上 **EasyCon 的 MCU 串口协议 + ECS 脚本虚拟机**，参考实现为 EasyCon 官方
> ESP32-S3 固件 `EasyConNS/EasyMCU_ESP32`。
>
> **原有能力（连 NS1 + 连 NS2 的蓝牙 HID 手柄模拟）一行都没有改动**，
> 只是在旁边多挂了一路 UART 和一套输入源。

---

## 1. 为什么不能直接「把 EasyCon 固件刷进去」

两个项目的串口协议**完全不兼容**，而且波特率不同，无法在同一个 UART 上共存：

| | UARTSwitchCon 协议 | EasyCon 协议 |
|---|---|---|
| 波特率 | **19200** | **115200** |
| 帧格式 | 9 字节 + CRC8-CCITT(poly 0x07) | 变长包，**除末字节外最高位=0，末字节最高位=1** |
| 握手 | vanilla `FF 33 CC` / chocolate `FF 44 EE` | 心跳 `A5 A5 81` → 回 `80` |
| 报文 | 9 字节原始报文直通 | 7 字节 Report 按 **7bit 大端**拆成 8 字节 |
| 能力 | 仅实时遥控 | 实时遥控 **+ 脚本烧录 + 片上虚拟机跑脚本** |
| NS2 | ✅ 支持（含 v12 chocolate 握手） | ❌ 官方固件只支持 NS1 |

所以方案是：**两条 UART 各跑一套协议，共用同一套蓝牙 HID 输出。**

---

## 2. 架构

```
                    ┌──────────────────── ESP32 ────────────────────┐
   AutoSplatoon ──► │ UART0  19200  原协议状态机 ──┐                │
   (19200 8N1)      │  (GPIO1/GPIO3)               │                │
                    │                              ├─► but1/but2/but3 │
   EasyCon PC  ──►  │ UART2  115200 EasyCon 状态机 ┘   lx/ly/cx/cy   │
   (115200 8N1)     │  (GPIO19=TX, GPIO26=RX)          │             │
                    │                                   ▼             │
                    │            send_buttons() 每 15ms 组装 report30 │
                    │                                   │             │
                    │                        esp_hid_device_send_report
                    └───────────────────────────────────┼─────────────┘
                                                        ▼
                                       蓝牙 HID ─► Switch (NS1 / NS2)
```

### 关键设计：影子报文（shadow report）

EasyCon 视角下永远是一份**完整的 Pro 手柄报告**
（16 位按键字 + HAT + LX/LY/RX/RY），而固件内部只有 `but1/but2/but3 + lx/ly/cx/cy`。

`easycon.c` 里维护一份「影子状态」，每次 EasyCon 改一个字段就重新做一次**折叠翻译**
（`easycon_apply()`），再写进固件原来就有的那几个变量：

- `CONTROLLER_TYPE == PRO_CON` → **1:1 直映**（推荐，也是默认值）
- `CONTROLLER_TYPE == JOYCON_L / JOYCON_R` → 折叠：A/B/X/Y 并进十字键、双摇杆并成一个、
  L/ZL/R/ZR 与 SL/SR 合并。**单个 Joy-Con 物理按键本来就少，这个映射必然是有损的**，
  EasyCon 脚本里用不到的键会被丢弃。

这样做的最大好处：**EasyCon 的输入走的是和原协议完全相同的出口**，
所以「NS1 能连、NS2 能连」这个结论对 EasyCon 一样成立，不需要碰蓝牙那一层。

---

## 3. 改动清单

### 新增文件（`ESP32/source/firmware/main/`）

| 文件 | 行数 | 作用 |
|---|---|---|
| `easycon.c` | ~1098 | **核心**：串口状态机 + ECS 脚本虚拟机 + 影子报文翻译 + 静默看门狗 |
| `easycon.h` | 153 | 协议常量（命令字/应答字/HAT/按键位）、对外接口 |
| `easycon_config.h` | 107 | **集中配置**：控制器类型、两路 UART 分工、Y 轴方向 |
| `easycon_storage.c` | 132 | 掉电存储：RAM 影子 + NVS blob，延迟后台落盘 |
| `switchcon_bridge.h` | 42 | `easycon.c` → `main.c` 的桥接接口声明 |

> ESP-IDF v3.0 用的是 `component.mk` 默认规则（编译目录下所有 `.c`），
> 所以新文件**不需要改构建脚本**，放进 `main/` 就会自动参与编译。

### 修改文件

**`main.c`**（1311 → 1322 行，共 6 处，全部是「加」不是「改」）

| 位置 | 改动 |
|---|---|
| L35–38 | `#include` 三个新头文件 |
| L105–110 | `UART_TXD_PIN/UART_RXD_PIN/UART_NUM` 改成 `LEGACY_UART_*` 的别名；`uart_config.baud_rate` 改用 `LEGACY_UART_BAUD_RATE`；删掉本地 `PRO_CON/JOYCON_L/JOYCON_R/CONTROLLER_TYPE`（移进 `easycon_config.h`） |
| L573–618 `send_buttons()` | 放行条件从「仅原协议已握手」改成「**原协议已握手 或 EasyCon 已激活**」；`0x90` 应答**只在原协议模式下发**（否则会被 EasyCon 上位机当成控制字节）；EasyCon 模式下每发一帧回调一次 `EasyCon_report_send_callback()` |
| L628–666 | 新增 5 个桥接函数 `switchcon_set_buttons / set_left_stick / set_right_stick / reset_inputs / is_connected / data_blink`，全部与原协议共用 `xSemaphore`，保证一帧输入快照原子取走 |
| L1253–1254 `app_main()` | `nvs_flash_init()` 之后、`set_bt_address()` 之前调用 `easycon_init()` |

> **蓝牙 HID 部分（配对、握手、报告结构、NS1/NS2 兼容代码）零改动。**

---

## 4. 配置（只需改 `easycon_config.h` 三处）

### 4.1 `CONTROLLER_TYPE`（默认 `PRO_CON`）

```c
#define CONTROLLER_TYPE PRO_CON   /* 默认：EasyCon 推荐 */
```

- **`PRO_CON`** —— EasyCon 官方参考固件就按 Pro 手柄上报，社区脚本也基本按 Pro 写。
  按键最全（A/B/X/Y/L/R/ZL/ZR/+/−/HOME/CAPTURE/L3/R3/十字键，双摇杆独立）。
- **`JOYCON_L`** —— 原 UARTSwitchCon 的默认值，作者说配对最省心。主要用 AutoSplatoon 时可以换回来。
- **`JOYCON_R`** —— 右 Joy-Con。

> 三种类型**都**能连 NS1 和 NS2，这个开关只影响可用按键数量和 EasyCon 里的手柄类型选择。

### 4.2 `EASYCON_UART_SWAP`（默认 `0`）

| 值 | 原协议 | EasyCon | 适用场景 |
|---|---|---|---|
| **`0`（默认）** | UART0（GPIO1/3，板载 USB） | **UART2（TX=GPIO19 / RX=GPIO26）** | 两条线各插一个上位机；AutoSplatoon 保持原样 |
| `1` | UART2（GPIO19/26） | **UART0（板载 USB）** | 只有一根 USB 线，专供 EasyCon |

> ⚠️ **`EASYCON_UART_SWAP = 1` 时务必同时关闭 ESP-IDF 的串口日志**，
> 否则启动日志会以 115200（和 EasyCon 同波特率）注入 UART0，扰乱 EasyCon 的包同步：
> `make menuconfig` → `Component config` → `ESP32-specific` → `UART for console output` → **None**。
> 默认值 `0` 把 EasyCon 放在独立的 UART2 上，不存在这个问题。

### 4.3 `EASYCON_Y_FLIP`（默认 `1`）

EasyCon 报文里 `LY=0` 是「上」，固件内部 `ly` 是 `255` 是「上」，所以默认翻转一次。
**实机若发现上下颠倒，改成 `0` 即可**，不用动别的代码。

---

## 5. 接线

UART2（默认给 EasyCon）：

```
USB-TTL 适配器            ESP32
  TX  ────────────────►  GPIO26  (EasyCon RX)
  RX  ◄────────────────  GPIO19  (EasyCon TX)
  GND ────────────────   GND
```

UART0（原协议）用板载 USB 口即可，**TX 接 TX、RX 接 RX**（照抄上游 README 的接法）。

> 这三根线正好是上游 README 自己推荐的两组引脚，所以即使固件里同时装了两套协议，
> 硬件上也不需要额外的引脚规划。

---

## 6. 编译与烧录

固件是**老式 GNU Make 工程**（`Makefile` + `component.mk`，不是 CMake），
而且必须用上游 fork 过的 IDF —— **`nullstalgia/esp-idf` 的 `master` 分支**。

> ⚠️ 仓库 README 里写的是 `release/v3.0`，但那是**错的**：
> 固件用的 `esp_hidd_api.h`（BT HID Device API）在 espressif 官方
> `release/v3.0`~`v3.3` **全都没有**，只有这个 fork 的 master（v4.0-dev）里才有；
> 而且项目 `sdkconfig` 用的是 `CONFIG_BLUEDROID_ENABLED` / `CONFIG_CLASSIC_BT_ENABLED` /
> `CONFIG_BT_ACL_CONNECTIONS`，**正是 master 的 BT Kconfig 命名**。

```bash
# 1. 准备工具链与 IDF（需用 nullstalgia/esp-idf 的 master 分支）
source $IDF_PATH/export.sh

# 2. 配置（首次或改过 swap 时）
cd ESP32/source/firmware
make menuconfig          # 若 EASYCON_UART_SWAP=1，请把 console output 设为 None

# 3. 编译
make -j4
```

### 上电后怎么确认在跑

> ⚠️ 本项目 `CONFIG_LOG_DEFAULT_LEVEL=0`，**所有 `ESP_LOGx` 都被编译掉了**，
> 所以启动时**没有任何文字日志**，只能看到芯片 ROM 自己的
> `rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)` 那几行。

正确做法是**用协议握手自检**（同时也是区分新旧固件的手段）：

| 串口 | 波特率 | 发送 | 期望回应 |
|---|---|---|---|
| 板载 USB（原协议） | 19200 | `FF 33 CC` | `FF CC 33`（vanilla） |
| 板载 USB（原协议） | 19200 | `FF 44 EE` | `FF EE 03`（chocolate，`03` = PRO_CON） |

> **`03` 这个字节本身就是「跑的是本移植固件」的指纹**：上游原版默认是
> `JOYCON_L`，会回 `01`；本移植把默认改成 `PRO_CON`，所以回 `03`。

### 本仓库已编译好的成品

**已经在 Windows 上编译成功**，成品固件在
`ESP32/source/firmware/dist/`，烧录说明见 `dist/FLASH.md`：

| 文件 | 大小 | 偏移 |
|---|---:|---|
| `OpenSwitchPad-merged-0x0.bin` | 630,240 B | **0x0（单文件，推荐）** |
| `OpenSwitchPad.bin` | 564,704 B | 0x10000 |
| `bootloader.bin` | 15,600 B | 0x1000 |
| `partitions_singleapp.bin` | 3,072 B | 0x8000 |
| `OpenSwitchPad.elf` | 5,310,804 B | — |

一条命令烧录：

```bat
esptool.py --chip esp32 --port COM3 --baud 921600 ^
  write_flash -z --flash_mode dio --flash_freq 40m --flash_size detect ^
  0x0 OpenSwitchPad-merged-0x0.bin
```

### 本机编译环境（已搭好，可直接复用）

| 组件 | 路径 / 版本 |
|---|---|
| MSYS2 (MINGW32) | `C:\esp32-build\msys32\` |
| `xtensa-esp32-elf-gcc` | 5.2.0（`msys32\opt\xtensa-esp32-elf\bin`） |
| IDF | `C:\esp32-build\esp-idf-master\`（`v4.0-dev-466-gc7ca88081`） |
| 构建脚本 | `C:\esp32-build\msys32\home\esp\build.sh` |

```bash
bash.exe --noprofile --norc /c/esp32-build/msys32/home/esp/build.sh
```

> 踩坑记录（已修）：
> 1. `esp-idf-master/make/ldgen.mk` 的 Windows 分支把目标名误写成了 `ldgen.section_infos`，
>    导致 `No rule to make target '.../build/ldgen_libraries'`，已改回 `ldgen_libraries`；
> 2. 非交互构建必须设 `BATCH_BUILD=1`，否则 kconfig 会因为无法提问而 `aborted!`。

---

## 7. 使用流程

1. **配对 Switch**：按上游原有方式把 ESP32 当成手柄配对到 NS1 或 NS2（这一步和以前完全一样）。
2. **接上 EasyCon**：把 USB-TTL 插到 PC，在 EasyCon 里选对应 COM 口。
   - 波特率默认 **115200**（EasyCon 自己会先试 115200，失败才回落 9600）。
   - 手柄类型选和 `CONTROLLER_TYPE` 一致的那个（默认 Pro Controller）。
3. **联机**：EasyCon 发心跳 `A5 A5 81`，固件回 `80`，状态栏变绿即已连上。
   之后按键/摇杆实时生效。
4. **跑脚本**：可以直接用 EasyCon 实时遥控，也可以把 ECS 脚本烧进固件由**片上虚拟机自己跑**
   （最多 1024 字节，掉电保存，重新上电自动启动）。

> **切回 AutoSplatoon**：EasyCon 掉线 12 秒后固件会自己松手并交还原协议（见下节）；
> 也可以直接复位 ESP32。

---

## 8. 协议实现要点（对照官方 AVR / ESP32 参考实现）

| 项目 | 实现 |
|---|---|
| 帧同步 | 靠 MSB：连续收字节直到收到 `>=0x80` 的字节，视为一包结束 |
| 心跳 | `A5 A5 81` → `80`（`REPLY_HELLO`）；`A5` = `CMD_READY`，同时用于解锁状态机 |
| 按键报文 | 8 字节 → 按 7bit 大端解出 7 字节 Report，再拆成 Button(16)/HAT/LX/LY/RX/RY |
| 报文应答 | 已连接回 `0xFF`(`Ack`)，未连接回 `0x00`(`Error`)，忙回 `0xFE`(`Busy`) |
| 版本查询 | `A5 85` → `0x46`（上位机只校验 `0x40 ≤ ver ≤ 0x80`） |
| Flash 烧录 | `A5,addr&0x7F,addr>>7,len&0x7F,len>>7,0x82` → 连续收 `len` 字节 → 回 `0x82`(`FlashEnd`) |
| 脚本启停 | `0x83`/`0x84` → 回 `0x83`(`ScriptAck`)，运行中拒绝操控报文 |
| 掉电存储 | 1024 字节脚本区 + 2 字节 seed + 2 字节 LED 设置，单个 NVS blob |
| 虚拟机 | 完整 ECS 指令集：按键/摇杆动作、Wait/For/Next/Break/Continue/Return/Call/跳转、寄存器与比较 |

### 相对官方 `EasyMCU_ESP32` 顺手修掉的 4 个问题

1. **`_v` 被写成 `bool`** —— 官方 ESP32 移植里这个算术临时变量是 `bool`，
   会让 `Mul/Div/Mod/And/Or/Xor`、`Break/Continue` 的层数、移位次数全部退化成 0/1。
   本移植改回 `uint8_t`。
2. **缺 MOV 指令分支** —— 官方移植漏了寄存器 MOV 的一条路径，本移植补齐。
3. **数据栈按 8 位压栈** —— 官方移植把 16 位值当 8 位压，高位丢失。本移植按 16 位存取。
4. **FOR 循环栈对齐** —— 官方把 FOR 栈基址放在 `290`，在 ESP32 上对 `int32_t` 是**非对齐访问**
   （部分场景会异常）。本移植移到 `296`，4 字节对齐。
   另外给三个栈都补了深度上限检查，畸形脚本不会越界写坏内存。

---

## 9. 两个特意加的保护

### 9.1 `0x90` 冲突

原协议每发一帧 HID 报文就回一个 `0x90`(`RESP_USB_ACK`)。
EasyCon 的包解析只看 MSB 分帧，`0x90` 会被它当成「一个长度为 1 的控制包」，
从而错位。所以 **`0x90` 现在只在原协议模式下发**。

### 9.2 上位机静默看门狗（防卡键）

拔掉串口线、或 EasyCon 进程崩溃时，固件会一直停在最后一帧输入上，
Switch 上就会「卡键 / 一直朝一个方向走」。现在：

> 超过 **12 秒**收不到任何串口字节，**且没有脚本在跑** → 自动全部松手、摇杆回中，
> 并交还控制权给原协议。

- 12 秒是根据 EasyCon 上位机 **5 秒心跳**取的 2 倍余量，在线期间不会误触发；
- **脚本在跑时不松手**，因为片上脚本本来就设计成可以脱离上位机独立运行；
- 上位机重连时的心跳会让固件自动重新进入 EasyCon 模式，能自愈。

---

## 10. 验证情况

### 10.1 真机编译 —— ✅ 已通过

已在 Windows 上用 MSYS2 + `xtensa-esp32-elf-gcc 5.2.0` + `nullstalgia/esp-idf` master
**完整编译成功**（`make` 返回 0）：

| 检查项 | 结果 |
|---|---|
| 编译 | **成功**，无错误 |
| 新增代码告警 | **`easycon.c` / `easycon_storage.c` 零告警**（`-Wall -Werror` 下通过） |
| 全工程告警 | 仅 3 条，全部与本次改动无关（2 条来自上游原有 `main.c:1118/1122` 的 `set_bt_address()`，1 条是 gcc 不认 `-Wno-frame-address`） |
| 链接 | 成功，生成 `OpenSwitchPad.elf` + `OpenSwitchPad.bin`（564,704 B） |
| 符号核验 | 用 `xtensa-esp32-elf-nm` 逐一确认符号**真实存在于 ELF 中**（见下） |

`nm` 确认存在的新增符号：

```
easycon_init / easycon_apply / easycon_proto_active / EasyCon_serial_task
EasyCon_tick / EasyCon_report_send_callback / EasyCon_script_start/stop
switchcon_set_buttons / set_left_stick / set_right_stick / is_connected / data_blink
easycon_storage_*        （全部为 T/D/B 段真实定义，非未定义引用）
```

对应目标文件：`build/main/easycon.o`（79,956 B）、`easycon_storage.o`（15,232 B）、`main.o`（79,356 B）。

内存占用（`make size`）：

```
DRAM .data        11,672 B      Used static DRAM  57,272 B  (46.0%)
DRAM .bss         45,600 B      Used static IRAM  70,424 B  (53.7%)
Flash code       424,586 B      Flash rodata      57,900 B
Total image    ~ 564,582 B      （app 分区 1 MB，占用 53.9%）
```

> 编译环境与成品固件见 §6；烧录说明见 `source/firmware/dist/FLASH.md`。

### 10.2 行为等价仿真（编译前做的，脚本在 `tmp/eztest/`）

工具链到位之前，为了不靠「看起来对」交付，先用 Python 写了两套等价验证：

#### `verify_easycon.py` — 行为等价仿真

| 部分 | 内容 | 结果 |
|---|---|---|
| A | 报文的 7bit 打包/解包**往返测试**（上位机算法 ↔ `EasyCon_serial_task()` 解包表达式逐行等价翻译） | **20004 组随机用例，0 失败** |
| B | 串口状态机时序：心跳、按键应答(ACK/ERROR/BUSY)、版本、LED、57 字节分 3 包烧录、脚本启停、运行中拒绝操控、未知命令 | 全部应答字节与上位机期望一致 |
| C | Flash 包地址/长度 7bit 拆分解码 | 6 组边界用例通过 |
| D | 静默看门狗：边界值、心跳期间不误触发、真掉线按时触发、脚本运行时不松手、`uint32` 回绕 | 6 组用例通过 |

#### `static_check.py` — 静态一致性

对 6 个 C 文件做括号/`#if` 配平，并逐个核对**每个桥接函数与对外 API 都有定义**：

```
OK switchcon_set_buttons / set_left_stick / set_right_stick / reset_inputs /
   is_connected / data_blink        → main.c
OK easycon_init / EasyCon_serial_task / EasyCon_tick / EasyCon_report_send_callback /
   EasyCon_script_* / set_buttons / press_buttons / release_buttons /
   set_HAT_switch / set_left_stick / set_right_stick / reset_hid_report
   / EasyCon_read_* / EasyCon_write_* / easycon_storage_*   → easycon.c / easycon_storage.c
静态检查: 通过
```

### 另外做的交叉核对

- 命令字/应答字与 EasyCon 上位机 `EzDvCommand` / `Reply` 枚举**逐字节比对一致**；
- 16 位按键位序与官方 `EasyConAPI/HID.h` 的 `JoystickButtons_t` **完全一致**；
- HAT 常量与官方 `HatButton` 一致；
- `easycon_apply()` 写出的 `but1/but2/but3` 位序，与固件**自己**原协议 vanilla 分支的
  位序**逐位比对一致**（Pro 手柄模式），保证 EasyCon 和 AutoSplatoon 走的是同一条出口；
- `UART0/1/2` 引脚占用排查：固件只额外用了 `GPIO12`（板载 LED），
  选的 `GPIO19/GPIO26` 无冲突；
- 调用顺序排查：`easycon_init()` 在 `nvs_flash_init()` 与 `xSemaphoreCreateMutex()` **之后**，
  `send_task`（唯一调用 `send_buttons()` 的任务）在蓝牙连接回调里才创建 —— 顺序安全；
- 锁顺序排查：`s_apply_lock` → `xSemaphore` 单向，`EasyCon_report_send_callback()` 不持锁，
  无 ABBA 死锁。

> **仍需在真机上做的验收**（编译已通过，剩下的是硬件相关）：
> ① 用协议握手自检（`FF 33 CC` → `FF CC 33`，见 §6）；② 连 NS2 正常（原有能力未回归）；
> ③ EasyCon 连上后按键/摇杆正确；④ 若摇杆上下颠倒，把 `EASYCON_Y_FLIP` 改 0；
> ⑤ 拔掉串口线 12 秒后确认自动松手。

---

## 11. 排错

| 现象 | 排查方向 |
|---|---|
| EasyCon 连不上（一直转圈） | 波特率必须是 **115200**；COM 口选对；`EASYCON_UART_SWAP=1` 时是不是忘了关 IDF 串口日志 |
| 按键全都不对 | EasyCon 里的**手柄类型**是否和 `CONTROLLER_TYPE` 一致 |
| 摇杆上下颠倒 | `EASYCON_Y_FLIP` 改 0 |
| 只有部分按键没反应 | 用的是 `JOYCON_L/JOYCON_R`，单个 Joy-Con 物理上没有那些键 → 换 `PRO_CON` |
| AutoSplatoon 连不上 | 检查是否落在 UART2 上（`EASYCON_UART_SWAP=1` 时）；接线要 TX↔RX |
| 脚本烧进去不跑 | 脚本 > 1024 字节会被截断；烧录后需重新上电或发一次启动命令 |
| Switch 上卡键 | 已由静默看门狗兜底；若 12 秒太久可调小 `EASYCON_IDLE_RELEASE_MS` |

---

## 12. 蓝牙配置的重大修复（IDF v3.0 → master 迁移踩坑）

> 这一节记录一个**与 EasyCon 移植无关、但会让人误判成"移植把蓝牙搞坏了"**的坑。

### 12.1 现象

移植完成后，原协议握手正常（`FF 33 CC` → `FF CC 33`）、EasyCon 也能连上，
**但 Switch（NS1/NS2）怎么都连不上**。

由于 `CONFIG_LOG_DEFAULT_LEVEL=0` 把日志全编译掉了，串口一片安静，只能靠猜。
打开日志（见 `dist/diag/`）后立刻看到：

```
I (956) BTDM_INIT: BT controller compile version [484c057]
E (963) app_main: enable controller failed: ESP_ERR_INVALID_ARG
```

`app_main` 在这里 `return`，后面注册 HID、设置可发现**全都没执行**——蓝牙等于没开。

### 12.2 根因：两个新增 Kconfig 符号被补成了错误默认值

工程自带的 `sdkconfig` 是 **ESP-IDF v3.0 时代**生成的，而这个 fork 必须用 **`master`（v4.0-dev）** 编译。
从 v3.0 到 v4.0-dev，`components/bt/Kconfig` 变动了 1300+ 行，新增了一批符号；
旧 `sdkconfig` 里没有它们，非交互编译时 `olddefconfig` 只能补默认值——偏偏这两个默认值是错的：

| 符号 | 为什么错 | 补成的值 | 应该的值 |
|---|---|---|---|
| `BTDM_CONTROLLER_MODE`（choice） | 该 choice **没有写 `default`**，Kconfig 规则取**第一个选项** = `BLE Only` | `CONFIG_BTDM_CONTROLLER_MODE_BLE_ONLY=y` | `CONFIG_BTDM_CONTROLLER_MODE_BR_EDR_ONLY=y` |
| `BT_HID_DEV_ENABLED`（bool） | fork 新增，`default n` | 未定义（`n`） | `CONFIG_BT_HID_DEV_ENABLED=y` |

**第一处**：`BT_CONTROLLER_INIT_CONFIG_DEFAULT()` 用 `BTDM_CONTROLLER_MODE_EFF` 初始化 `bt_cfg.mode`：

```c
#if defined(CONFIG_BTDM_CONTROLLER_MODE_BLE_ONLY)
#define BTDM_CONTROLLER_MODE_EFF   ESP_BT_MODE_BLE        // ← 命中这里
#elif defined(CONFIG_BTDM_CONTROLLER_MODE_BR_EDR_ONLY)
#define BTDM_CONTROLLER_MODE_EFF   ESP_BT_MODE_CLASSIC_BT
```

于是控制器按 BLE 初始化，而 `main.c` 请求 `esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)`，
**模式不匹配 → 返回 `ESP_ERR_INVALID_ARG`**。

**第二处**：`bt_target.h` 只在开关打开时才定义 HID 宏：

```c
#if CONFIG_BT_HID_DEV_ENABLED
#define HID_DEV_INCLUDED            TRUE
#define BTA_HD_INCLUDED             TRUE
#endif
```

为 `n` 时整个 `btc_hd.cc` 编译成空壳，`BTC_PID_HD` 消息没有处理者，
调 `esp_hid_device_register_app()` 时跳到 NULL → `InstrFetchProhibited`（PC=0）→ 重启循环。

### 12.3 修复

`firmware/sdkconfig` 里这三行（**重新编译前务必确认没被改回**）：

```
CONFIG_BTDM_CONTROLLER_MODE_BR_EDR_ONLY=y
CONFIG_CLASSIC_BT_ENABLED=y
CONFIG_BT_HID_DEV_ENABLED=y
```

> 注意：`CONFIG_CLASSIC_BT_ENABLED` 本来就是 `y`（v3.0 时代就有这个符号），
> 但 `BT_HID_DEV_ENABLED` 的 `depends on CLASSIC_BT_ENABLED`，所以两者必须同时为 `y`。

### 12.4 验证（真机日志）

修复后，开机日志变成：

```
I (947) BTDM_INIT: BT controller compile version [484c057]
I (1066) phy: phy_version: 4100, ...                     ← 控制器真的启用了（修复前没有这行）
I (2146) app_main: setting hid parameters
I (2153) app_main: starting hid device
I (2155) application_cb: app is now registered!
I (2169) app_main: setting to connectable, discoverable
I (3112) tag: authentication success: Nintendo Switch    ← 配对成功
I (3143) connection_cb: connected to bc:89:a6:56:f9:87
I (3149) send_task: Sending hid reports on core 0
I (3212) intr_data_cb: reply02                           ← Switch 子命令正常应答
...
```

复位 1 次、panic 0 次，Switch 稳定在线。

### 12.5 顺带纠正：README 说的分支是错的

仓库 `README` 让人用 `release/v3.0`，**这是错的**：

- fork 的 `release/v3.0` 分支里**没有 `esp_hidd_api.h`**，编到一半就 `fatal error`；
- 只有 fork 的 **`master`**（`v4.0-dev-466-gc7ca88081`）才有 `feature/btclassic-hid-device` 合并进来的 HID Device 支持。

而 `master` 正是引入了上述两个新符号的分支——**坑就出在这个"必须用 master"与"旧 sdkconfig"的组合上**。

> 另外上游 IDF 自身还有一个真实 bug：`make/ldgen.mk` 的 Windows 分支把目标名误写成
> `ldgen.section_infos`（全工程无人引用），导致 `No rule to make target '.../build/ldgen_libraries'`。
> 已改为 `ldgen_libraries`。

---

## 附：文件一览

```
ESP32/source/firmware/main/
├── main.c                 ← 改：接入桥接、放行条件、初始化   (1322 行)
├── easycon_config.h       ← 新：集中配置                     (107 行)
├── easycon.h              ← 新：协议常量与接口               (153 行)
├── easycon.c              ← 新：状态机 + 虚拟机 + 翻译       (1098 行)
├── easycon_storage.c      ← 新：NVS 掉电存储                 (132 行)
└── switchcon_bridge.h     ← 新：桥接接口声明                 (42 行)
```

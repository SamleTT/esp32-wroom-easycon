# ESP32 WROOM · EasyCon 无线手柄固件

把一块 **ESP32-WROOM-32** 开发板变成 Switch / Switch 2 的 **Pro Controller**：

- 🔵 **蓝牙**直连主机，当无线手柄用（Switch 1 / Switch 2 均可）；
- 🔌 电脑上的 **EasyCon** 通过**同一根 USB 线**（串口）控制主机；
- ✅ **只需要一根 USB 线**，不需要额外买 USB-TTL 转接板。

本项目基于 [UARTSwitchCon](https://github.com/nullstalgia/UARTSwitchCon)（GPL-3.0），
在其 ESP32 版本（原项目称为 *OpenSwitchPad*）上移植了 **EasyCon 串口协议**，
并修复了蓝牙控制器从未启动的问题（见下文「踩坑记录」）。

> 上游 README 原文见 [`README-upstream.md`](README-upstream.md)。

---

## 一、直接下载刷机包

👉 **前往 [Releases 页面](https://github.com/SamleTT/esp32-wroom-easycon/releases/latest)
下载 `esp32-wroom-EasyCon-USB-flasher-v1.0.0.zip`**（约 19.5 MB）

| 步骤 | 操作 |
|---|---|
| 1 | 解压整个 zip |
| 2 | 双击 `一键烧录.bat` |
| 3 | 按提示选串口（只有一个会自动选） |
| 4 | 看到「[成功] 烧录完成」即可 |

- 包内**内置便携版 Python 3.12 + esptool 5.5.0**，接收方**不需要装任何东西**，
  也不用装 ESP-IDF；
- 详细说明、常见问题、EasyCon 连接参数都在包内的 `使用说明.txt`；
- 校验：zip 的 sha256 为
  `418ebdcad42995ccd5c42a8e253e96ca99f87bd582cf6dc6e43732baee999c70`。

> ⚠️ 合并固件（`*-merged-0x0.bin`）会覆盖 NVS 区，因此**每刷一次蓝牙地址都会重新生成，
> Switch 需要重新配对一次**。想保留地址请改用分文件烧录（不碰 NVS，见 `FLASH.md`）。

### 烧录后的配对方法

1. Switch 主机进入 **手柄 → 更改握法/顺序**，保持该界面不要退出；
2. 按一下开发板上的 **EN / RST** 复位键（或重新插拔 USB）；
3. 等 2~5 秒，主机会识别出一个 **Pro Controller**。

### EasyCon 连接参数

| 项 | 值 |
|---|---|
| 端口 | 开发板对应的 COM 口（与烧录同一个） |
| 波特率 | **115200** |
| 手柄类型 | **Pro Controller** |

---

## 二、功能状态

| 功能 | 状态 | 说明 |
|---|---|---|
| 蓝牙连接 Switch / Switch 2 | ✅ 真机验证 | 日志出现 `authentication success: Nintendo Switch` |
| EasyCon 控制（板载 USB） | ✅ | 本仓库 `dist/easycon-on-usb/` 固件 |
| 原 UARTSwitchCon 协议 | ⚠️ 已移位 | 移到 **UART2 (GPIO19/26)**，需外接 USB-TTL |
| 串口日志 | ⚠️ 生产版关闭 | 需要日志请刷 `dist/diag/` 版本 |

> 本固件把 **EasyCon 放在 UART0（板载 USB）**、原 UARTSwitchCon 协议放到 UART2。
> 如果你更需要在板载 USB 上跑 AutoSplatoon，把 `main/easycon_config.h` 里的
> `EASYCON_UART_SWAP` 改回 `0` 重新编译即可。

---

## 三、目录结构

```
.
├── README.md                 本文件
├── README-upstream.md        UARTSwitchCon 上游原始 README
├── LICENSE                   GPL-3.0
├── Protocol.md               原 UARTSwitchCon 串口协议说明
├── AVR/                      原项目的 AVR（Arduino）版本源码
└── ESP32/
    ├── README.md
    ├── EASYCON_PORT.md       ★ EasyCon 协议移植笔记 / 踩坑记录
    └── source/firmware/
        ├── main/             ★ 固件源码
        │   ├── main.c            主逻辑、蓝牙 HID、配对
        │   ├── easycon.c/.h      EasyCon 协议实现
        │   ├── easycon_config.h  ★ 配置开关（UART 归属、轴方向等）
        │   ├── easycon_storage.c EasyCon 存储
        │   └── switchcon_bridge.h
        ├── sdkconfig         ★ 关键编译配置（见下节）
        ├── Makefile
        └── dist/             预编译固件（一键刷机包见 Releases）
            ├── FLASH.md                       烧录详解 / 排错
            ├── OpenSwitchPad-merged-0x0.bin   合并固件（烧 0x0）
            ├── bootloader.bin                 引导（0x1000）
            ├── partitions_singleapp.bin       分区表（0x8000）
            ├── OpenSwitchPad.bin              应用（0x10000）
            ├── diag/                          带日志的诊断版
            └── easycon-on-usb/                EasyCon 走板载 USB 的变体
```

> 一键刷机包体积约 20 MB，放在
> [Releases](https://github.com/SamleTT/esp32-wroom-easycon/releases/latest) 里，
> 不放进仓库。需要重建请使用 `dist/_build_pkg/gen_pkg.py`（需联网）。

---

## 四、从源码编译

### 4.1 ⚠️ sdkconfig 里这三行绝对不能改

```
CONFIG_BTDM_CONTROLLER_MODE_BR_EDR_ONLY=y
CONFIG_CLASSIC_BT_ENABLED=y
CONFIG_BT_HID_DEV_ENABLED=y
```

**为什么**：本工程的 `sdkconfig` 是 ESP-IDF **v3.0 时代**生成的，而编译必须使用
[nullstalgia/esp-idf](https://github.com/nullstalgia/esp-idf) 的 **master (v4.0-dev)**
（v3.0 分支没有 `esp_hidd_api.h`）。master 新增的 Kconfig 符号会被 `make olddefconfig`
补成**错误默认值**：

- `BTDM_CONTROLLER_MODE` 这个 choice 没有写 `default` → 取第一项 = **BLE Only**
  → 控制器按 BLE 初始化，而代码请求 `ESP_BT_MODE_CLASSIC_BT` → `ESP_ERR_INVALID_ARG`，
  **蓝牙根本不启动**；
- `BT_HID_DEV_ENABLED`（fork 新增，`default n`）→ 为 `n` 时 `bt_target.h` 不定义
  `HID_DEV_INCLUDED`，`btc_hd.cc` 编成空壳 → `esp_hid_device_register_app()` 跳 NULL
  → **崩溃重启**（`InstrFetchProhibited`）。

这两个坑都已在真机上复现并修复。

### 4.2 构建环境

| 项 | 值 |
|---|---|
| IDF | `nullstalgia/esp-idf` master（`v4.0-dev`） |
| 构建 | MSYS2 + `make` |
| 烧录 | esptool ≥ 4.x（本项目用 5.5.0） |

```bash
cd ESP32/source/firmware
make -j4
make flash monitor
```

> 注：若在 Windows 上用 IDF v4.0-dev 的 `make`，需给 `make/ldgen.mk` 的 Windows 分支打补丁
> （把 `ldgen.section_infos` 改成 `ldgen_libraries`），细节见 `EASYCON_PORT.md`。

### 4.3 常用配置（`main/easycon_config.h`）

| 宏 | 默认 | 说明 |
|---|---|---|
| `EASYCON_UART_SWAP` | `1` | `1` = EasyCon 走板载 USB（UART0）；`0` = EasyCon 走 UART2 |
| `EASYCON_Y_FLIP` | — | 摇杆上下颠倒时改这个 |

---

## 五、踩坑记录（重要）

以下问题都实际踩过，详见 [`ESP32/EASYCON_PORT.md`](ESP32/EASYCON_PORT.md)：

1. **蓝牙完全不启动** — `BTDM_CONTROLLER_MODE` 被补成 `BLE_ONLY`，见 4.1。
2. **启动即崩溃重启** — `BT_HID_DEV_ENABLED` 未开启，HID 注册跳空指针，见 4.1。
3. **Windows 批处理中文乱码** — `cmd.exe` 按块读取批处理文件，UTF-8 是变长编码，
   字符跨块边界会被截断。**批处理必须存 GBK + `chcp 936`**，不要用 UTF-8 + `chcp 65001`。
4. **打开 ESP32 串口会复位进下载模式** — pyserial 打开串口时 DTR/RTS 都会被置位。
   要落在正常运行模式必须**先放 DTR 再放 RTS**（`dtr=False; rts=True; sleep(0.15); rts=False`），
   顺序反了会进 DOWNLOAD 模式。
5. **合并固件会清 NVS** — 合并镜像从 `0x0` 覆盖到约 `0x8EE60`，而 NVS 在 `0x9000`，
   镜像里该段是 `0xFF` → 相当于擦除蓝牙地址。
6. **镜像内嵌编译时间戳** — 重新编译后 sha256 必变、大小不变，不能拿哈希当可复现判据。

---

## 六、许可证

本项目沿用上游的 **GNU General Public License v3.0**，见 [LICENSE](LICENSE)。

上游项目：[nullstalgia/UARTSwitchCon](https://github.com/nullstalgia/UARTSwitchCon)
（聚合了 [BlueCubeMod](https://github.com/NathanReeves/BlueCubeMod) 与
[wchill/SwitchInputEmulator](https://github.com/wchill/SwitchInputEmulator) 的成果）

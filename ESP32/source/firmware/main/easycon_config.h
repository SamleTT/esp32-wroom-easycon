/*
 * easycon_config.h
 *
 * UARTSwitchCon(ESP32, 蓝牙 HID) + EasyCon 双协议固件 —— 集中配置
 *
 * 本文件把「控制器类型」和「两套协议各用哪个 UART」集中到一处，
 * main.c 与 easycon.c 共用，避免分散修改。
 */
#ifndef __EASYCON_CONFIG_H__
#define __EASYCON_CONFIG_H__

#include "driver/uart.h"
#include "driver/gpio.h"

/* ====================================================================
 * 1. 控制器类型
 * --------------------------------------------------------------------
 * 原 UARTSwitchCon 支持 左 Joy-Con / 右 Joy-Con / Pro 手柄 三种。
 * 编译期常量，直接改下面这一行即可。
 *
 *   JOYCON_L —— 原项目默认值，配对最稳（推荐用于 AutoSplatoon）
 *   PRO_CON  —— 【EasyCon 推荐】按键最全，EasyCon 脚本里
 *               A/B/X/Y/L/R/ZL/ZR/+/−/HOME/CAPTURE/L3/R3/十字键
 *               全部可用，双摇杆独立
 *   JOYCON_R —— 右 Joy-Con
 *
 * 注意：EasyCon 脚本通常按 Pro 手柄的完整按键写，
 *       用单个 Joy-Con 时部分按键（+、HOME、R/RZR、右摇杆等）无法表达。
 * ==================================================================== */
#define PRO_CON    0x03
#define JOYCON_L   0x01
#define JOYCON_R   0x02

/*
 * 默认值说明：
 *   EasyCon 官方参考固件(EasyMCU_ESP32)按【Pro 手柄】上报，
 *   社区脚本也几乎都按 Pro 手柄写，所以本移植把默认值设为 PRO_CON，
 *   做到「烧录后直接就能用 EasyCon」。
 *
 *   若你主要用 AutoSplatoon（原 UARTSwitchCon 上位机），
 *   或更看重配对稳定性（左 Joy-Con 最稳），改成 JOYCON_L 即可。
 */
#ifndef CONTROLLER_TYPE
#define CONTROLLER_TYPE PRO_CON
#endif

/* ====================================================================
 * 2. 两套协议分别挂在哪个 UART
 * --------------------------------------------------------------------
 * 原固件协议（AutoSplatoon / UARTSwitchCon Client）：19200 波特率
 * EasyCon 协议                                  ：115200 波特率
 *
 * 两套协议波特率不同，无法在同一个 UART 上共存，所以各占一个 UART：
 *
 *   EASYCON_UART_SWAP = 0（默认，改动最小）
 *       原协议  -> UART0 (板载 USB-UART，GPIO1/GPIO3)
 *       EasyCon -> UART2 (需要一根额外 USB-TTL，TX=GPIO19, RX=GPIO26)
 *
 *   EASYCON_UART_SWAP = 1（单线上位机，只插一根线）
 *       EasyCon -> UART0 (板载 USB-UART，GPIO1/GPIO3)
 *       原协议  -> UART2 (需要额外 USB-TTL，TX=GPIO19, RX=GPIO26)
 *
 * 两块 ESP32 上的 TX/RX 记得交叉相接（本机 TX -> 上位机 RX）。
 * ==================================================================== */
#define EASYCON_UART_SWAP 0

#if EASYCON_UART_SWAP == 0

  /* 原 UARTSwitchCon 协议 */
  #define LEGACY_UART_NUM      UART_NUM_0
  #define LEGACY_UART_TX_PIN   UART_PIN_NO_CHANGE   /* UART0 默认 GPIO1 */
  #define LEGACY_UART_RX_PIN   UART_PIN_NO_CHANGE   /* UART0 默认 GPIO3 */

  /* EasyCon */
  #define EASYCON_UART_NUM     UART_NUM_2
  #define EASYCON_UART_TX_PIN  GPIO_NUM_19
  #define EASYCON_UART_RX_PIN  GPIO_NUM_26

#else

  /* EasyCon（走板载 USB-UART，理想情况下不用额外接线） */
  #define EASYCON_UART_NUM     UART_NUM_0
  #define EASYCON_UART_TX_PIN  UART_PIN_NO_CHANGE   /* UART0 默认 GPIO1 */
  #define EASYCON_UART_RX_PIN  UART_PIN_NO_CHANGE   /* UART0 默认 GPIO3 */

  /* 原 UARTSwitchCon 协议 */
  #define LEGACY_UART_NUM      UART_NUM_2
  #define LEGACY_UART_TX_PIN   GPIO_NUM_19
  #define LEGACY_UART_RX_PIN   GPIO_NUM_26

#endif

#define EASYCON_UART_BAUD_RATE  115200
#define LEGACY_UART_BAUD_RATE   19200

/* ====================================================================
 * 3. 摇杆 Y 轴方向
 * --------------------------------------------------------------------
 * EasyCon 报文中 LY=0 表示「上」，而本固件内部摇杆变量 lx_send/ly_send
 * 约定 255 表示「上」（与固件原本的 chocolate 模式、原协议路径一致），
 * 所以这里做一次翻转。
 *
 * 实机测试若发现上下颠倒，把下面改成 0 即可（无需改其它代码）。
 * ==================================================================== */
#define EASYCON_Y_FLIP 1

#endif /* __EASYCON_CONFIG_H__ */

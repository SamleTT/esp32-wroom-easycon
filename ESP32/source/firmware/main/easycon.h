/*
 * easycon.h
 *
 * EasyCon（EasyConNS/EasyCon）MCU 侧串口协议 + ECS 脚本虚拟机
 * —— 移植到 UARTSwitchCon(ESP32, 蓝牙 HID) 所需的全部常量与接口。
 *
 * 协议来源（权威）：
 *   EasyConNS/Protocol     通信协议1.md
 *   EasyConNS/EasyConAPI   EasyCon.c / EasyCon.h / EasyCon_API.h / HID.h
 *   EasyConNS/EasyMCU_ESP32S3（官方 ESP32 参考实现）
 *
 * 与官方实现保持一致的要点：
 *   - 串口 115200 8N1
 *   - 命令/报文以「包」为单位：除最后一个字节外最高位为 0，
 *     最后一个字节最高位为 1
 *   - 按键报文 8 字节（7 字节 Report 按 7bit 大端拆包）
 *   - 版本号回 0x46，与 EasyCon 上位机兼容
 */
#ifndef __EASYCON_H__
#define __EASYCON_H__

#include <stdint.h>
#include <stdbool.h>

/* ====================================================================
 * 版本与内存
 * ==================================================================== */
/* 回给上位机 CMD_VERSION 的固件版本。
 * 上位机只要求 0x40 <= ver <= 0x80，0x46 为 EasyConAPI 的 VERSION。 */
#define EASYCON_VERSION      0x46

/* EasyCon 可用的内存 / 掉电存储大小。
 * 脚本从地址 0 开始存，因此这同时也是「最大脚本字节数」。
 * SEED_OFFSET = MEM_SIZE, LED_SETTING = MEM_SIZE+2，所以
 * 掉电存储区需要 MEM_SIZE + 4 字节以上。 */
#define MEM_SIZE             1024

#define SERIAL_BUFFER_SIZE   20
#define ECHO_TIMES           2
#define ECHO_INTERVAL        2

/* ====================================================================
 * 串口控制字 / 应答字（与上位机 EzDvCommand / Reply 一一对应）
 * ==================================================================== */
#define CMD_READY            0xA5
#define CMD_DEBUG            0x80
#define CMD_HELLO            0x81
#define CMD_FLASH            0x82
#define CMD_SCRIPTSTART      0x83
#define CMD_SCRIPTSTOP       0x84
#define CMD_VERSION          0x85
#define CMD_LED              0x86
#define CMD_UNPAIR           0x87
#define CMD_CHGMODE          0x88
#define CMD_CHGCOLOR         0x89
#define CMD_SAV_AMB          0x90

#define REPLY_ERROR          0x00
#define REPLY_ACK            0xFF
#define REPLY_BUSY           0xFE
#define REPLY_HELLO          0x80
#define REPLY_FLASHSTART     0x81
#define REPLY_FLASHEND       0x82
#define REPLY_SCRIPTACK      0x83

/* ====================================================================
 * 摇杆 / 十字键
 * ==================================================================== */
#define STICK_MIN            0
#define STICK_CENTER         128
#define STICK_MAX            255

#define HAT_TOP              0x00
#define HAT_TOP_RIGHT        0x01
#define HAT_RIGHT            0x02
#define HAT_BOTTOM_RIGHT     0x03
#define HAT_BOTTOM           0x04
#define HAT_BOTTOM_LEFT      0x05
#define HAT_LEFT             0x06
#define HAT_TOP_LEFT         0x07
#define HAT_CENTER           0x08
#define HAT_CENTERED         HAT_CENTER   /* 兼容官方源码命名 */

/* ====================================================================
 * EasyCon 16 位按键字（= EasyCon HID.h / JoystickButtons_t）
 * 注意与固件内部 but1/but2/but3 的位序不同，映射在 easycon.c 里做。
 * ==================================================================== */
#define EZ_Y                 (1 << 0)
#define EZ_B                 (1 << 1)
#define EZ_A                 (1 << 2)
#define EZ_X                 (1 << 3)
#define EZ_L                 (1 << 4)
#define EZ_R                 (1 << 5)
#define EZ_ZL                (1 << 6)
#define EZ_ZR                (1 << 7)
#define EZ_MINUS             (1 << 8)
#define EZ_PLUS              (1 << 9)
#define EZ_LCLICK            (1 << 10)
#define EZ_RCLICK            (1 << 11)
#define EZ_HOME              (1 << 12)
#define EZ_CAPTURE           (1 << 13)

/* ====================================================================
 * 对外接口
 * ==================================================================== */

/* 初始化：掉电存储 + UART + EasyCon_script_init + 创建任务。
 * 必须在 nvs_flash_init() 与 gpio_config() 之后调用。 */
void easycon_init(void);

/* 1ms 主时钟 —— 由 easycon 内部的 tick 任务调用 */
void EasyCon_tick(void);

/* 串口状态机 —— 每收到一个字节调用一次 */
void EasyCon_serial_task(int16_t byte);

/* 串口发送一个字节（EasyCon 应答） */
void EasyCon_serial_send(uint8_t byte);

/* 是否到达可以发报文的时刻（配合 ECHO_INTERVAL 防丢键） */
bool EasyCon_need_send_report(void);

/* 每发出一份 HID 报文后调用 */
void EasyCon_report_send_callback(void);

/* 脚本控制 */
void EasyCon_script_init(void);
void EasyCon_script_task(void);
void EasyCon_script_start(void);
void EasyCon_script_stop(void);
void EasyCon_script_auto_start(void);
bool EasyCon_is_script_running(void);

/* HID 报文设置（由虚拟机调用；实现里会翻译到 switchcon_bridge） */
void reset_hid_report(void);
void set_left_stick(const uint8_t LX, const uint8_t LY);
void set_right_stick(const uint8_t RX, const uint8_t RY);
void set_buttons(const uint16_t Button);
void press_buttons(const uint16_t Button);
void release_buttons(const uint16_t Button);
void set_HAT_switch(const uint8_t HAT);

/* 掉电存储（实现在 easycon_storage.c） */
void    easycon_storage_init(void);
void    easycon_storage_tick(void);
uint8_t EasyCon_read_byte(uint16_t addr);
void    EasyCon_read_start(void);
void    EasyCon_write_start(uint8_t mode);
void    EasyCon_write_data(uint16_t addr, uint8_t *data, uint16_t len);
void    EasyCon_write_end(uint8_t mode);
void    easycon_storage_request_flush(void);

#endif /* __EASYCON_H__ */

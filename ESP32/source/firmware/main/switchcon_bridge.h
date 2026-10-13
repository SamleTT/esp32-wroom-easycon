/*
 * switchcon_bridge.h
 *
 * EasyCon 层 <-> UARTSwitchCon 固件层 之间的桥接接口。
 *
 * 实现放在 main.c（因为 send_buttons()/蓝牙上报逻辑都在那里），
 * 由 easycon.c 调用，把 EasyCon 解析出的「统一 Switch 报告」
 * 翻译成主固件内部使用的 but1/but2/but3 + lx/ly/cx/cy 变量。
 */
#ifndef __SWITCHCON_BRIDGE_H__
#define __SWITCHCON_BRIDGE_H__

#include <stdint.h>
#include <stdbool.h>

/* 按键三个字节，含义与原固件完全一致：
 *   but1 : bit0=Y  bit1=X  bit2=B  bit3=A  bit4=SR bit5=SL bit6=R  bit7=ZR
 *   but2 : bit0=-  bit1=+  bit2=Rs bit3=Ls bit4=H  bit5=Cap
 *   but3 : bit0=D  bit1=U  bit2=R  bit3=L  bit4=SR bit5=SL bit6=L  bit7=ZL
 */
void switchcon_set_buttons(uint8_t but1, uint8_t but2, uint8_t but3);

/* 左摇杆，lx/ly 采用固件内部约定：0~255，中心 128，ly 越大越靠「上」 */
void switchcon_set_left_stick(uint8_t lx, uint8_t ly);

/* 右摇杆，同上 */
void switchcon_set_right_stick(uint8_t rx, uint8_t ry);

/* 全部按键与摇杆回中、松开 */
void switchcon_reset_inputs(void);

/* 蓝牙主机（Switch）是否已连接/已配对 */
bool switchcon_is_connected(void);

/* 数据指示灯闪一下（非阻塞）。当前实现为空操作，避免和连接状态灯打架 */
void switchcon_data_blink(void);

/* 由 easycon.c 定义：EasyCon 协议是否已经真正在工作。
 * main.c 的发送循环用它来放行「真实报文」（否则只发 dummy 报文）。 */
extern volatile bool easycon_proto_active;

#endif /* __SWITCHCON_BRIDGE_H__ */

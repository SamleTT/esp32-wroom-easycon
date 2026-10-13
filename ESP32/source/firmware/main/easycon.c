/*
 * easycon.c
 *
 * 把 EasyCon（EasyConNS）的 MCU 侧串口协议与 ECS 脚本虚拟机移植到
 * UARTSwitchCon(ESP32，蓝牙 HID) 固件上，使 EasyCon 上位机可以直接
 * 驱动这个「能连 NS1 / NS2」的 ESP32 手柄固件。
 *
 * 数据流：
 *
 *   EasyCon 上位机 ──UART(115200)──> easycon_rx_task
 *        └─> EasyCon_serial_task()
 *              ├─ 命令字 → 应答 / 脚本烧录 / 脚本启停
 *              └─ 按键报文(8B) → set_buttons/set_left_stick/...
 *                                   └─ easycon_apply()  翻译成
 *                                      switchcon_set_buttons()/... 写进主固件
 *                                      but1/but2/but3/lx/ly/cx/cy
 *
 *   主固件 send_task ──(每 15ms)──> send_buttons() ──蓝牙 HID──> Switch
 *
 * ECS 脚本由 easycon_script_pump 任务解释执行（1ms 一拍），
 * 直接操作同一份影子状态，因此「上位机实时操控」与「固件内跑脚本」
 * 共用同一条出口。
 *
 * 说明：本移植以 EasyConNS/Protocol 与 EasyConNS/EasyConAPI 的 AVR 参考
 * 实现为语义基准，并修正了官方 ESP32-S3 参考实现中的若干缺陷
 * （_v 误用 bool、MOV 指令缺失、16 位栈被压成 8 位、int16 截断语义等）。
 */
#include "easycon.h"
#include "easycon_config.h"
#include "switchcon_bridge.h"

#include <string.h>
#include <stdlib.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "easycon";

/* ====================================================================
 * 串口状态机 / 脚本运行状态
 * ==================================================================== */
static uint8_t mem[MEM_SIZE] __attribute__((aligned(4)));

static uint8_t serial_buffer[SERIAL_BUFFER_SIZE];
static size_t  serial_buffer_length = 0;
static bool    serial_command_ready = true;   /* 与官方实现一致：上电即 Ready */

static uint16_t flash_addr  = 0;   /* 本次烧录起始地址 */
static uint16_t flash_index = 0;   /* 当前已收到多少字节 */
static uint16_t flash_count = 0;   /* 本次需要收到多少字节 */

static uint16_t script_addr    = 0;   /* 下一条指令地址 */
static uint16_t script_eof     = 0;   /* 脚本结束地址 */
static uint16_t tail_wait      = 0;   /* 压缩指令的补时 */
static uint32_t timer_elapsed  = 0;
static bool     auto_run       = false;

static volatile uint8_t  echo_ms         = 0;
static volatile uint8_t  _report_echo    = 0;
static volatile uint8_t  _script_running = 0;
static volatile uint8_t  _ledflag        = 0;
static volatile uint32_t timer_ms        = 0;
static volatile uint32_t wait_ms         = 0;

/* ====================================================================
 * 「统一 Switch 报告」影子状态
 * EasyCon 视角下的一份完整 Pro 手柄报告，最后由 easycon_apply()
 * 依据 CONTROLLER_TYPE 折叠成主固件内部表示。
 * ==================================================================== */
static uint16_t ez_buttons = 0;
static uint8_t  ez_hat = HAT_CENTERED;
static uint8_t  ez_lx = STICK_CENTER, ez_ly = STICK_CENTER;
static uint8_t  ez_rx = STICK_CENTER, ez_ry = STICK_CENTER;

static SemaphoreHandle_t s_apply_lock = NULL;
static SemaphoreHandle_t s_tx_lock    = NULL;

static volatile uint32_t s_rx_reports = 0;

/* 最近一次收到串口字节的毫秒时标（供静默看门狗使用）。
 * EasyCon 上位机每 5s 发一次 Ready,Ready,Hello 心跳，连续 3 次失败（约 15s）
 * 才判定掉线，所以这里取 2 倍余量：12s 收不到任何字节就认为上位机已消失。 */
#define EASYCON_IDLE_RELEASE_MS 12000
static volatile uint32_t s_last_rx_ms = 0;

/* 供 main.c 判断是否放行真实 HID 报文 */
volatile bool easycon_proto_active = false;

/* ====================================================================
 * 虚拟机内存布局（全部为 mem[] 内偏移）
 *
 *   20  + i*2   DX(i) / DY(i)   i=0..31   方向表
 *   90  + i     KEY(i)          i=0..33   按键剩余计时
 *   130 + (i-1)*2  REG(i)       i=1..7    通用寄存器（int16）
 *   180 + i*2   STACK(i)        i=0..24   数据栈（int16）
 *   230 + i*2   CALLSTACK(i)    i=0..29   调用栈（uint16）
 *   296 + i*16  FOR 栈          i=0..6    循环栈
 *
 * 注意 FOR 栈基址取 296（4 字节对齐）而不是官方的 290，
 * 避免在 ESP32 上对 int32 做非对齐访问。
 * ==================================================================== */
#define DX(i)        (mem[20 + (i) * 2])
#define DY(i)        (mem[21 + (i) * 2])
#define KEY(i)       (mem[90 + (i)])
#define REG(i)       (*(int16_t  *)(mem + 130 + ((i) - 1) * 2))
#define STACK(i)     (*(int16_t  *)(mem + 180 + (i) * 2))
#define CALLSTACK(i) (*(uint16_t *)(mem + 230 + (i) * 2))
#define FOR_I(i)     (*(int32_t  *)(mem + 296 + (i) * 16))
#define FOR_ADDR(i)  (*(uint16_t *)(mem + 300 + (i) * 16))
#define FOR_NEXT(i)  (*(uint16_t *)(mem + 302 + (i) * 16))
#define FOR_C(i)     (*(int32_t  *)(mem + 304 + (i) * 16))

#define KEYCODE_MAX      33
#define VARSPACE_OFFSET  130
#define SEED_OFFSET      (MEM_SIZE)
#define LED_SETTING      (MEM_SIZE + 2)

/* 各栈深度上限（防止畸形脚本越界写坏内存） */
#define STACK_MAX        25
#define CALLSTACK_MAX    30
#define FORSTACK_MAX     7

static uint8_t  _ins0, _ins1, _ins2, _ins3;
static uint16_t _ins_word;
static uint32_t _ins_dword;
static uint16_t _addr;
static uint8_t  _keycode, _lr, _direction;
static int32_t  _forstackindex  = 0;
static int32_t  _callstackindex = 0;
static int32_t  _stackindex     = 0;
static uint16_t _e_val  = 0;
static int16_t  _ri0, _ri1;
/* 注意：官方 ESP32 port 把 _v 误写成 bool，会让 Mul/Div/Mod/And/Or/Xor
 * 以及 Break/Continue 的层数、移位次数全部退化成 0/1。这里用 uint8_t。 */
static uint8_t  _v      = 0;
static bool     _flag   = false;
static uint16_t _seed   = 0;

#define E_SET       (_e_val != 0)
#define E(v)        (_e_val = (v))
#define _ins        (_ins_word)
#define _insEx      (_ins_dword)
#define _BV(i)      (1 << (i))
#define Min(a, b)   ((a) < (b) ? (a) : (b))
#define Max(a, b)   ((a) > (b) ? (a) : (b))
#define SETWAIT(n)  do { wait_ms = (uint32_t)(n); } while (0)
#define JUMP(a)     do { script_addr = (uint16_t)(a); } while (0)
#define JUMPNEAR(o) do { script_addr = (uint16_t)(script_addr + (o)); } while (0)
#define RESETAFTER(keycode, n) do { KEY(keycode) = (n); } while (0)

static void binaryop(uint8_t op, uint8_t reg, int16_t value);

/* ====================================================================
 * 摇杆 Y 轴换算
 * EasyCon: 0=上 128=中 255=下
 * 固件内部: 255=上 128=中 0=下
 * ==================================================================== */
#if EASYCON_Y_FLIP
#define EZ_LY2HID(y) ((uint8_t)(255 - (y)))
#else
#define EZ_LY2HID(y) ((uint8_t)(y))
#endif

/* ====================================================================
 * 把影子状态翻译成主固件内部状态
 * ==================================================================== */
static void easycon_apply(void)
{
    /* 十字键 */
    bool d = false, u = false, r = false, l = false;
    switch (ez_hat) {
        case HAT_TOP:          u = true; break;
        case HAT_TOP_RIGHT:    u = true; r = true; break;
        case HAT_RIGHT:        r = true; break;
        case HAT_BOTTOM_RIGHT: d = true; r = true; break;
        case HAT_BOTTOM:       d = true; break;
        case HAT_BOTTOM_LEFT:  d = true; l = true; break;
        case HAT_LEFT:         l = true; break;
        case HAT_TOP_LEFT:     u = true; l = true; break;
        default:               break;              /* HAT_CENTERED */
    }

    /* 统一摇杆，采用固件内部约定（LY 越大越靠上） */
    uint8_t hidLX = ez_lx;
    uint8_t hidLY = EZ_LY2HID(ez_ly);
    uint8_t hidRX = ez_rx;
    uint8_t hidRY = EZ_LY2HID(ez_ry);

    uint8_t but1 = 0, but2 = 0, but3 = 0;
    uint8_t lx = STICK_CENTER, ly = STICK_CENTER;
    uint8_t rx = STICK_CENTER, ry = STICK_CENTER;

#if CONTROLLER_TYPE == PRO_CON
    /* ---------------- Pro 手柄：与 EasyCon 一一对应 ---------------- */
    but1 = (uint8_t)((((ez_buttons & EZ_Y ) ? 1 : 0) << 0) |
                     (((ez_buttons & EZ_X ) ? 1 : 0) << 1) |
                     (((ez_buttons & EZ_B ) ? 1 : 0) << 2) |
                     (((ez_buttons & EZ_A ) ? 1 : 0) << 3) |
                     (((ez_buttons & EZ_R ) ? 1 : 0) << 6) |
                     (((ez_buttons & EZ_ZR) ? 1 : 0) << 7));

    but2 = (uint8_t)((((ez_buttons & EZ_MINUS  ) ? 1 : 0) << 0) |
                     (((ez_buttons & EZ_PLUS   ) ? 1 : 0) << 1) |
                     (((ez_buttons & EZ_RCLICK ) ? 1 : 0) << 2) |
                     (((ez_buttons & EZ_LCLICK ) ? 1 : 0) << 3) |
                     (((ez_buttons & EZ_HOME   ) ? 1 : 0) << 4) |
                     (((ez_buttons & EZ_CAPTURE) ? 1 : 0) << 5));

    but3 = (uint8_t)(((d ? 1 : 0) << 0) | ((u ? 1 : 0) << 1) |
                     ((r ? 1 : 0) << 2) | ((l ? 1 : 0) << 3) |
                     (((ez_buttons & EZ_L ) ? 1 : 0) << 6) |
                     (((ez_buttons & EZ_ZL) ? 1 : 0) << 7));

    lx = hidLX; ly = hidLY;
    rx = hidRX; ry = hidRY;

#elif CONTROLLER_TYPE == JOYCON_L
    /* ------------- 单个左 Joy-Con（可横持）：折叠映射 ------------- */
    /* 单摇杆来源优先级：十字键 > 左摇杆 > 右摇杆（与固件 chocolate 一致） */
    uint8_t sx = STICK_CENTER, sy = STICK_CENTER;
    if (d || u || l || r) {
        sx = r ? 255 : (l ? 0 : STICK_CENTER);
        sy = u ? 255 : (d ? 0 : STICK_CENTER);
    } else if (hidLX != STICK_CENTER || hidLY != STICK_CENTER) {
        sx = hidLX; sy = hidLY;
    } else if (hidRX != STICK_CENTER || hidRY != STICK_CENTER) {
        sx = hidRX; sy = hidRY;
    }
    /* 横持左 Joy-Con 的摇杆换算（照抄固件 JOYCON_L chocolate 分支） */
    lx = sy;
    ly = (uint8_t)(255 - sx);

    /* 面键 A/B/X/Y 与十字键合并成 4 个方向位（单 Joy-Con 只有 4 个面键） */
    bool dir_d = d || (ez_buttons & EZ_B);
    bool dir_u = u || (ez_buttons & EZ_X);
    bool dir_r = r || (ez_buttons & EZ_A);
    bool dir_l = l || (ez_buttons & EZ_Y);

    but3 = (uint8_t)(((dir_d ? 1 : 0) << 0) | ((dir_u ? 1 : 0) << 1) |
                     ((dir_r ? 1 : 0) << 2) | ((dir_l ? 1 : 0) << 3) |
                     (((ez_buttons & EZ_L ) ? 1 : 0) << 6) |
                     (((ez_buttons & EZ_ZL) ? 1 : 0) << 7));
    /* 横持时 SL/SR 随 L/ZL 一起按下，方便侧握使用 */
    if (ez_buttons & (EZ_L | EZ_ZL)) {
        but3 |= (1 << 4) | (1 << 5);
    }
    /* 左 Joy-Con 只有 −、Capture、L3 */
    but2 = (uint8_t)((((ez_buttons & EZ_MINUS  ) ? 1 : 0) << 0) |
                     (((ez_buttons & EZ_LCLICK ) ? 1 : 0) << 3) |
                     (((ez_buttons & EZ_CAPTURE) ? 1 : 0) << 5));

#else /* CONTROLLER_TYPE == JOYCON_R */
    /* ------------- 单个右 Joy-Con：折叠映射 ------------- */
    uint8_t sx2 = STICK_CENTER, sy2 = STICK_CENTER;
    if (d || u || l || r) {
        sx2 = r ? 255 : (l ? 0 : STICK_CENTER);
        sy2 = u ? 255 : (d ? 0 : STICK_CENTER);
    } else if (hidRX != STICK_CENTER || hidRY != STICK_CENTER) {
        sx2 = hidRX; sy2 = hidRY;
    } else if (hidLX != STICK_CENTER || hidLY != STICK_CENTER) {
        sx2 = hidLX; sy2 = hidLY;
    }
    /* 照抄固件 JOYCON_R chocolate 分支 */
    rx = (uint8_t)(255 - sy2);
    ry = sx2;

    bool dir_d2 = d || (ez_buttons & EZ_B);
    bool dir_u2 = u || (ez_buttons & EZ_X);
    bool dir_r2 = r || (ez_buttons & EZ_A);
    bool dir_l2 = l || (ez_buttons & EZ_Y);

    but1 = (uint8_t)(((dir_l2 ? 1 : 0) << 0) |   /* Y */
                     ((dir_u2 ? 1 : 0) << 1) |   /* X */
                     ((dir_d2 ? 1 : 0) << 2) |   /* B */
                     ((dir_r2 ? 1 : 0) << 3) |   /* A */
                     (((ez_buttons & (EZ_R | EZ_ZR)) ? 1 : 0) << 4) | /* SR */
                     (((ez_buttons & (EZ_R | EZ_ZR)) ? 1 : 0) << 5) | /* SL */
                     (((ez_buttons & EZ_R ) ? 1 : 0) << 6) |
                     (((ez_buttons & EZ_ZR) ? 1 : 0) << 7));
    /* 右 Joy-Con 只有 +、HOME、R3 */
    but2 = (uint8_t)((((ez_buttons & EZ_PLUS  ) ? 1 : 0) << 1) |
                     (((ez_buttons & EZ_RCLICK) ? 1 : 0) << 2) |
                     (((ez_buttons & EZ_HOME  ) ? 1 : 0) << 4));
#endif

    switchcon_set_buttons(but1, but2, but3);
    switchcon_set_left_stick(lx, ly);
    switchcon_set_right_stick(rx, ry);
}

/* ====================================================================
 * HID 报文设置接口（EasyCon API 要求实现）
 * ==================================================================== */
void set_buttons(const uint16_t Button)
{
    if (s_apply_lock) xSemaphoreTake(s_apply_lock, portMAX_DELAY);
    ez_buttons = Button;
    easycon_apply();
    if (s_apply_lock) xSemaphoreGive(s_apply_lock);
}

void press_buttons(const uint16_t Button)
{
    if (s_apply_lock) xSemaphoreTake(s_apply_lock, portMAX_DELAY);
    ez_buttons |= Button;
    easycon_apply();
    if (s_apply_lock) xSemaphoreGive(s_apply_lock);
}

void release_buttons(const uint16_t Button)
{
    if (s_apply_lock) xSemaphoreTake(s_apply_lock, portMAX_DELAY);
    ez_buttons &= (uint16_t)~Button;
    easycon_apply();
    if (s_apply_lock) xSemaphoreGive(s_apply_lock);
}

void set_HAT_switch(const uint8_t HAT)
{
    if (s_apply_lock) xSemaphoreTake(s_apply_lock, portMAX_DELAY);
    ez_hat = HAT;
    easycon_apply();
    if (s_apply_lock) xSemaphoreGive(s_apply_lock);
}

void set_left_stick(const uint8_t LX, const uint8_t LY)
{
    if (s_apply_lock) xSemaphoreTake(s_apply_lock, portMAX_DELAY);
    ez_lx = LX; ez_ly = LY;
    easycon_apply();
    if (s_apply_lock) xSemaphoreGive(s_apply_lock);
}

void set_right_stick(const uint8_t RX, const uint8_t RY)
{
    if (s_apply_lock) xSemaphoreTake(s_apply_lock, portMAX_DELAY);
    ez_rx = RX; ez_ry = RY;
    easycon_apply();
    if (s_apply_lock) xSemaphoreGive(s_apply_lock);
}

void reset_hid_report(void)
{
    if (s_apply_lock) xSemaphoreTake(s_apply_lock, portMAX_DELAY);
    ez_buttons = 0;
    ez_hat = HAT_CENTERED;
    ez_lx = ez_ly = ez_rx = ez_ry = STICK_CENTER;
    easycon_apply();
    if (s_apply_lock) xSemaphoreGive(s_apply_lock);
}

/* ====================================================================
 * 串口发送
 * ==================================================================== */
void EasyCon_serial_send(uint8_t byte)
{
    if (s_tx_lock) xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uart_write_bytes(EASYCON_UART_NUM, (const char *)&byte, 1);
    if (s_tx_lock) xSemaphoreGive(s_tx_lock);
    switchcon_data_blink();
}

/* ====================================================================
 * 1ms 主时钟
 * ==================================================================== */
void EasyCon_tick(void)
{
    timer_ms++;
    if (echo_ms != 0) echo_ms--;
    if (wait_ms != 0 && (_report_echo == 0 || wait_ms > 1)) wait_ms--;

    /* ==== 上位机静默看门狗 ====
     * 拔掉串口线、或上位机进程崩溃时，EasyCon 不会再发心跳。
     * 此时若还保持 EasyCon 模式，影子报文会一直停在最后一次输入上，
     * Switch 上就会出现「卡键 / 一直朝一个方向走」。
     * 这里在上位机静默且没有脚本在跑时自动松手，并把控制权交还原协议。
     * 脚本在跑时不松手，因为脚本本来就设计成可以脱离上位机独立运行。 */
    if (easycon_proto_active && !_script_running &&
        (uint32_t)(timer_ms - s_last_rx_ms) > EASYCON_IDLE_RELEASE_MS) {
        easycon_proto_active = false;   /* 让 send_buttons() 回到原协议分支 */
        reset_hid_report();             /* 全部按键松开、摇杆回中 */
    }
}

bool EasyCon_need_send_report(void)
{
    return (echo_ms == 0);
}

void EasyCon_report_send_callback(void)
{
    if (!_script_running || _report_echo > 0 || wait_ms < 2) {
        _report_echo = (uint8_t)Max(0, (int)_report_echo - 1);
    }
    echo_ms = ECHO_INTERVAL;
}

bool EasyCon_is_script_running(void)
{
    return _script_running == 1;
}

/* ====================================================================
 * 脚本初始化 / 启动 / 停止
 * ==================================================================== */
void EasyCon_script_init(void)
{
    _seed = (uint16_t)EasyCon_read_byte(SEED_OFFSET + 1) << 8 | EasyCon_read_byte(SEED_OFFSET);
    _seed += 1;
    srand(_seed);
    EasyCon_write_start(1);
    EasyCon_write_data(SEED_OFFSET, (uint8_t *)&_seed, 2);
    EasyCon_write_end(1);

    memset(mem, 0, sizeof(mem));

    /* 预置 32 个方向对应的摇杆值（与官方一致） */
    for (int i = 0; i < 16; i++) {
        int x = (Min(i, 8)) << 5;
        int y = (Max(i, 8) - 8) << 5;
        x = Min(x, 255);
        y = Min(y, 255);
        DX(i) = (uint8_t)x;
        DY(i) = (uint8_t)y;
    }
    for (int i = 16; i < 32; i++) {
        int x = (24 - Min(i, 24)) << 5;
        int y = (32 - Max(i, 24)) << 5;
        x = Min(x, 255);
        y = Min(y, 255);
        DX(i) = (uint8_t)x;
        DY(i) = (uint8_t)y;
    }

    _report_echo = ECHO_TIMES;
    _ledflag = EasyCon_read_byte(LED_SETTING);
    /* 存储区第 1 字节 bit7 为 0 且已有脚本时，表示上电自动运行 */
    auto_run = ((EasyCon_read_byte(1) >> 7) == 0) && (EasyCon_read_byte(0) != 0xFF);
}

void EasyCon_script_auto_start(void)
{
    if (auto_run) EasyCon_script_start();
}

void EasyCon_script_start(void)
{
    script_addr = 2;
    uint16_t eof = (uint16_t)(EasyCon_read_byte(0) | (EasyCon_read_byte(1) << 8));
    if (eof == 0xFFFF) eof = 0;
    script_eof = (uint16_t)(eof & 0x7FFF);

    wait_ms = 0;
    echo_ms = 0;
    timer_ms = 0;
    tail_wait = 0;
    _forstackindex = 0;
    _callstackindex = 0;
    _stackindex = 0;
    _flag = false;
    _v = 0;
    _e_val = 0;
    memset(mem + VARSPACE_OFFSET, 0, sizeof(mem) - VARSPACE_OFFSET);
    _script_running = 1;
    _seed = (uint16_t)EasyCon_read_byte(SEED_OFFSET + 1) << 8 | EasyCon_read_byte(SEED_OFFSET);
    ESP_LOGI(TAG, "script start, eof=%u", (unsigned)script_eof);
}

void EasyCon_script_stop(void)
{
    _script_running = 0;
    timer_elapsed = timer_ms;
    reset_hid_report();
    _report_echo = ECHO_TIMES;
    ESP_LOGI(TAG, "script stop");
}

/* ====================================================================
 * ECS 脚本虚拟机
 * ==================================================================== */
void EasyCon_script_task(void)
{
    int32_t n;
    int16_t reg;

    while (1) {
        if (!_script_running) return;
        if (wait_ms > 0) return;

        /* 按键/摇杆「按住时长」倒计时，到期自动松开 */
        for (int i = 0; i <= KEYCODE_MAX; i++) {
            if (KEY(i) != 0) {
                KEY(i)--;
                if (KEY(i) == 0) {
                    if (i == 32) {
                        set_left_stick(STICK_CENTER, STICK_CENTER);
                        _report_echo = ECHO_TIMES;
                    } else if (i == 33) {
                        set_right_stick(STICK_CENTER, STICK_CENTER);
                        _report_echo = ECHO_TIMES;
                    } else if ((i & 0x10) == 0) {
                        release_buttons(_BV(i));
                        _report_echo = ECHO_TIMES;
                    } else {
                        set_HAT_switch(HAT_CENTERED);
                        _report_echo = ECHO_TIMES;
                    }
                }
            }
        }

        if (tail_wait != 0) {
            SETWAIT(tail_wait);
            tail_wait = 0;
            return;
        }

        if (script_addr >= script_eof) {
            EasyCon_script_stop();
            return;
        }

        _addr = script_addr;
        _ins0 = EasyCon_read_byte(script_addr++);
        _ins1 = EasyCon_read_byte(script_addr++);
        _ins_word = (uint16_t)_ins0 | ((uint16_t)_ins1 << 8);

        if (_ins0 & 0x80) {
            /* ---------------- 按键 / 摇杆动作 ---------------- */
            if ((_ins0 & 0x40) == 0) {
                /* 指令：Key */
                _keycode = (uint8_t)((_ins0 >> 1) & 0x1F);
                if ((_keycode & 0x10) == 0) {
                    press_buttons(_BV(_keycode));
                    _report_echo = ECHO_TIMES;
                } else {
                    set_HAT_switch((uint8_t)(_keycode & 0xF));
                    _report_echo = ECHO_TIMES;
                }

                if (E_SET) {
                    SETWAIT(REG(_e_val));
                    RESETAFTER(_keycode, 1);
                } else if ((_ins0 & 0x01) == 0) {
                    n = _ins1;
                    n *= 10;
                    SETWAIT(n);
                    RESETAFTER(_keycode, 1);
                } else if ((_ins1 & 0x80) == 0) {
                    tail_wait = (uint16_t)(_ins1 & 0x7F);
                    tail_wait = (uint16_t)(tail_wait * 50);
                    SETWAIT(50);
                    RESETAFTER(_keycode, 1);
                } else {
                    n = _ins1 & 0x7F;
                    RESETAFTER(_keycode, (uint8_t)n);
                }
            } else {
                /* 指令：Stick */
                _lr = (uint8_t)((_ins0 >> 5) & 1);
                _keycode = (uint8_t)(32 | _lr);
                _direction = (uint8_t)(_ins0 & 0x1F);
                if (_lr) {
                    set_right_stick(DX(_direction), DY(_direction));
                } else {
                    set_left_stick(DX(_direction), DY(_direction));
                }
                _report_echo = ECHO_TIMES;

                if (E_SET) {
                    SETWAIT(REG(_e_val));
                    RESETAFTER(_keycode, 1);
                } else if ((_ins1 & 0x80) == 0) {
                    n = _ins1 & 0x7F;
                    n *= 50;
                    SETWAIT(n);
                    RESETAFTER(_keycode, 1);
                } else {
                    n = _ins1 & 0x7F;
                    RESETAFTER(_keycode, (uint8_t)n);
                }
            }
        } else {
            /* ---------------- 流程控制 ---------------- */
            switch ((_ins0 >> 3) & 0x0F) {

            case 0x00: /* SerialPrint */
                if ((_ins0 & 0x04) != 0) {
                    reg = (int16_t)(_ins & ((1 << 9) - 1));
                    if ((_ins0 & 0x02) == 0) {
                        EasyCon_serial_send((uint8_t)reg);
                        EasyCon_serial_send((uint8_t)(reg >> 8));
                    } else {
                        EasyCon_serial_send(mem[reg]);
                        EasyCon_serial_send(mem[reg + 1]);
                    }
                }
                break;

            case 0x01: /* Wait */
                if (E_SET) {
                    n = REG(_e_val);
                } else if ((_ins0 & 0x04) == 0) {
                    n = _ins & ((1 << 10) - 1);
                    n *= 10;
                } else if ((_ins0 & 0x02) == 0) {
                    _ins2 = EasyCon_read_byte(script_addr++);
                    _ins3 = EasyCon_read_byte(script_addr++);
                    _ins_dword = (uint32_t)_ins0 | ((uint32_t)_ins1 << 8) |
                                 ((uint32_t)_ins2 << 16) | ((uint32_t)_ins3 << 24);
                    n = (int32_t)(_insEx & ((1L << 25) - 1));
                    n *= 10;
                } else {
                    n = _ins & ((1 << 9) - 1);
                }
                SETWAIT(n);
                break;

            case 0x02: /* For */
                if (_forstackindex == 0 || FOR_ADDR(_forstackindex - 1) != _addr) {
                    if (_forstackindex >= FORSTACK_MAX) {
                        EasyCon_script_stop();
                        return;
                    }
                    _forstackindex++;
                    FOR_I(_forstackindex - 1) = 0;
                    FOR_ADDR(_forstackindex - 1) = _addr;
                    FOR_NEXT(_forstackindex - 1) = (uint16_t)(_ins & ((1 << 11) - 1));
                    if (E_SET) {
                        _ri0 = (int16_t)(REG(_e_val) & 0xF);
                        if (_ri0 != 0) {
                            FOR_I(_forstackindex - 1) = _ri0 | (int32_t)0x80000000;
                        }
                        _ri1 = (int16_t)((REG(_e_val) >> 4) & 0xF);
                        if (_ri1 != 0) {
                            FOR_C(_forstackindex - 1) = REG(_ri1);
                            E(2);
                            JUMP(FOR_NEXT(_forstackindex - 1));
                            break;
                        }
                    }
                    E(0);
                    JUMP(FOR_NEXT(_forstackindex - 1));
                    break;
                }
                break;

            case 0x03: /* Next */
                if (_forstackindex <= 0) {
                    EasyCon_script_stop();
                    return;
                }
                if (_ins0 & 0x04) {
                    _ins2 = EasyCon_read_byte(script_addr++);
                    _ins3 = EasyCon_read_byte(script_addr++);
                    _ins_dword = (uint32_t)_ins0 | ((uint32_t)_ins1 << 8) |
                                 ((uint32_t)_ins2 << 16) | ((uint32_t)_ins3 << 24);
                }
                if (E_SET) {
                    if (_e_val == 1) {
                        /* Mode 1: break */
                        _forstackindex--;
                        break;
                    } else if (_e_val == 0) {
                        /* Mode 0: 初始化循环次数 */
                        if ((_ins0 & 0x04) == 0) {
                            FOR_C(_forstackindex - 1) = _ins & ((1 << 10) - 1);
                            if (FOR_C(_forstackindex - 1) == 0) {
                                FOR_C(_forstackindex - 1) = (int32_t)0x80000000;
                            }
                        } else {
                            FOR_C(_forstackindex - 1) = (int32_t)(_insEx & ((1L << 26) - 1));
                        }
                    }
                    /* Mode 2: 循环次数已被覆盖，不处理 */
                } else {
                    if (FOR_I(_forstackindex - 1) & (int32_t)0x80000000) {
                        REG(FOR_I(_forstackindex - 1) & 0xF) += 1;
                    } else {
                        FOR_I(_forstackindex - 1) += 1;
                    }
                }
                if (FOR_I(_forstackindex - 1) & (int32_t)0x80000000) {
                    n = REG(FOR_I(_forstackindex - 1) & 0xF);
                } else {
                    n = FOR_I(_forstackindex - 1);
                }
                if (FOR_C(_forstackindex - 1) != (int32_t)0x80000000 &&
                    n >= FOR_C(_forstackindex - 1)) {
                    _forstackindex--;
                } else {
                    JUMP(FOR_ADDR(_forstackindex - 1));
                }
                break;

            case 0x04: /* 比较 / Break / Continue / Return */
                if (_ins0 & 0x04) {
                    _ri0 = (int16_t)((_ins1 >> 3) & 0x07);
                    _ri1 = (int16_t)(_ins1 & 0x07);
                    switch (_ins0 & 0x03) {
                        case 0x00: _v = (uint8_t)(REG(_ri0) == REG(_ri1)); break;
                        case 0x01: _v = (uint8_t)(REG(_ri0) != REG(_ri1)); break;
                        case 0x02: _v = (uint8_t)(REG(_ri0) <  REG(_ri1)); break;
                        case 0x03: _v = (uint8_t)(REG(_ri0) <= REG(_ri1)); break;
                        default: break;
                    }
                    _v = (uint8_t)(_v != 0);
                    switch ((_ins1 >> 6) & 0x03) {
                        case 0x00: _flag = (_v != 0); break;
                        case 0x01: _flag = _flag && (_v != 0); break;
                        case 0x02: _flag = _flag || (_v != 0); break;
                        case 0x03: _flag = (_flag != (_v != 0)); break;
                        default: break;
                    }
                } else {
                    switch ((_ins1 >> 5) & 0x07) {
                        case 0x00: /* Break */
                            if ((_ins1 & 0x10) && !_flag) break;
                            _v = (uint8_t)(_ins1 & 0x0F);
                            if ((_forstackindex - (int32_t)_v) < 0) {
                                EasyCon_script_stop();
                                return;
                            }
                            _forstackindex -= _v;
                            E(1);
                            JUMP(FOR_NEXT(_forstackindex - 1));
                            break;
                        case 0x01: /* Continue */
                            if ((_ins1 & 0x10) && !_flag) break;
                            _v = (uint8_t)(_ins1 & 0x0F);
                            if ((_forstackindex - (int32_t)_v) < 0) {
                                EasyCon_script_stop();
                                return;
                            }
                            _forstackindex -= _v;
                            JUMP(FOR_NEXT(_forstackindex - 1));
                            break;
                        case 0x07: /* Return */
                            if ((_ins1 & 0x10) && !_flag) break;
                            if (_callstackindex) {
                                JUMP(CALLSTACK(_callstackindex - 1));
                                _callstackindex--;
                            } else {
                                EasyCon_script_stop();
                            }
                            break;
                        default:
                            break;
                    }
                }
                break;

            case 0x05: /* 寄存器运算 */
                if ((_ins0 & 0x04) == 0) {
                    _ri0 = (int16_t)((_ins >> 7) & 0x07);
                    if (_ri0 == 0) {
                        if ((_ins1 & (1 << 6)) == 0) {
                            /* 与 32 位立即数做二元运算 */
                            _ins2 = EasyCon_read_byte(script_addr++);
                            _ins3 = EasyCon_read_byte(script_addr++);
                            _ins_dword = (uint32_t)_ins0 | ((uint32_t)_ins1 << 8) |
                                         ((uint32_t)_ins2 << 16) | ((uint32_t)_ins3 << 24);
                            _v = (uint8_t)((_ins >> 3) & 0x07);
                            _ri0 = (int16_t)(_ins & 0x07);
                            reg = (int16_t)_insEx;
                            binaryop(_v, (uint8_t)_ri0, reg);
                        }
                    } else {
                        /* 指令：Mov reg, imm7（7 位带符号立即数） */
                        reg = (int16_t)(_ins1 & 0x7F);
                        reg = (int16_t)(reg << 9);
                        reg = (int16_t)(reg >> 9);
                        REG(_ri0) = reg;
                    }
                } else if ((_ins0 & 0x06) == 0x04) {
                    /* 寄存器之间的二元运算 */
                    _v   = (uint8_t)((_ins >> 6) & 0x07);
                    _ri0 = (int16_t)((_ins >> 3) & 0x07);
                    _ri1 = (int16_t)(_ins & 0x07);
                    binaryop(_v, (uint8_t)_ri0, REG(_ri1));
                } else if ((_ins0 & 0x07) == 0x06) {
                    /* 左移 / 右移 */
                    _ri0 = (int16_t)((_ins1 >> 4) & 0x07);
                    _v = (uint8_t)(_ins1 & 0x0F);
                    if (_ri0 == 0) break;
                    if ((_ins1 & 0x80) == 0) {
                        REG(_ri0) = (int16_t)(REG(_ri0) << _v);
                    } else {
                        REG(_ri0) = (int16_t)(REG(_ri0) >> _v);
                    }
                } else {
                    /* 一元运算 */
                    _ri0 = (int16_t)(_ins1 & 0x07);
                    switch ((_ins1 >> 3) & 0x0F) {
                        case 0x02: /* Negative */
                            if (_ri0 != 0) REG(_ri0) = (int16_t)(-REG(_ri0));
                            break;
                        case 0x03: /* Not */
                            if (_ri0 != 0) REG(_ri0) = (int16_t)(~REG(_ri0));
                            break;
                        case 0x04: /* Push */
                            if (_stackindex < STACK_MAX) {
                                _stackindex++;
                                STACK(_stackindex - 1) = (int16_t)REG(_ri0);
                            }
                            break;
                        case 0x05: /* Pop */
                            if (_ri0 != 0 && _stackindex > 0) {
                                _stackindex--;
                                REG(_ri0) = STACK(_stackindex);
                            }
                            break;
                        case 0x07: /* StoreOp */
                            E(_ri0);
                            break;
                        case 0x08: /* Bool */
                            if (_ri0 != 0) REG(_ri0) = (int16_t)(REG(_ri0) != 0);
                            break;
                        case 0x09: /* Rand */
                            if (_ri0 != 0) {
                                if (!_seed) {
                                    _seed = (uint16_t)timer_ms;
                                    EasyCon_write_start(1);
                                    EasyCon_write_data(SEED_OFFSET, (uint8_t *)&_seed, 2);
                                    EasyCon_write_end(1);
                                    srand(_seed);
                                }
                                if (REG(_ri0) != 0) {
                                    REG(_ri0) = (int16_t)(rand() % REG(_ri0));
                                }
                            }
                            break;
                        default:
                            break;
                    }
                }
                break;

            case 0x06: /* 分支 / 调用 */
                reg = (int16_t)(_ins & ((1 << 9) - 1));
                reg = (int16_t)(reg << 7);
                reg = (int16_t)(reg >> 6);
                switch ((_ins0 >> 1) & 0x3) {
                    case 0x00: /* Branch */
                        JUMPNEAR(reg);
                        break;
                    case 0x01: /* BranchTrue */
                        if (_flag) JUMPNEAR(reg);
                        break;
                    case 0x02: /* BranchFalse */
                        if (!_flag) JUMPNEAR(reg);
                        break;
                    case 0x03: /* Call */
                        if (_callstackindex < CALLSTACK_MAX) {
                            _callstackindex++;
                            CALLSTACK(_callstackindex - 1) = (uint16_t)script_addr;
                            JUMPNEAR(reg);
                        }
                        break;
                    default:
                        break;
                }
                break;

            default:
                break;
            }
        }
    }
}

static void binaryop(uint8_t op, uint8_t reg, int16_t value)
{
    if (reg == 0) return;
    switch (op) {
        case 0x00: REG(reg)  = value; break;                        /* Mov */
        case 0x01: REG(reg)  = (int16_t)(REG(reg) + value); break;   /* Add */
        case 0x02: REG(reg)  = (int16_t)(REG(reg) * value); break;   /* Mul */
        case 0x03: if (value) REG(reg) = (int16_t)(REG(reg) / value); break;
        case 0x04: if (value) REG(reg) = (int16_t)(REG(reg) % value); break;
        case 0x05: REG(reg)  = (int16_t)(REG(reg) & value); break;
        case 0x06: REG(reg)  = (int16_t)(REG(reg) | value); break;
        case 0x07: REG(reg)  = (int16_t)(REG(reg) ^ value); break;
        default: break;
    }
}

/* ====================================================================
 * 串口状态机
 * --------------------------------------------------------------------
 * 包格式：除最后一个字节外最高位为 0，最后一个字节最高位为 1。
 *   - 8 字节且末字节最高位为 1 → 按键报文
 *   - 1 字节且为命令字         → 命令
 *   - Flash 状态下所有字节都是烧录数据
 * ==================================================================== */
void EasyCon_serial_task(int16_t byte)
{
    if (byte < 0) return;

    s_last_rx_ms = timer_ms;   /* 刷新静默看门狗时标 */

    /* ---------- Flash（脚本烧录）状态：全部按数据收 ---------- */
    if (flash_index < flash_count) {
        serial_buffer[flash_index] = (uint8_t)byte;
        flash_index++;
        if (flash_index == flash_count) {
            EasyCon_write_start(2);
            EasyCon_write_data(flash_addr, serial_buffer, flash_count);
            easycon_storage_request_flush();
            EasyCon_serial_send(REPLY_FLASHEND);
            easycon_proto_active = true;
        }
        return;
    }

    serial_buffer[serial_buffer_length++] = (uint8_t)byte;

    if ((byte & 0x80) != 0) {
        if (serial_buffer_length == 1 && !serial_command_ready && byte == CMD_READY) {
            /* Ready 命令：只把状态机解锁，不回包 */
            serial_command_ready = true;
        } else if (serial_buffer_length == 8) {
            /* ---------- 按键报文 ---------- */
            if (_script_running) {
                EasyCon_serial_send(REPLY_BUSY);
            } else {
                set_buttons((uint16_t)((serial_buffer[0] << 9) |
                                       (serial_buffer[1] << 2) |
                                       (serial_buffer[2] >> 5)));
                set_HAT_switch((uint8_t)((serial_buffer[2] << 3) |
                                         (serial_buffer[3] >> 4)));
                set_left_stick((uint8_t)((serial_buffer[3] << 4) |
                                         (serial_buffer[4] >> 3)),
                               (uint8_t)((serial_buffer[4] << 5) |
                                         (serial_buffer[5] >> 2)));
                set_right_stick((uint8_t)((serial_buffer[5] << 6) |
                                          (serial_buffer[6] >> 1)),
                                (uint8_t)((serial_buffer[6] << 7) |
                                          (serial_buffer[7] & 0x7F)));
                _report_echo = ECHO_TIMES;
                s_rx_reports++;
                easycon_proto_active = true;
                EasyCon_serial_send(switchcon_is_connected() ? REPLY_ACK : REPLY_ERROR);
            }
            serial_command_ready = false;
        } else if (serial_command_ready) {
            /* ---------- 命令字 ---------- */
            switch (byte) {
                case CMD_DEBUG: {
                    uint32_t v = (uint32_t)FOR_I(_forstackindex - 1);
                    for (int i = 0; i < 4; i++) { EasyCon_serial_send((uint8_t)v); v >>= 8; }
                    v = timer_elapsed;
                    for (int i = 0; i < 4; i++) { EasyCon_serial_send((uint8_t)v); v >>= 8; }
                    v = script_addr;
                    for (int i = 0; i < 2; i++) { EasyCon_serial_send((uint8_t)v); v >>= 8; }
                    v = s_rx_reports;
                    for (int i = 0; i < 4; i++) { EasyCon_serial_send((uint8_t)v); v >>= 8; }
                    easycon_proto_active = true;
                    break;
                }
                case CMD_VERSION:
                    EasyCon_serial_send(EASYCON_VERSION);
                    easycon_proto_active = true;
                    break;
                case CMD_LED:
                    _ledflag ^= 0x8;
                    EasyCon_write_start(1);
                    EasyCon_write_data(LED_SETTING, (uint8_t *)&_ledflag, 1);
                    EasyCon_write_end(1);
                    EasyCon_serial_send(0x00);   /* 上位机要求回 0 */
                    easycon_proto_active = true;
                    break;
                case CMD_READY:
                    serial_command_ready = true;
                    break;
                case CMD_HELLO:
                    EasyCon_serial_send(REPLY_HELLO);
                    easycon_proto_active = true;
                    break;
                case CMD_FLASH:
                    /* 包体：addr_lo, addr_hi, len_lo, len_hi, CMD_FLASH */
                    if (serial_buffer_length != 5) {
                        EasyCon_serial_send(REPLY_ERROR);
                        break;
                    }
                    EasyCon_script_stop();
                    flash_addr  = (uint16_t)(serial_buffer[0] | (serial_buffer[1] << 7));
                    flash_count = (uint16_t)(serial_buffer[2] | (serial_buffer[3] << 7));
                    if (flash_count > SERIAL_BUFFER_SIZE) {
                        flash_count = SERIAL_BUFFER_SIZE;
                    }
                    flash_index = 0;
                    EasyCon_serial_send(REPLY_FLASHSTART);
                    easycon_proto_active = true;
                    break;
                case CMD_SCRIPTSTART:
                    EasyCon_script_start();
                    EasyCon_serial_send(REPLY_SCRIPTACK);
                    easycon_proto_active = true;
                    break;
                case CMD_SCRIPTSTOP:
                    EasyCon_script_stop();
                    EasyCon_serial_send(REPLY_SCRIPTACK);
                    easycon_proto_active = true;
                    break;
                default:
                    EasyCon_serial_send(REPLY_ERROR);
                    break;
            }
        } else {
            /* 非 Ready 状态下的孤立末字节：回一个长度字节让上位机重新同步 */
            EasyCon_serial_send((uint8_t)serial_buffer_length);
        }
        serial_buffer_length = 0;
    }

    if (serial_buffer_length >= SERIAL_BUFFER_SIZE) {
        serial_buffer_length = 0;
    }
}

/* ====================================================================
 * UART 与任务
 * ==================================================================== */
static void easycon_uart_init(void)
{
    uart_config_t cfg;
    cfg.baud_rate = EASYCON_UART_BAUD_RATE;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity    = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

    uart_param_config(EASYCON_UART_NUM, &cfg);
    uart_set_pin(EASYCON_UART_NUM, EASYCON_UART_TX_PIN, EASYCON_UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(EASYCON_UART_NUM, 1024, 1024, 0, NULL, 0);
}

static void easycon_rx_task(void *pv)
{
    (void)pv;
    uint8_t buf[64];
    while (1) {
        int len = uart_read_bytes(EASYCON_UART_NUM, buf, sizeof(buf),
                                  pdMS_TO_TICKS(5));
        for (int i = 0; i < len; i++) {
            EasyCon_serial_task((int16_t)buf[i]);
        }
    }
}

static void easycon_tick_task(void *pv)
{
    (void)pv;
    TickType_t last = xTaskGetTickCount();
    while (1) {
        TickType_t now = xTaskGetTickCount();
        if (now != last) {
            EasyCon_tick();
            easycon_storage_tick();
            last = now;
        }
        vTaskDelay(1);
    }
}

static void easycon_script_pump_task(void *pv)
{
    (void)pv;
    while (1) {
        EasyCon_script_task();
        vTaskDelay(1);
    }
}

void easycon_init(void)
{
    s_apply_lock = xSemaphoreCreateMutex();
    s_tx_lock    = xSemaphoreCreateMutex();

    easycon_storage_init();
    easycon_uart_init();
    EasyCon_script_init();

    xTaskCreatePinnedToCore(easycon_rx_task,          "ez_rx",     4096, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(easycon_tick_task,        "ez_tick",   2048, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(easycon_script_pump_task, "ez_script", 4096, NULL, 1, NULL, 1);

    EasyCon_script_auto_start();

    ESP_LOGI(TAG, "EasyCon ready: uart=%d baud=%d ver=0x%02X MEM_SIZE=%d",
             (int)EASYCON_UART_NUM, EASYCON_UART_BAUD_RATE, EASYCON_VERSION, MEM_SIZE);
}

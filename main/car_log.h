#pragma once
#include <stddef.h>
#include <stdint.h>

/* 固件运行日志环形缓冲。
 *
 * 背景：Mac + ESP32-C3（USB-Serial-JTAG）用常规方式打开串口会拉动 DTR/RTS
 * 把芯片带进 ROM DOWNLOAD 模式，导致"插上就看不到日志"。本模块通过
 * esp_log_set_vprintf() 劫持所有 ESP_LOGx 输出存入内存环形缓冲，
 * 网页 GET /mylog 即可实时读取，无需串口。
 *
 * 注意：panic handler / bootloader 使用 esp_rom_printf 直接写串口，
 * 不经过本钩子，崩溃现场仍需用 tools/serial_log.py 抓串口。
 */

#define CAR_LOG_LINES  96      /* 保留最近多少行（约 14KB 堆） */
#define CAR_LOG_LINE   152     /* 单行最大字节含结尾 0，超出截断 */

/* 尽早在 app_main 开头调用。分配失败不影响主流程，仅关闭网页日志功能 */
void car_log_init(void);

/* 主动写入一行（不经过 ESP_LOG，用于标记关键节点） */
void car_log_push(const char *line);

/* 增量导出序号 >= from 的日志：
 *   {"lines":[...],"next":N,"total":T,"lost":L}
 * next 为下次应传入的 from（已考虑缓冲写满时的截断，不会丢行）
 * lost 为因环形缓冲覆盖而永久丢失的行数
 * from 传 0 表示取当前缓冲内全部内容
 */
void car_log_json(char *buf, size_t len, uint32_t from);

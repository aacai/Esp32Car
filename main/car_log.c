#include "car_log.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

static char          *s_buf;      /* CAR_LOG_LINES * CAR_LOG_LINE 连续块 */
static uint32_t       s_total;    /* 累计写入行数，永远递增（即下一行的序号） */
static vprintf_like_t s_orig;     /* 原始输出函数，继续送往串口 */

/* 日志可能来自任意任务，用自旋锁保护；临界区内只做 memcpy，不做格式化 */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static inline char *slot_of(uint32_t seq)
{
    return s_buf + (size_t)(seq % CAR_LOG_LINES) * CAR_LOG_LINE;
}

/* 清洗并入环：剥离 ANSI 颜色序列、合并换行、替换控制字符 */
static void push_line(const char *src)
{
    if (!s_buf || !src) return;

    char   clean[CAR_LOG_LINE];
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < sizeof(clean); i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == 0x1b) {                       /* ESC[...m 颜色码整段丢弃 */
            while (src[i] && src[i] != 'm') i++;
            if (!src[i]) break;
            continue;
        }
        if (c == '\r' || c == '\n') continue;  /* 一条日志压成一行 */
        if (c < 0x20) c = ' ';
        clean[j++] = (char)c;
    }
    while (j > 0 && clean[j - 1] == ' ') j--;  /* 去尾部空白 */
    clean[j] = 0;
    if (j == 0) return;

    portENTER_CRITICAL(&s_mux);
    memcpy(slot_of(s_total), clean, j + 1);
    s_total++;
    portEXIT_CRITICAL(&s_mux);
}

void car_log_push(const char *line)
{
    push_line(line);
}

/* esp_log 的输出钩子：先留一份到内存，再原样转发给串口 */
static int car_log_vprintf(const char *fmt, va_list ap)
{
    va_list cp;
    va_copy(cp, ap);
    char tmp[CAR_LOG_LINE];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, cp);   /* 在锁外格式化，减少临界区时长 */
    va_end(cp);
    if (n > 0) push_line(tmp);

    return s_orig ? s_orig(fmt, ap) : vprintf(fmt, ap);
}

void car_log_init(void)
{
    if (s_buf) return;
    s_buf = calloc(CAR_LOG_LINES, CAR_LOG_LINE);
    if (!s_buf) return;                              /* 内存不足则静默放弃 */
    s_orig = esp_log_set_vprintf(car_log_vprintf);
}

static void json_escape(const char *src, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 2 < out_len; i++) {
        if (src[i] == '"' || src[i] == '\\') out[j++] = '\\';
        out[j++] = src[i];
    }
    out[j] = 0;
}

static void cat(char *buf, size_t len, size_t *off, const char *fmt, ...)
{
    if (*off >= len) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, len - *off, fmt, ap);
    va_end(ap);
    if (n > 0) {
        *off += (size_t)n;
        if (*off > len) *off = len;
    }
}

void car_log_json(char *buf, size_t len, uint32_t from)
{
    if (!buf || len < 64) return;
    buf[0] = 0;

    if (!s_buf) {
        snprintf(buf, len, "{\"lines\":[],\"next\":0,\"total\":0,\"lost\":0,\"off\":1}");
        return;
    }

    uint32_t total;
    portENTER_CRITICAL(&s_mux);
    total = s_total;
    portEXIT_CRITICAL(&s_mux);

    uint32_t cnt    = (total < CAR_LOG_LINES) ? total : CAR_LOG_LINES;
    uint32_t oldest = total - cnt;
    uint32_t lost   = 0;
    if (from < oldest) { lost = oldest - from; from = oldest; }
    if (from > total)  { from = total; }

    size_t off = 0;
    cat(buf, len, &off, "{\"lines\":[");

    char line[CAR_LOG_LINE];
    char esc[CAR_LOG_LINE * 2];
    int  first = 1;
    uint32_t s = from;
    for (; s < total; s++) {
        portENTER_CRITICAL(&s_mux);
        /* 复制期间该槽位理论上可能被新日志覆盖，概率极低且仅影响该行内容 */
        strncpy(line, slot_of(s), sizeof(line) - 1);
        portEXIT_CRITICAL(&s_mux);
        line[sizeof(line) - 1] = 0;

        json_escape(line, esc, sizeof(esc));
        /* 预留 48 字节给收尾字段，放不下就停在这里，next 如实反映进度 */
        if (off + strlen(esc) + 8 >= len - 48) break;
        cat(buf, len, &off, "%s\"%s\"", first ? "" : ",", esc);
        first = 0;
    }

    /* next 写实际输出到的位置而非 total，缓冲截断时下次能接着取，不丢日志 */
    cat(buf, len, &off, "],\"next\":%lu,\"total\":%lu,\"lost\":%lu}",
        (unsigned long)s, (unsigned long)total, (unsigned long)lost);
}

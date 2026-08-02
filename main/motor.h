#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void motor_init(void);
void motor_set_speed(int percent);   /* 0~100，立即生效 */
void motor_forward(void);
void motor_backward(void);
void motor_turn_left(void);
void motor_turn_right(void);
void motor_stop(void);

/* 统一指令解析（BLE / WiFi 共用）：
   'F'前进 'B'后退 'L'左转 'R'右转 'S'停止
   数字 '0'~'9' 设置速度 0%~90% */
void car_control(char cmd);

/* 状态读取，供 Web /status 调试接口使用 */
typedef enum {
    CAR_STOP = 0,
    CAR_FORWARD,
    CAR_BACKWARD,
    CAR_LEFT,
    CAR_RIGHT
} car_action_t;

car_action_t car_get_action(void);
int          car_get_speed(void);
char         car_get_last_cmd(void);
uint32_t     car_get_cmd_count(void);

#ifdef __cplusplus
}
#endif

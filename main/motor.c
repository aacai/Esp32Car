#include "motor.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "motor";

/* 引脚（可由 menuconfig 覆盖）。
   注意：ESP32-C3 的 GPIO18/19 是 USB D-/D+（USB-Serial/JTAG），
   绝不可用作电机 PWM，否则会禁用 USB、导致无法烧录/调试。
   默认按 docs/接线文档.md 使用 GPIO1/3/4/5/2/10，远离 USB 与 SPI Flash 引脚。 */
#ifndef CONFIG_MOTOR_IN1_GPIO
#define CONFIG_MOTOR_IN1_GPIO 1
#endif
#ifndef CONFIG_MOTOR_IN2_GPIO
#define CONFIG_MOTOR_IN2_GPIO 3
#endif
#ifndef CONFIG_MOTOR_IN3_GPIO
#define CONFIG_MOTOR_IN3_GPIO 4
#endif
#ifndef CONFIG_MOTOR_IN4_GPIO
#define CONFIG_MOTOR_IN4_GPIO 5
#endif
#ifndef CONFIG_MOTOR_ENA_GPIO
#define CONFIG_MOTOR_ENA_GPIO 2
#endif
#ifndef CONFIG_MOTOR_ENB_GPIO
#define CONFIG_MOTOR_ENB_GPIO 10
#endif

#define LEFT_IN1   CONFIG_MOTOR_IN1_GPIO
#define LEFT_IN2   CONFIG_MOTOR_IN2_GPIO
#define RIGHT_IN3  CONFIG_MOTOR_IN3_GPIO
#define RIGHT_IN4  CONFIG_MOTOR_IN4_GPIO
#define ENA_GPIO   CONFIG_MOTOR_ENA_GPIO
#define ENB_GPIO   CONFIG_MOTOR_ENB_GPIO

#define LEDC_TIMER      LEDC_TIMER_0
#define LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LEDC_CH_LEFT    LEDC_CHANNEL_0
#define LEDC_CH_RIGHT   LEDC_CHANNEL_1
#define LEDC_DUTY_RES   LEDC_TIMER_10_BIT
#define LEDC_FREQ_HZ    5000
#define LEDC_MAX_DUTY   ((1 << LEDC_DUTY_RES) - 1)

static int g_speed = 60;

/* 调试状态 */
static car_action_t g_action = CAR_STOP;
static char         g_last_cmd = 'S';
static uint32_t     g_cmd_count = 0;

static void set_left_pwm(int percent)
{
    uint32_t duty = (uint32_t)((percent * LEDC_MAX_DUTY) / 100);
    ledc_set_duty(LEDC_MODE, LEDC_CH_LEFT, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CH_LEFT);
}

static void set_right_pwm(int percent)
{
    uint32_t duty = (uint32_t)((percent * LEDC_MAX_DUTY) / 100);
    ledc_set_duty(LEDC_MODE, LEDC_CH_RIGHT, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CH_RIGHT);
}

static void left_forward(void)  { gpio_set_level(LEFT_IN1, 1);  gpio_set_level(LEFT_IN2, 0); }
static void left_back(void)     { gpio_set_level(LEFT_IN1, 0);  gpio_set_level(LEFT_IN2, 1); }
static void right_forward(void) { gpio_set_level(RIGHT_IN3, 1); gpio_set_level(RIGHT_IN4, 0); }
static void right_back(void)    { gpio_set_level(RIGHT_IN3, 0); gpio_set_level(RIGHT_IN4, 1); }

void motor_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LEFT_IN1) | (1ULL << LEFT_IN2) |
                        (1ULL << RIGHT_IN3) | (1ULL << RIGHT_IN4),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    ledc_timer_config_t t = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);

    ledc_channel_config_t lc = {
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CH_LEFT,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = ENA_GPIO,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&lc);
    lc.channel = LEDC_CH_RIGHT;
    lc.gpio_num = ENB_GPIO;
    ledc_channel_config(&lc);

    motor_stop();
    ESP_LOGI(TAG, "motor driver ready (left=%d/%d right=%d/%d)",
            LEFT_IN1, LEFT_IN2, RIGHT_IN3, RIGHT_IN4);
}

void motor_set_speed(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    g_speed = percent;
    set_left_pwm(g_speed);
    set_right_pwm(g_speed);
}

void motor_forward(void)
{
    left_forward();  right_forward();
    set_left_pwm(g_speed); set_right_pwm(g_speed);
    ESP_LOGI(TAG, "forward %d%%", g_speed);
}

void motor_backward(void)
{
    left_back(); right_back();
    set_left_pwm(g_speed); set_right_pwm(g_speed);
    ESP_LOGI(TAG, "backward %d%%", g_speed);
}

void motor_turn_left(void)
{
    /* 右轮前进、左轮后退 = 原地/差速左转 */
    left_back(); right_forward();
    set_left_pwm(g_speed); set_right_pwm(g_speed);
    ESP_LOGI(TAG, "turn left %d%%", g_speed);
}

void motor_turn_right(void)
{
    left_forward(); right_back();
    set_left_pwm(g_speed); set_right_pwm(g_speed);
    ESP_LOGI(TAG, "turn right %d%%", g_speed);
}

void motor_stop(void)
{
    gpio_set_level(LEFT_IN1, 0);  gpio_set_level(LEFT_IN2, 0);
    gpio_set_level(RIGHT_IN3, 0); gpio_set_level(RIGHT_IN4, 0);
    set_left_pwm(0); set_right_pwm(0);
    ESP_LOGI(TAG, "stop");
}

void car_control(char cmd)
{
    switch (cmd) {
        case 'F': case 'f': motor_forward();  g_action = CAR_FORWARD;  g_last_cmd = 'F'; g_cmd_count++; break;
        case 'B': case 'b': motor_backward(); g_action = CAR_BACKWARD; g_last_cmd = 'B'; g_cmd_count++; break;
        case 'L': case 'l': motor_turn_left(); g_action = CAR_LEFT;    g_last_cmd = 'L'; g_cmd_count++; break;
        case 'R': case 'r': motor_turn_right();g_action = CAR_RIGHT;   g_last_cmd = 'R'; g_cmd_count++; break;
        case 'S': case 's': motor_stop();     g_action = CAR_STOP;     g_last_cmd = 'S'; g_cmd_count++; break;
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            motor_set_speed((cmd - '0') * 10);
            g_last_cmd = cmd; g_cmd_count++;
            ESP_LOGI(TAG, "speed=%d%%", (cmd - '0') * 10);
            break;
        default: ESP_LOGW(TAG, "未知指令: %c (0x%02x)", cmd, cmd); break;
    }
}

car_action_t car_get_action(void)    { return g_action; }
int          car_get_speed(void)     { return g_speed; }
char         car_get_last_cmd(void)  { return g_last_cmd; }
uint32_t     car_get_cmd_count(void) { return g_cmd_count; }

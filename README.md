# ESP32-C3 蓝牙/WiFi 小车（L298N 四电机）

基于 **ESP-IDF v5.3**（纯 C）的 ESP32-C3 小车模板：驱动 **L298N 电机模块 + 4 个直流电机**，提供 **漂亮 Web 遥控页**（WiFi）和 **蓝牙 BLE** 双控制。可在 VS Code 中构建/烧录/调试。

## 目录结构

```
main/
├── main.c        # 入口：初始化电机、WiFi、BLE、Web 服务
├── motor.c/.h    # L298N 驱动（差速转向 + LEDC PWM 调速）
├── web_car.c/.h  # HTTP 服务器：提供遥控页面 /，响应 /api?c=X
├── web/index.html# 遥控页（方向键 + 速度滑块 + 连接蓝牙按钮）
├── ble_car.c/.h  # 蓝牙 NimBLE GATT 串行服务，写入指令即控车
├── CMakeLists.txt
└── Kconfig.projbuild
```

## 一、接线（L298N + 4 电机）—— 并联，不是串联

L298N 只有 **2 路 H 桥**（OUT1/2、OUT3/4）。4 个电机按"左右两组"接：

| L298N 端 | 接法 |
|----------|------|
| OUT1 (+) | 左侧两个电机的 **红线并到一起** 接 OUT1 |
| OUT2 (−) | 左侧两个电机的 **黑线并到一起** 接 OUT2 |
| OUT3 (+) | 右侧两个电机的 **红线并到一起** 接 OUT3 |
| OUT4 (−) | 右侧两个电机的 **黑线并到一起** 接 OUT4 |

> **为什么并联**：并联时每个电机都得到完整电池电压（转速正常）；串联会把电压平分，电机发软。并联后每路电流翻倍，但 TT 小电机单只约 200–500mA，L298N 单路峰值约 2A，够用。

| L298N 端 | 接 ESP32-C3 | 说明 |
|----------|-------------|------|
| IN1 | GPIO1 | 左组方向 + |
| IN2 | GPIO3 | 左组方向 − |
| IN3 | GPIO4 | 右组方向 + |
| IN4 | GPIO5 | 右组方向 − |
| ENA | GPIO2 | 左组 PWM 调速 |
| ENB | GPIO10 | 右组 PWM 调速 |
| GND | ESP32-C3 GND | **必须共地** |
| 5V  | ESP32-C3 5V | 给板子供电（可选，也可 USB 供电，但 GND 必须相连） |
| 12V/+ | 电池正极 | 2×18650(7.4V) / 2S 锂电 / 6×AA(9V) |
| GND | 电池负极 | |

⚠️ **关键**：把 L298N 上 **ENA、ENB 的跳线帽拔掉**，否则 PWM 调速被短路、电机只会全速转。L298N 自身约 2V 压降，用 7.4V 锂电时电机约得 5.4V（适合 6V TT 电机）。

## 二、控制方式

控制指令（Web / BLE 通用）：`F`前进 `B`后退 `L`左转 `R`右转 `S`停止，数字 `0`~`9` 设速度 0%~90%。

### 方式 A：Web 遥控页（WiFi）
1. 小车连上 WiFi 后，串口监视里能看到它的 IP；浏览器打开 `http://<小车IP>/`。
2. 页面有方向键（按下即动、松开停止）、速度滑块。点击即控。

### 方式 B：蓝牙 BLE
- **手机 App**：用 nRF Connect 搜索 `ESP32-C3-Car`，向特征 `0x1234` 写入指令字符（如 `F`）。
- **同一个 Web 页**：点页面里的 **"🔗 连接蓝牙"** 按钮，连上后方向键改为走蓝牙。
  - ⚠️ 浏览器 **Web Bluetooth 要求安全上下文**：页面若由小车以 **http://** 提供，Chrome 会禁用 `navigator.bluetooth`。解决办法：把 `main/web/index.html` 存到手机/电脑本地，用 `file://` 打开（仍为安全上下文），此时既能用蓝牙，也能通过 WiFi 的 `/api` 控车（服务器已开启 CORS）。

## 三、环境准备与运行

1. 安装 ESP-IDF v5.3（`esp32c3` 目标）+ VS Code 扩展 **Espressif IDF**，运行 `ESP-IDF: Configure ESP-IDF Extension`。
2. 改 WiFi 凭据：`sdkconfig.defaults` 的 `CONFIG_EXAMPLE_WIFI_SSID/PASSWORD`，或 `idf.py menuconfig` → `Car Configuration`。
3. 构建烧录：`Ctrl+E Ctrl+D`（烧录+监视），串口会打印小车 IP 与指令日志。
4. 控制：浏览器开 `http://<IP>/`，或用蓝牙（见上）。

> 若 BLE 部分编译异常，可在 `idf.py menuconfig` → `Car Configuration` 关闭 `Enable Bluetooth LE`，此时 Web(WiFi) 小车仍可用。

## 四、调试
`F5` 选择 **ESP-IDF Debug**（内置 USB-JTAG），可在 `app_main` 设断点。

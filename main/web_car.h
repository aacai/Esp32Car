#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 HTTP 服务器：提供遥控页面 (/)，并响应 /api?c=X 控制指令 */
void web_car_start(void);

#ifdef __cplusplus
}
#endif

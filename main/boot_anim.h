#ifndef BOOT_ANIM_H
#define BOOT_ANIM_H

#include <stdint.h>
#include <stdbool.h>

/*
 * 开机动画
 *   - boot_anim_init()  创建一个独立的 boot 屏并加载为当前活动屏
 *                        （必须在 ui_init() 之前调用，让 UI 创建在另一个 screen 上）
 *   - boot_anim_play(target, min_ms, max_ms)
 *                        启动一个 lv_timer 周期检查 ready 标志：
 *                          · 收到 ready 信号 + 已过 min_ms → 立即切换
 *                          · 达到 max_ms 仍未 ready → 强制切换
 *                        切换用 LV_SCR_LOAD_ANIM_FADE_ON，并自动销毁 boot 屏
 *   - boot_anim_signal_ready()  在 WiFi 连上 / IP 获取到时调用，
 *                                通知动画可以提前结束
 */
void boot_anim_init(void);

/* target: 主 UI 的 screen 对象（由 ui_get_main_screen() 取得） */
void boot_anim_play(void *target, uint32_t min_ms, uint32_t max_ms);

/* 收到 ready 信号（WiFi 已连/IP 已获取），允许动画提前结束 */
void boot_anim_signal_ready(void);

/* WiFi 连接失败信号 */
void boot_anim_signal_failed(void);

/* 设置IP地址显示 */
void boot_anim_set_ip(const char *ip);

/* 设置加载文字 */
void boot_anim_set_loading_text(const char *text);

/* BLE连接成功：蓝牙图标变蓝 */
void boot_anim_bt_ready(void);

/* BLE开始连接：进度条开始从50%向100%走 */
void boot_anim_bt_connecting(void);

/* 立即切换到目标屏幕 */
void boot_anim_switch_now(void);

/* 检查是否超时 */
bool boot_anim_timed_out(void);

#endif /* BOOT_ANIM_H */

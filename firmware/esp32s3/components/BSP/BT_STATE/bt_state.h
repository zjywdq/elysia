#ifndef BT_STATE_H
#define BT_STATE_H

#include <stdbool.h>

// ============================
// 蓝牙连接状态监听（GPIO38 外部中断）
// ============================
// 接线：本机 GPIO38 ← bule teeh 工程的 STATUS_GPIO（GPIO5）。
// 对端蓝牙连上耳机时该脚输出高电平，断开时输出低电平。
// 蓝牙已连接 → 关闭本地扬声器输出；蓝牙未连接 → 打开扬声器输出。

/**
 * @brief 初始化 GPIO38 外部中断（双边沿触发 + 消抖）
 *        必须在创建播放任务之前调用
 */
void bt_state_init(void);

/**
 * @brief 查询蓝牙当前是否已连接
 *
 * @return true 已连接（本地扬声器应关闭），false 未连接（扬声器打开）
 */
bool bt_is_connected(void);

#endif // BT_STATE_H

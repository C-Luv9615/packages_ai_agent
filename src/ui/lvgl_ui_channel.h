/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化 LVGL UI 通道。
 * 初始化 LVGL 显示驱动、创建 UI 控件（Chat View、PTT Button、Recording Indicator）
 * 并初始化聊天历史环形缓冲区。在 Phase 3 调用。
 */
int lvgl_ui_channel_init(void);

/**
 * 启动 LVGL UI 通道。
 * 创建 LVGL 事件循环线程（lv_timer_handler 周期调用）。
 * 在 Phase 5 调用，不依赖网络。
 */
int lvgl_ui_channel_start(void);

/**
 * 停止 LVGL UI 通道。
 * 停止事件循环线程，释放 UI 资源和聊天历史。
 */
void lvgl_ui_channel_stop(void);

/**
 * 显示聊天界面。
 * 调用 lv_screen_load() 将 AI Agent 聊天屏幕切换到前台。
 * 可通过 CLI "show_chat" 命令或收到 Agent 回复时自动触发。
 */
void lvgl_ui_channel_show(void);

/**
 * 发送 Agent 回复到 UI。
 * 在 Chat View 中添加 Agent 消息气泡，并调用 voice_channel_speak() 进行 TTS 播报。
 * 首次调用时自动显示聊天界面。由 outbound_dispatch_task 调用。
 *
 * @param text  UTF-8 文本，Agent 回复内容
 * @return 0 成功，负值表示错误
 */
int lvgl_ui_channel_send(const char* text);

/**
 * 发送 Agent 回复到 UI（扩展版）：显式指定卡片左侧色条颜色。
 * bar_color=0 → 沿用 chat_view_classify_reply 启发式自动分类（与 send 等价）。
 * 非 0 → 用指定颜色覆盖分类（绿=结论/蓝=结构化/橙=jira/灰=普通，见
 * LVGL_UI_CARD_COLOR_* 宏）。其余行为（自动显屏、async 调度、TTS）同 send。
 *
 * @param text      UTF-8 文本，Agent 回复内容
 * @param bar_color 色条颜色，0=自动分类
 * @return 0 成功，负值表示错误
 */
int lvgl_ui_channel_send_ex(const char* text, uint32_t bar_color);

/**
 * 显示用户消息气泡（右对齐蓝色气泡，无色条卡片）。
 * 用于回显 PTT 识别到的用户语音文本。不触发 TTS（用户自己的话不播报）。
 * 首次调用时自动显示聊天界面。线程安全：通过 lv_async_call 调度到 LVGL 线程。
 *
 * @param text  UTF-8 文本，用户语音识别内容
 * @return 0 成功，负值表示错误
 */
int lvgl_ui_channel_send_user(const char* text);

/* PTT 按钮状态机，可由外部（AP 经 RPMsg）驱动，反映语音管线阶段。 */
enum lvgl_ui_ptt_state {
    LVGL_UI_PTT_IDLE       = 0,  /* 空闲：恢复默认按钮样式，隐藏指示器 */
    LVGL_UI_PTT_RECORDING = 1,  /* 录音中：按钮红+脉冲，指示器"录音中..." */
    LVGL_UI_PTT_PROCESSING = 2, /* 识别中：停止脉冲，指示器"识别中..." */
};

/**
 * 设置 PTT 按钮状态（录音中/识别中/空闲）。线程安全：通过 lv_async_call
 * 调度到 LVGL 线程执行，避免跨核/跨线程动 widget。
 * @param state  enum lvgl_ui_ptt_state
 * @return 0 成功，负值表示错误
 */
int lvgl_ui_channel_set_ptt(int state);

/* 宠物状态环的状态语义（M7 差异化页）。值与 pet_status_view.h 的 enum
 * pet_state 一一对应；此处用 int 以免把 pet_status_view.h 拖进 AP/agent 的
 * 公共包含路径。0=idle,1=listening,2=thinking,3=executing,4=waiting,
 * 5=error。 */
int lvgl_ui_channel_set_pet_state(int state);

#ifdef __cplusplus
}
#endif

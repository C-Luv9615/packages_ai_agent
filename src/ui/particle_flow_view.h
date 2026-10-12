/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * particle_flow_view.h — 圆屏粒子流（M7 差异化页 ②，纯 LVGL）。
 *
 * UI方案.md §2.4 方案②（三体物理 / 粒子流）的纯 LVGL 退路实现：一只随
 * agent 状态变色/变速的「能量流」视觉——若干粒子环绕中央核心做受控流动，
 * 态切换改变粒子的色、流向（内聚 / 外散 / 旋转 / 混沌）与核心脉动，作为
 * 与宠物状态环（方案①）并列的第二块差异化屏。视觉与宠物脸+环刻意不同
 * （抽象能量流 vs 具象脸），借同一套 agent 状态镜像语义（HachimoDock）。
 *
 * 与主腿（chat_view）/宠物屏（pet_status_view）的关系：独立 LVGL 屏幕，
 * 经 set_page 命令或空闲自动切换前台；不抢占宠物首屏，仅作差异化加分页。
 * 线程安全：与 pet_status_view 一致，所有 widget 操作经 lv_async_call
 * 调度到 LVGL 线程；物理积分由 lv_timer 在 LVGL 线程推进。整数定点运算，
 * 无浮点依赖（CP 有 FPU 亦不依赖，便于跨核）。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 粒子流态——复用 pet_status_view 的 agent 状态语义。 */
enum particle_state {
    PARTICLE_STATE_IDLE      = 0, /* 待机：缓漂 + 绿粒 */
    PARTICLE_STATE_LISTENING = 1, /* 听音：内聚旋入 + 青 */
    PARTICLE_STATE_THINKING  = 2, /* 思考：快旋 + 蓝 */
    PARTICLE_STATE_EXECUTING = 3, /* 执行：外散爆 + 橙 */
    PARTICLE_STATE_WAITING   = 4, /* 等待：驻留脉动 + 黄 */
    PARTICLE_STATE_ERROR     = 5, /* 错误：混沌乱跑 + 红 */
};

/**
 * 初始化粒子屏（创建 widgets + 物理，但不加载前台）。
 * 必须在 LVGL 已起（lvgl_ui_channel_init 之后）后调用。幂等。
 */
int particle_flow_view_init(void);

/** 将粒子屏加载到前台（保存当前屏为 prev）。经 lv_async_call。幂等。 */
void particle_flow_view_show(void);

/** 恢复 show 之前的屏幕。经 lv_async_call。 */
void particle_flow_view_hide(void);

/** 粒子屏当前是否在前台。 */
bool particle_flow_view_is_visible(void);

/** 设置粒子流态（驱动颜色/流向/速度/核心脉动）。线程安全。 */
void particle_flow_view_set_state(enum particle_state state);

#ifdef __cplusplus
}
#endif

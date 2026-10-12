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

/*
 * pet_status_view.c — 圆屏宠物状态环（M7 差异化页，纯 LVGL 退路）。
 *
 * 设计见 pet_status_view.h。一只随 agent 状态变色/变动的宠物脸 + 状态环，
 * 作为待机首屏。状态语义与 HachimoDock 的 agent 状态镜像一致；颜色与主腿
 * 色条呼应（绿=待机/结论、蓝=思考/结构化、橙=执行/jira、黄=等待、红=错误、
 * 青=听音）。所有 widget 操作经 lv_async_call 调度到 LVGL 线程，与 chat_view
 * 同构。动画一律用 lv_anim_t（v9 API：init / set_xxx / start），避免裸定时器抖动。
 */

#include <nuttx/config.h>

#ifdef CONFIG_AI_AGENT_PET_STATUS

#include "ui/pet_status_view.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>

#include <lvgl/lvgl.h>

#define TAG "pet_view"

/* The MiSans CJK static font compiled into liblvgl.a on CP (and into the
 * ai_agent .o on qemu/AP-LVGL_UI). Declared by the font .c via LV_FONT_DECLARE. */
LV_FONT_DECLARE(lv_font_misans_16_cjk);

/* Layout — sized for the 240×296 round LCD but centers on any display
 * (qemu 466 round included) via lv_display_get_*res at init. */
#define PET_FACE_SIZE     96
#define PET_RING_SIZE     150
#define PET_EYE_R         9
#define PET_PUPIL_R       4
#define PET_EYE_DX        20   /* eye offset from face center, x */
#define PET_EYE_DY        (-8) /* eye offset from face center, y (up) */
#define PET_LABEL_DY      78   /* label offset below face center */

/* State palette — bar color semantics borrowed from chat_view cards. */
static const uint32_t s_state_color[] = {
  0x4ade80,  /* IDLE      green  */
  0x22d3ee,  /* LISTENING cyan   */
  0x3b82f6,  /* THINKING  blue   */
  0xf97316,  /* EXECUTING orange */
  0xeab308,  /* WAITING   yellow */
  0xef4444,  /* ERROR     red    */
};
static const char *const s_state_text[] = {
  "待机", "聆听中", "思考中", "执行中", "等待确认", "出错了",
};

struct pet_state_s {
  bool initialized;
  bool visible;
  enum pet_state state;
  lv_obj_t *screen;
  lv_obj_t *ring;     /* lv_arc: status ring around face */
  lv_obj_t *face;     /* circle: pet body/face */
  lv_obj_t *eye_l;
  lv_obj_t *eye_r;
  lv_obj_t *pupil_l;
  lv_obj_t *pupil_r;
  lv_obj_t *label;
  lv_obj_t *prev_screen;
  /* active animation tag — one effect at a time, stopped on state change */
  bool anim_active;
};

static struct pet_state_s s_pet;

/* ---------- animation exec callbacks (LVGL v9: (obj, value)) ---------- */

/* LVGL v9 exec_cb signature is lv_anim_exec_xcb_t = void(*)(void*, lv_anim_value_t),
 * i.e. the first arg is the animated var (void*), NOT lv_obj_t* nor lv_anim_t*.
 * We cast to lv_obj_t* inside for the lv_obj and lv_arc calls. */
/* Spinner: a 90° segment rotating (THINKING / LISTENING). value = start angle. */
static void spin_cb(void *obj, int32_t v)
{
  lv_obj_t *o = (lv_obj_t *)obj;
  lv_arc_set_angles(o, v, v + 90);
}

/* Growing fill: indicator grows 0→360 (EXECUTING). value = end angle. */
static void fill_cb(void *obj, int32_t v)
{
  lv_obj_t *o = (lv_obj_t *)obj;
  lv_arc_set_angles(o, 0, v);
}

/* Breathing: face opacity pulses (IDLE). value 0..100. */
static void breathe_cb(void *obj, int32_t v)
{
  lv_obj_t *o = (lv_obj_t *)obj;
  /* 100% → 255, 0% → 190 (gentle). */
  lv_obj_set_style_opa(o, LV_OPA_COVER - (v * (LV_OPA_COVER - 190)) / 100, 0);
}

/* Blink: both eyes opacity (WAITING). value 0..100, triangle-ish via sin-free. */
static void blink_cb(void *obj, int32_t v)
{
  lv_obj_t *o = (lv_obj_t *)obj;
  /* open(0) → closed(50) → open(100): opaque at ends, transparent at mid. */
  lv_opa_t op = (v < 50) ? LV_OPA_COVER - (v * 2)
                       : LV_OPA_TRANSP + ((v - 50) * 2 * LV_OPA_COVER / 100);
  if (op > LV_OPA_COVER) op = LV_OPA_COVER;
  lv_obj_set_style_opa(o, op, 0);
}

/* Shake: face x jitter (ERROR). value 0..100 maps to a few oscillations. */
static void shake_cb(void *obj, int32_t v)
{
  lv_obj_t *o = (lv_obj_t *)obj;
  /* 0..100 → four half-swings of ±5px. Simple triangle per 25-step. */
  int seg = v / 25;
  int phase = v % 25;
  int amp = (phase < 13) ? (phase * 5 / 12) : ((25 - phase) * 5 / 12);
  int sign = (seg % 2 == 0) ? 1 : -1;
  lv_obj_set_style_translate_x(o, sign * amp, 0);
}

static void stop_anim(void)
{
  /* Delete any anim bound to our widgets. lv_anim_delete is a no-op if none. */
  if (s_pet.ring)  lv_anim_delete(s_pet.ring,  spin_cb);
  if (s_pet.face)  lv_anim_delete(s_pet.face,  breathe_cb);
  if (s_pet.face)  lv_anim_delete(s_pet.face,  shake_cb);
  if (s_pet.eye_l) lv_anim_delete(s_pet.eye_l, blink_cb);
  if (s_pet.eye_r) lv_anim_delete(s_pet.eye_r, blink_cb);
  s_pet.anim_active = false;
}

/* Position pupils within eyes according to state. */
static void apply_pupils(enum pet_state st)
{
  int dx = 0, dy = 0;            /* pupil offset from eye center */
  lv_opa_t eye_opa = LV_OPA_COVER;
  switch (st)
    {
      case PET_STATE_THINKING:  dy = -3; break;             /* look up */
      case PET_STATE_LISTENING: dy = 3;  break;              /* look down */
      case PET_STATE_EXECUTING: dx = 0;  dy = 0; break;      /* focused */
      case PET_STATE_WAITING:   /* blink handled by anim; pupils normal */
      case PET_STATE_IDLE:      dx = 0;  dy = 0; break;
      case PET_STATE_ERROR:     dx = 3;  dy = -3; eye_opa = LV_OPA_60; break;
      default: break;
    }
  if (s_pet.pupil_l) lv_obj_align(s_pet.pupil_l, LV_ALIGN_CENTER, -dx, dy);
  if (s_pet.pupil_r) lv_obj_align(s_pet.pupil_r, LV_ALIGN_CENTER,  dx, dy);
  /* For ERROR, dim eyes a touch so the "X" feel reads (pupils at corners). */
  if (s_pet.eye_l) lv_obj_set_style_opa(s_pet.eye_l, eye_opa, 0);
  if (s_pet.eye_r) lv_obj_set_style_opa(s_pet.eye_r, eye_opa, 0);
}

static void start_anim_for(enum pet_state st)
{
  lv_anim_t a;
  uint32_t color = s_state_color[st];

  stop_anim();
  if (!s_pet.ring || !s_pet.face) return;

  /* Ring color always tracks state; base angles reset per effect. */
  lv_obj_set_style_arc_color(s_pet.ring, lv_color_hex(color), 0);

  switch (st)
    {
      case PET_STATE_THINKING:
      case PET_STATE_LISTENING:
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_pet.ring);
        lv_anim_set_values(&a, 0, 360);
        lv_anim_set_time(&a, 1400);
        lv_anim_set_exec_cb(&a, spin_cb);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        break;
      case PET_STATE_EXECUTING:
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_pet.ring);
        lv_anim_set_values(&a, 0, 360);
        lv_anim_set_time(&a, 900);
        lv_anim_set_exec_cb(&a, fill_cb);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        break;
      case PET_STATE_IDLE:
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_pet.face);
        lv_anim_set_values(&a, 0, 100);
        lv_anim_set_time(&a, 1800);
        lv_anim_set_exec_cb(&a, breathe_cb);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        lv_arc_set_angles(s_pet.ring, 0, 360);  /* full ring */
        break;
      case PET_STATE_WAITING:
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_pet.eye_l);
        lv_anim_set_values(&a, 0, 100);
        lv_anim_set_time(&a, 700);
        lv_anim_set_exec_cb(&a, blink_cb);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_pet.eye_r);
        lv_anim_set_values(&a, 0, 100);
        lv_anim_set_time(&a, 700);
        lv_anim_set_exec_cb(&a, blink_cb);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        lv_arc_set_angles(s_pet.ring, 0, 360);
        break;
      case PET_STATE_ERROR:
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_pet.face);
        lv_anim_set_values(&a, 0, 100);
        lv_anim_set_time(&a, 350);
        lv_anim_set_exec_cb(&a, shake_cb);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        lv_arc_set_angles(s_pet.ring, 0, 360);
        break;
      default:
        lv_arc_set_angles(s_pet.ring, 0, 360);
        break;
    }
  s_pet.anim_active = true;
}

/* ---------- async apply (runs in LVGL thread) ---------- */

static void apply_state_async_cb(void *data)
{
  enum pet_state st = (enum pet_state)(intptr_t)data;
  if (!s_pet.initialized || !s_pet.face) return;

  s_pet.state = st;
  uint32_t color = s_state_color[st];

  lv_obj_set_style_bg_color(s_pet.face, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(s_pet.face, LV_OPA_COVER, 0);
  lv_obj_set_style_translate_x(s_pet.face, 0, 0);  /* clear any shake offset */
  lv_obj_set_style_opa(s_pet.face, LV_OPA_COVER, 0);

  if (s_pet.label) lv_label_set_text(s_pet.label, s_state_text[st]);

  apply_pupils(st);
  start_anim_for(st);
  syslog(LOG_INFO, "[%s] state → %s\n", TAG, s_state_text[st]);
}

void pet_status_view_set_state(enum pet_state state)
{
  if ((int)state < 0 || (int)state > PET_STATE_ERROR) return;
  /* Async: widget mutation must happen in the LVGL thread. */
  lv_async_call(apply_state_async_cb, (void *)(intptr_t)state);
}

/* ---------- screen show / hide ---------- */

static void show_async_cb(void *data)
{
  (void)data;
  if (!s_pet.initialized || !s_pet.screen || s_pet.visible) return;
  s_pet.prev_screen = lv_screen_active();
  lv_screen_load(s_pet.screen);
  s_pet.visible = true;
  /* Re-assert current state's animation (lv_anim may have been paused off-screen). */
  start_anim_for(s_pet.state);
  syslog(LOG_INFO, "[%s] pet screen shown\n", TAG);
}

void pet_status_view_show(void)
{
  if (!s_pet.initialized) return;
  lv_async_call(show_async_cb, NULL);
}

static void hide_async_cb(void *data)
{
  (void)data;
  if (!s_pet.initialized || !s_pet.visible) return;
  stop_anim();
  if (s_pet.prev_screen) lv_screen_load(s_pet.prev_screen);
  s_pet.visible = false;
  syslog(LOG_INFO, "[%s] pet screen hidden\n", TAG);
}

void pet_status_view_hide(void)
{
  if (!s_pet.initialized) return;
  lv_async_call(hide_async_cb, NULL);
}

bool pet_status_view_is_visible(void)
{
  return s_pet.visible;
}

/* ---------- init ---------- */

static lv_obj_t *make_circle(lv_obj_t *parent, int32_t size,
                              uint32_t color, lv_opa_t opa)
{
  lv_obj_t *o = lv_obj_create(parent);
  if (!o) return NULL;
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, size, size);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_center(o);
  return o;
}

int pet_status_view_init(void)
{
  if (s_pet.initialized) return 0;

  memset(&s_pet, 0, sizeof(s_pet));
  s_pet.state = PET_STATE_IDLE;

  s_pet.screen = lv_obj_create(NULL);
  if (!s_pet.screen) return -ENOMEM;
  lv_obj_set_style_bg_color(s_pet.screen, lv_color_hex(0x1a1a2e), 0);
  lv_obj_set_style_bg_opa(s_pet.screen, LV_OPA_COVER, 0);
  lv_obj_set_style_text_font(s_pet.screen, &lv_font_misans_16_cjk, 0);

  /* Ring (arc) — centered, then face centered on top of it. */
  s_pet.ring = lv_arc_create(s_pet.screen);
  if (s_pet.ring)
    {
      lv_obj_set_size(s_pet.ring, PET_RING_SIZE, PET_RING_SIZE);
      lv_obj_center(s_pet.ring);
      lv_arc_set_rotation(s_pet.ring, 0);
      lv_arc_set_bg_angles(s_pet.ring, 0, 360);
      lv_arc_set_angles(s_pet.ring, 0, 360);
      lv_arc_set_range(s_pet.ring, 0, 360);
      lv_obj_set_style_arc_width(s_pet.ring, 4, 0);
      lv_obj_set_style_arc_width(s_pet.ring, 4, LV_PART_MAIN);
      lv_obj_set_style_arc_color(s_pet.ring, lv_color_hex(0x2a2a3e), LV_PART_MAIN);
      lv_obj_set_style_arc_color(s_pet.ring, lv_color_hex(0x4ade80), 0);
      lv_obj_set_style_arc_rounded(s_pet.ring, true, 0);
      lv_obj_set_style_arc_rounded(s_pet.ring, true, LV_PART_MAIN);
      lv_obj_remove_flag(s_pet.ring, LV_OBJ_FLAG_CLICKABLE);
      /* Hide the arc knob. */
      lv_obj_set_style_bg_opa(s_pet.ring, LV_OPA_TRANSP, LV_PART_KNOB);
    }

  /* Face. */
  s_pet.face = make_circle(s_pet.screen, PET_FACE_SIZE, 0x4ade80, LV_OPA_COVER);
  if (!s_pet.face) goto fail;

  /* Eyes (white) — children of face so they move with it. */
  s_pet.eye_l = make_circle(s_pet.face, PET_EYE_R * 2, 0xffffff, LV_OPA_COVER);
  if (s_pet.eye_l) lv_obj_align(s_pet.eye_l, LV_ALIGN_CENTER, -PET_EYE_DX, PET_EYE_DY);
  s_pet.eye_r = make_circle(s_pet.face, PET_EYE_R * 2, 0xffffff, LV_OPA_COVER);
  if (s_pet.eye_r) lv_obj_align(s_pet.eye_r, LV_ALIGN_CENTER,  PET_EYE_DX, PET_EYE_DY);

  /* Pupils (dark) — children of eyes. */
  s_pet.pupil_l = make_circle(s_pet.eye_l, PET_PUPIL_R * 2, 0x101020, LV_OPA_COVER);
  s_pet.pupil_r = make_circle(s_pet.eye_r, PET_PUPIL_R * 2, 0x101020, LV_OPA_COVER);

  /* Status label below the face. */
  s_pet.label = lv_label_create(s_pet.screen);
  if (s_pet.label)
    {
      lv_label_set_text(s_pet.label, s_state_text[PET_STATE_IDLE]);
      lv_obj_set_style_text_color(s_pet.label, lv_color_hex(0xcccccc), 0);
      lv_obj_set_style_text_font(s_pet.label, &lv_font_misans_16_cjk, 0);
      lv_obj_align(s_pet.label, LV_ALIGN_CENTER, 0, PET_LABEL_DY);
    }

  s_pet.initialized = true;
  syslog(LOG_INFO, "[%s] init OK (圆屏宠物状态环)\n", TAG);
  return 0;

fail:
  if (s_pet.screen) lv_obj_del(s_pet.screen);
  memset(&s_pet, 0, sizeof(s_pet));
  return -ENOMEM;
}

#endif /* CONFIG_AI_AGENT_PET_STATUS */

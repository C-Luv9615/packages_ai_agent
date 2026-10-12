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

/*
 * particle_flow_view.c — 圆屏粒子流（M7 差异化页 ②，纯 LVGL）。
 *
 * 设计见 particle_flow_view.h。18 颗粒子在圆屏内做受控布朗流动，中央核心
 * 脉动；态切换改变粒子色 + 流向偏置（内聚 / 外散 / 旋转 / 混沌）+ 核心脉速。
 * 视觉与宠物脸+环刻意不同（抽象能量流），借同一套 agent 状态镜像语义。
 *
 * 物理：整数定点，位置以像素为单位，速度以 q8（256 = 1px/tick）为单位，
 * 每 tick (33ms≈30fps) 由 lv_timer 在 LVGL 线程推进；边界反弹用速度反转 +
 * 位置钳位（距离平方比较，无 sqrt）。无浮点 / math.h 依赖，跨核可移植。
 * 随机扰动用每粒子独立 LCG（确定性伪噪声，免 rand 依赖）。
 */

#include <nuttx/config.h>

#ifdef CONFIG_AI_AGENT_PARTICLE_FLOW

#include "ui/particle_flow_view.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>

#include <lvgl/lvgl.h>

#define TAG "particle_view"

LV_FONT_DECLARE(lv_font_misans_16_cjk);

#define N_PARTICLES    18
#define PARTICLE_DIAM  7        /* px, odd so center pixel is exact */
#define CORE_DIAM      34
#define GUIDE_RING_DIAM 150
#define VEL_Q           8        /* velocity fixed-point: 256 = 1 px/tick */
#define ONE_PX          (1 << VEL_Q)
#define TICK_MS         33

/* State palette — same agent-status semantics as pet_status_view. */
static const uint32_t s_state_color[] = {
  0x4ade80, 0x22d3ee, 0x3b82f6, 0xf97316, 0xeab308, 0xef4444,
};
static const char *const s_state_text[] = {
  "粒子·待机", "粒子·聆听", "粒子·思考", "粒子·执行", "粒子·等待", "粒子·错误",
};

/* Per-state physics knobs:
 *   perturb  — LCG noise amplitude fed into velocity (q8 units)
 *   swirl    — tangential bias strength (rotation; +CW, -CCW)
 *   radial   — radial bias (+outward / -inward)
 *   damp     — velocity retention per tick (0..256) */
typedef struct {
  int32_t perturb;
  int32_t swirl;
  int32_t radial;
  int32_t damp;
} phys_t;

static const phys_t s_phys[] = {
  [PARTICLE_STATE_IDLE]      = { 10,   4,   0, 248 },  /* gentle drift */
  [PARTICLE_STATE_LISTENING] = { 16,  10, -18, 244 },  /* inward + slow swirl */
  [PARTICLE_STATE_THINKING] = { 20,  34,   0, 250 },  /* fast swirl */
  [PARTICLE_STATE_EXECUTING] = { 28,  -8,  26, 246 }, /* outward burst */
  [PARTICLE_STATE_WAITING]   = {  4,   2,   0, 220 },  /* near-frozen pulse */
  [PARTICLE_STATE_ERROR]     = { 90,   0,   0, 252 },  /* chaotic scatter */
};

struct particle_s {
  int32_t x, y;          /* position, pixels */
  int32_t vx, vy;        /* velocity, q8 (256 = 1px/tick) */
  uint32_t lcg;          /* per-particle pseudo-noise state */
  int32_t last_rx, last_ry;  /* last rendered top-left; skip set_pos if unchanged */
};

struct pf_state_s {
  bool initialized;
  bool visible;
  enum particle_state state;
  lv_obj_t *screen;
  lv_obj_t *guide_ring;  /* faint lv_arc, shared motif with pet */
  lv_obj_t *core;        /* central pulsing circle */
  lv_obj_t *particles[N_PARTICLES];
  lv_obj_t *label;
  lv_obj_t *prev_screen;
  struct particle_s p[N_PARTICLES];
  lv_timer_t *timer;
  uint32_t tick;
  /* display geometry (pixels) */
  int32_t cx, cy, R, R_sq;
};

static struct pf_state_s s_pf;

/* ---------- LCG (per-particle deterministic pseudo-noise) ---------- */

static inline int32_t lcg_next(uint32_t *s)
{
  *s = (*s * 1103515765u + 12345u) & 0x7fffffffu;
  return (int32_t)(*s >> 15) - 16384;   /* [-16384, 16383] */
}

/* ---------- physics step (runs in LVGL thread via timer) ---------- */

static void physics_step(void)
{
  const phys_t *ph = &s_phys[s_pf.state];
  int32_t cx = s_pf.cx, cy = s_pf.cy, R_sq = s_pf.R_sq;
  int32_t damp = ph->damp;
  int32_t perturb = ph->perturb;
  int32_t swirl = ph->swirl;
  int32_t radial = ph->radial;

  for (int i = 0; i < N_PARTICLES; i++)
    {
      struct particle_s *p = &s_pf.p[i];
      int32_t dx = p->x - cx;
      int32_t dy = p->y - cy;

      /* LCG noise → small velocity kick (perturb in q8; noise normalized to ~[-1,1] px). */
      int32_t nx = lcg_next(&p->lcg);
      int32_t ny = lcg_next(&p->lcg);
      p->vx += (nx * perturb) / 16384;
      p->vy += (ny * perturb) / 16384;

      /* Swirl: tangential bias ∝ radial offset (unnormalized → faster swirl
       * farther out, reads as rotation). tang = (-dy, dx). */
      p->vx += (-dy * swirl) / 64;
      p->vy += ( dx * swirl) / 64;

      /* Radial bias: outward (+) pulls particles to the rim, inward (-) sucks
       * them toward the core. */
      p->vx += (dx * radial) / 64;
      p->vy += (dy * radial) / 64;

      /* Damping. */
      p->vx = (p->vx * damp) / 256;
      p->vy = (p->vy * damp) / 256;

      /* Speed clamp so chaos stays bounded. */
      int32_t vmax = 3 * ONE_PX;
      if (p->vx >  vmax) p->vx =  vmax;
      if (p->vx < -vmax) p->vx = -vmax;
      if (p->vy >  vmax) p->vy =  vmax;
      if (p->vy < -vmax) p->vy = -vmax;

      /* Advance. */
      int32_t nxp = p->x + (p->vx >> VEL_Q);
      int32_t nyp = p->y + (p->vy >> VEL_Q);

      /* Round-bound bounce: if next step leaves the disc, reverse velocity
       * (cheap reflection) and don't move this tick. dist_sq in pixel units
       * fits in int32 (max ~217k). */
      int32_t ddx = nxp - cx;
      int32_t ddy = nyp - cy;
      int32_t dsq = ddx * ddx + ddy * ddy;
      if (dsq > R_sq)
        {
          p->vx = -p->vx;
          p->vy = -p->vy;
        }
      else
        {
          p->x = nxp;
          p->y = nyp;
        }

      /* Render — only invalidate when the integer pixel position actually
       * changes.  Particles move sub-pixel most ticks (q8 velocity), so an
       * unconditional set_pos would invalidate 18 regions every 33 ms even
       * when nothing visibly moved.  On flush paths that can't keep up
       * (qemu goldfish headless fb) the constant re-invalidation starves the
       * flush so the screen never reaches the framebuffer.  Skipping no-op
       * moves collapses invalidations to only real pixel crossings. */
      lv_obj_t *o = s_pf.particles[i];
      if (o)
        {
          int32_t rx = p->x - (PARTICLE_DIAM / 2);
          int32_t ry = p->y - (PARTICLE_DIAM / 2);
          if (rx != p->last_rx || ry != p->last_ry)
            {
              lv_obj_set_pos(o, rx, ry);
              p->last_rx = rx;
              p->last_ry = ry;
            }
        }
    }

  /* Core pulse: opacity breathing driven by tick (no lv_anim to keep the
   * pulsing phase-locked to the physics tick). */
  if (s_pf.core)
    {
      uint32_t t = s_pf.tick & 63;            /* 0..63 */
      int32_t tri = (t < 32) ? t : (63 - t);  /* triangle 0..31 */
      lv_opa_t opa = LV_OPA_60 + (tri * (LV_OPA_COVER - LV_OPA_60)) / 31;
      lv_obj_set_style_opa(s_pf.core, opa, 0);
    }

  s_pf.tick++;
}

static void timer_cb(lv_timer_t *t)
{
  (void)t;
  if (s_pf.initialized && s_pf.visible)
    physics_step();
}

/* ---------- async apply (runs in LVGL thread) ---------- */

static void apply_state_async_cb(void *data)
{
  enum particle_state st = (enum particle_state)(intptr_t)data;
  if (!s_pf.initialized) return;
  s_pf.state = st;
  uint32_t color = s_state_color[st];

  for (int i = 0; i < N_PARTICLES; i++)
    {
      if (s_pf.particles[i])
        {
          lv_obj_set_style_bg_color(s_pf.particles[i], lv_color_hex(color), 0);
        }
    }
  if (s_pf.core)
    {
      lv_obj_set_style_bg_color(s_pf.core, lv_color_hex(color), 0);
    }
  if (s_pf.guide_ring)
    {
      lv_obj_set_style_arc_color(s_pf.guide_ring, lv_color_hex(color), 0);
    }
  if (s_pf.label)
    {
      lv_label_set_text(s_pf.label, s_state_text[st]);
    }
  syslog(LOG_INFO, "[%s] state → %s\n", TAG, s_state_text[st]);
}

void particle_flow_view_set_state(enum particle_state state)
{
  if ((int)state < 0 || (int)state > PARTICLE_STATE_ERROR) return;
  lv_async_call(apply_state_async_cb, (void *)(intptr_t)state);
}

/* ---------- screen show / hide ---------- */

static void show_async_cb(void *data)
{
  (void)data;
  if (!s_pf.initialized || !s_pf.screen || s_pf.visible) return;
  s_pf.prev_screen = lv_screen_active();
  lv_screen_load(s_pf.screen);
  /* Re-assert bg + force a synchronous refresh so the secondary (async-loaded)
   * screen flushes immediately.  The boot/home pet screen is flushed during
   * lvgl_ui_channel_init; a page-switched-in screen needs the explicit nudge
   * or its first frame can be skipped by lazy flush paths. */
  lv_obj_set_style_bg_color(s_pf.screen, lv_color_hex(0x1a1a2e), 0);
  lv_obj_set_style_bg_opa(s_pf.screen, LV_OPA_COVER, 0);
  lv_obj_invalidate(s_pf.screen);
  lv_refr_now(lv_display_get_default());
  s_pf.visible = true;
  syslog(LOG_INFO, "[%s] particle screen shown\n", TAG);
}

void particle_flow_view_show(void)
{
  if (!s_pf.initialized) return;
  lv_async_call(show_async_cb, NULL);
}

static void hide_async_cb(void *data)
{
  (void)data;
  if (!s_pf.initialized || !s_pf.visible) return;
  if (s_pf.prev_screen) lv_screen_load(s_pf.prev_screen);
  s_pf.visible = false;
  syslog(LOG_INFO, "[%s] particle screen hidden\n", TAG);
}

void particle_flow_view_hide(void)
{
  if (!s_pf.initialized) return;
  lv_async_call(hide_async_cb, NULL);
}

bool particle_flow_view_is_visible(void)
{
  return s_pf.visible;
}

/* ---------- init ---------- */

static lv_obj_t *make_circle(lv_obj_t *parent, int32_t diam,
                              uint32_t color, lv_opa_t opa)
{
  lv_obj_t *o = lv_obj_create(parent);
  if (!o) return NULL;
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, diam, diam);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_center(o);
  return o;
}

int particle_flow_view_init(void)
{
  if (s_pf.initialized) return 0;

  memset(&s_pf, 0, sizeof(s_pf));
  s_pf.state = PARTICLE_STATE_IDLE;

  s_pf.screen = lv_obj_create(NULL);
  if (!s_pf.screen) return -ENOMEM;
  lv_obj_set_style_bg_color(s_pf.screen, lv_color_hex(0x1a1a2e), 0);
  lv_obj_set_style_bg_opa(s_pf.screen, LV_OPA_COVER, 0);
  lv_obj_set_style_text_font(s_pf.screen, &lv_font_misans_16_cjk, 0);

  /* Display geometry: center + round bound. */
  lv_display_t *disp = lv_display_get_default();
  int32_t h = lv_display_get_horizontal_resolution(disp);
  int32_t v = lv_display_get_vertical_resolution(disp);
  s_pf.cx = h / 2;
  s_pf.cy = v / 2;
  int32_t half = (h < v ? h : v) / 2;
  s_pf.R = (half > 8) ? (half - 8) : 8;
  s_pf.R_sq = s_pf.R * s_pf.R;

  /* Faint guide ring (shared motif with pet_status_view). */
  s_pf.guide_ring = lv_arc_create(s_pf.screen);
  if (s_pf.guide_ring)
    {
      lv_obj_set_size(s_pf.guide_ring, GUIDE_RING_DIAM, GUIDE_RING_DIAM);
      lv_obj_center(s_pf.guide_ring);
      lv_arc_set_bg_angles(s_pf.guide_ring, 0, 360);
      lv_arc_set_angles(s_pf.guide_ring, 0, 360);
      lv_obj_set_style_arc_width(s_pf.guide_ring, 2, 0);
      lv_obj_set_style_arc_width(s_pf.guide_ring, 2, LV_PART_MAIN);
      lv_obj_set_style_arc_color(s_pf.guide_ring, lv_color_hex(0x2a2a3e), LV_PART_MAIN);
      lv_obj_set_style_arc_color(s_pf.guide_ring, lv_color_hex(0x4ade80), 0);
      lv_obj_set_style_bg_opa(s_pf.guide_ring, LV_OPA_TRANSP, LV_PART_KNOB);
      lv_obj_remove_flag(s_pf.guide_ring, LV_OBJ_FLAG_CLICKABLE);
    }

  /* Central pulsing core. */
  s_pf.core = make_circle(s_pf.screen, CORE_DIAM, 0x4ade80, LV_OPA_80);

  /* Particles — scattered initially within the disc. */
  uint32_t seed = 0x1234abcd;
  for (int i = 0; i < N_PARTICLES; i++)
    {
      /* place around a ring with jitter so they don't overlap the core */
      int32_t ang = (i * 360 / N_PARTICLES) % 360;
      /* cheap sin/cos via a 4-bin triangle so no math.h: */
      int32_t qx = (ang < 90) ? ang : (ang < 270) ? (180 - ang) : (ang - 360);
      int32_t qy = ((ang + 270) % 360);
      qy = (qy < 90) ? qy : (qy < 270) ? (180 - qy) : (qy - 360);
      /* qx, qy ∈ [-90, 90] → normalize to [-1, 1] */
      int32_t rx = (qx * 0x7fff) / 90;
      int32_t ry = (qy * 0x7fff) / 90;
      int32_t radius = s_pf.R * 2 / 3;
      int32_t jitter = (lcg_next(&seed) * radius) / 16384;
      int32_t rr = radius + (jitter / 4);
      if (rr < 6) rr = 6;
      if (rr > s_pf.R - 2) rr = s_pf.R - 2;

      s_pf.p[i].x = s_pf.cx + (int32_t)(((int64_t)rx * rr) / 0x7fff);
      s_pf.p[i].y = s_pf.cy + (int32_t)(((int64_t)ry * rr) / 0x7fff);
      s_pf.p[i].vx = lcg_next(&seed) / 8;   /* small initial velocity */
      s_pf.p[i].vy = lcg_next(&seed) / 8;
      s_pf.p[i].lcg = seed;

      s_pf.particles[i] = make_circle(s_pf.screen, PARTICLE_DIAM,
                                       0x4ade80, LV_OPA_COVER);
      if (s_pf.particles[i])
        {
          int32_t prx = s_pf.p[i].x - (PARTICLE_DIAM / 2);
          int32_t pry = s_pf.p[i].y - (PARTICLE_DIAM / 2);
          lv_obj_set_pos(s_pf.particles[i], prx, pry);
          s_pf.p[i].last_rx = prx;
          s_pf.p[i].last_ry = pry;
        }
    }

  /* State label. */
  s_pf.label = lv_label_create(s_pf.screen);
  if (s_pf.label)
    {
      lv_label_set_text(s_pf.label, s_state_text[PARTICLE_STATE_IDLE]);
      lv_obj_set_style_text_color(s_pf.label, lv_color_hex(0xcccccc), 0);
      lv_obj_set_style_text_font(s_pf.label, &lv_font_misans_16_cjk, 0);
      lv_obj_align(s_pf.label, LV_ALIGN_CENTER, 0, s_pf.cy / 2 + 8);
    }

  /* Physics timer — runs on the LVGL thread, so widget mutation is safe. */
  s_pf.timer = lv_timer_create(timer_cb, TICK_MS, NULL);
  if (s_pf.timer)
    {
      lv_timer_set_repeat_count(s_pf.timer, -1);
    }

  s_pf.initialized = true;
  syslog(LOG_INFO, "[%s] init OK (圆屏粒子流, %d 粒子, disp %dx%d R=%d)\n",
         TAG, N_PARTICLES, (int)h, (int)v, (int)s_pf.R);
  return 0;
}

#endif /* CONFIG_AI_AGENT_PARTICLE_FLOW */

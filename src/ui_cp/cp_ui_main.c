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
 * ai_agent_ui — CP-side (路 B) UI renderer entry point.
 *
 * BK7258 dual-core: CP (CPU0) owns LCD/LVGL/FPU/GDMA, AP (CPU1) runs the
 * agent.  This standalone NSH builtin builds ONLY the LVGL chat-view assets
 * (lvgl_ui_channel + lvgl_toast + MiSans CJK font) on the CP core, without
 * pulling agent_main / llm / voice / TLS.  voice_channel_* and
 * message_bus are stubbed here (cp_stubs.c) — CP only renders; TTS/ASR live
 * on AP and will drive this UI over the AP→CP UI RPMsg endpoint in a
 * follow-up slice.
 *
 * For now this is a bring-up smoke test: launch from CP NSH, it inits the
 * chat view, lights the round screen with our own widgets, and posts one
 * test bubble — proving the ai_agent UI assets run on the real CP+LCD
 * (M3 prerequisite, before the RPMsg transport is layered on).
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include "ui/lvgl_ui_channel.h"
#include "ui_rpmsg_server.h"
#ifdef CONFIG_AI_AGENT_PET_STATUS
#include "ui/pet_status_view.h"
#endif

#define TAG "ai_agent_ui"

int main(int argc, FAR char *argv[])
{
  int ret;

  syslog(LOG_INFO, "[%s] CP UI renderer starting (LCD/LVGL owner core)\n", TAG);

  ret = lvgl_ui_channel_init();
  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] lvgl_ui_channel_init failed rc=%d\n", TAG, ret);
      return 1;
    }

  ret = lvgl_ui_channel_start();
  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] lvgl_ui_channel_start failed rc=%d\n", TAG, ret);
      return 2;
    }

#ifdef CONFIG_AI_AGENT_PET_STATUS
  /* M7 差异化页 (pure-LVGL 退路): show the pet status ring as the cold-start
   * home screen.  The chat screen is built but stays in the background; the
   * first incoming RPMsg message (or a SET_PET_STATE) drives the foreground.
   * Idle-yield (lvgl_ui_channel's timer) brings the pet back after a chat
   * conversation goes quiet.  No test bubble here — it would immediately
   * displace the pet home screen we want to demo. */
  pet_status_view_show();
  syslog(LOG_INFO, "[%s] pet status ring is the home screen\n", TAG);
#else
  /* Bring the chat screen to the foreground. */
  lvgl_ui_channel_show();

  /* Smoke bubble: proves the chat-view + MiSans CJK font render on the
   * real CP+LCD. voice_channel_speak() is a no-op stub on this side. */
  lvgl_ui_channel_send("CP真机 UI 测试气泡：ai_agent 界面已点亮");
#endif

  /* Open the AP→CP UI RPMsg endpoint.  Once the AP agent's link comes up,
   * bubbles/toasts it sends over RPMSG land here and are rendered by the
   * real lvgl_ui_channel_* / lvgl_toast_* on this core.  Idempotent. */
  ret = ui_rpmsg_server_init();
  if (ret != 0)
    {
      syslog(LOG_WARNING, "[%s] ui_rpmsg_server_init failed rc=%d "
                          "(CP renders locally; AP-driven bubbles disabled)\n",
             TAG, ret);
    }

  syslog(LOG_INFO, "[%s] UI up; LVGL thread running. Type 'quit' to exit.\n",
         TAG);

  /* The LVGL event-loop thread spawned by _start() is detached and keeps
   * rendering.  Keep this task alive so a manual NSH launch isn't reaped
   * and so the RPMsg-UI endpoint stays hosted.
   *
   * Ctrl-C is NOT a reliable exit here: in a flat build NSH does not deliver
   * SIGINT to the foreground builtin (CONFIG_TTY_SIGINT targets the
   * foreground group, but the builtin is not job-controlled by NSH).  So we
   * read stdin and quit on "quit"/"exit"/"q" — the foreground builtin owns
   * the console stdin, so this always reaches us, unlike the signal.  EOF
   * (stdin closed, e.g. launched with stdin redirected away) falls back to
   * pause() so the endpoint keeps being hosted. */
  for (;;)
    {
      char line[16];
      char *s;

      if (fgets(line, sizeof(line), stdin) == NULL)
        {
          /* stdin closed/unavailable — keep hosting the endpoint. */
          pause();
          continue;
        }

      s = line;
      while (*s == ' ' || *s == '\t')
        {
          s++;
        }

      if (strncmp(s, "quit", 4) == 0 || strncmp(s, "exit", 4) == 0 ||
          strncmp(s, "q", 1) == 0)
        {
          syslog(LOG_INFO, "[%s] quit received; tearing down UI\n", TAG);
          break;
        }
      /* Ignore other input; keep hosting. */
    }

  /* Graceful teardown (schedules widget deletion in the LVGL thread,
   * waits up to 2s).  Safe to skip on exit, but exercising the path
   * also verifies lvgl_ui_channel_stop works on real CP. */
  lvgl_ui_channel_stop();
  syslog(LOG_INFO, "[%s] UI stopped; returning to NSH\n", TAG);

  return 0;
}

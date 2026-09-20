# PC 侧 MCP server（remote_ctrl + vela 编译）

跑在 PC（宿主机）上的 MCP server，供设备端 ai_agent（mcp_client）经 HTTP 调用。
一次 `mcp_add` 同时拿到两组能力：

| 组 | 工具 | 用途 |
|----|------|------|
| 远程编码 | `rc_repo_open` / `rc_read_file` / `rc_write_patch` / `rc_run` / `rc_git`（push 禁用）/ `rc_summary` | 在 PC 仓库上读改跑提交（配合 mimocode / ACP bridge 的编码闭环） |
| vela 编译 | `vela_build` / `vela_build_status` | 在 PC 上发起 vela 固件编译并轮询结果 |

设备端 ai_agent 只发起与查询，编译产物和日志都留在 PC，不占用设备上下文与存储。

## 启动

```bash
cd <openvela workspace 根目录>
python3 packages/ai_agent/pc/remote_ctrl_mcp_server.py \
    --root . --host 0.0.0.0 --port 8760
# 可选: --token <secret>           # 开启 x-user-token 鉴权
# 可选: --jobs 32                  # vela_build 默认 -j
# 可选: --log-dir /tmp             # 编译日志目录
# 可选: --target bk7258-devkit-ap=vendor/beken/boards/bk7258/bk7258-devkit/configs/ap
#       （--target NAME=CONFIG_PATH 可重复传，扩展白名单）
```

内置 target 白名单：`goldfish-arm64-v8a-ap`、`qemu-arm64-v8a-ap`（映射到
`vendor/openvela/boards/vela/configs/<target>`）。白名单外的 target 一律拒绝。

## 设备端接入（qemu）

qemu guest 经 `10.0.2.2` 访问宿主。在 ai_agent CLI 中：

```
mcp_add pc http://10.0.2.2:8760/mcp
mcp_discover
```

配置持久化在 config store（`ai.mcp.server.<idx>.*`），重启后仍在。

设备端「编译 vela」skill（`agent_skills/vela-build.md`，随固件 builtin 分发）
会引导 agent 完成上述配置和调用。

## 为什么 vela_build 是异步的

设备侧 HTTP 读超时 = `AGENT_LLM_SOCKET_TIMEOUT_SEC` = 120 s，而 vela 编译
增量 ~35 s、clean 要几分钟——同步等待必然超时。所以：

```
ai_agent                    PC server
   │ vela_build {target}       │  校验白名单 → 起后台线程 → <1s 返回 started
   │──────────────────────────>│
   │<──────────────────────────│
   │ vela_build_status {}      │  （线程内: unset CROSSDEV; source build/envsetup.sh
   │──────────────────────────>│    && lunch <config> && m -jN，日志落 --log-dir）
   │<── running / success / …  │
   │  …每 30–60 s 轮询…        │
```

`vela_build_status` 返回 `state`（idle|running|success|failed）、`target`、
`elapsed`、`log_tail`（末 ~2.5 KB，失败时定位错误行）、成功时的 artifact
（`out/openvela_vela_<target>/vela_ap.bin` 等）。

## 安全模型

- **路径 confinement**：所有 rc_* 路径解析后必须落在 `--root` 内，逃逸（`../`）拒绝。
- **命令 allowlist**：`rc_run` 只允许白名单首 token（python3/pip/pytest/make/gcc/…），
  `shell=False` 逐参数执行，无管道/`&&`，带超时。
- **编译命令由 server 拼装**：`vela_build` 只接受 target 名（白名单校验），
  `lunch` 的 config 路径来自 server 端 target 表，agent 无法注入任意 shell。
- **push 禁用**：`rc_git` 不提供 push，推送走 Gerrit MCP（人工 review）。
- **可选 token**：`--token` 开启后所有请求须带 `x-user-token`。
- **同时只允许一个 build**：running 期间再次 `vela_build` 直接拒绝并提示轮询。

## 测试

```bash
# 起一个假 workspace + server
python3 pc/test_mcp.py                       # 需先起 server（见脚本头部注释）
```

`test_mcp.py` 覆盖：initialize/tools/list、rc_* 全套、allowlist 拦截（`rm`）、
路径逃逸拒绝、push 拒绝，以及 vela_build/status 的假构建全流程
（假 root + 假 build 脚本，不真编）。

## 已知边界

- server 单进程内共享一个 build 槽位（全局锁），多设备同时编同一 PC 会互相拒绝。
- 编译产物留在 PC 的 `out/openvela_vela_<target>/`，设备端只读状态文本。
- `log_tail` 截 2.5 KB，若需完整日志直接看 `--log-dir` 下的 `vela_build_<target>_<时间戳>.log`。

## BK7258 板上 media 框架可行性（spike 结论）

见 `agent_skills/vela-build.md` 所在仓库的提交说明 / 团队文档：板侧
`/dev/audio/pcm0p`、`pcm0c` 是标准 NuttX audio lower-half（media graph 经
aw-alsa-lib 消费的正是这类设备），但 media 音频路由依赖 mediad + ffmpeg
filter graph（alsasrc/alsasink）；BK7258 AP 为 336 KB RAM 的 ARMv8-M 核、
无 PSRAM，ffmpeg 不可行。板上音频维持 NuttX audio 框架，media 框架链路由
qemu（goldfish，media-only 播放+采集）承载。

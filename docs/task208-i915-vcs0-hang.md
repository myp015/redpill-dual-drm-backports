# Task#208：r1000 / DSM 7.2 / 4.4.302 的 VCS0 hang 现场关联诊断

## 目标与现场

在 r1000（CFL-S i3-8100T，8086:3e91）上通过 Emby VAAPI 转码时，`vcs0` 出现 hang。Task#208 的目标是通过可靠的单次现场采样，将硬件 ACTHD、实际执行上下文、request、batch 对象和等待条件关联起来；目前尚未确认根因，也未声称 hang 已修复。

## 已完成取证

- 前六轮现场诊断代码及对应 dmesg 已在 Git 历史中，最近一次诊断代码提交为 `dc51dab`；同步上游后的 HEAD 为 `81cacfe15346302e9a27b07d61ff7eadbf3efd9b`。
- 第六轮记录的 ACTHD 为 `0x1122b0`，对应的 PPGTT 软件页表 walk 显示 PTE present。该结果只能说明所用软件页表存在映射，不能单独证明采样页表就是硬件当时实际使用的 context。
- ring 中存在跨引擎 semaphore wait；采样到的相关 HWSP 值高于比较目标，且 status page preempt 值为 0。这些观测不支持采样时刻的简单“等待值不足”或“preempt 值非零”解释，但不排除其他 request、地址或时序上的等待问题。
- batch 窗口出现若干被解码为 RCS 命令的 opcode，是优先核查线索；由于 ACTHD 与 request/context/batch 对象尚未可靠关联，不据此推断命令非法或认定根因。
- `i915_cmd_parser.c` 的 parser 初始化条件不覆盖 CFL VCS；不能假设这次提交必经软件 parser，也不能把 parser 通过解释为完整嵌套 batch 链已验证。

## 当前诊断代码的可靠性问题

`drivers/gpu/drm/i915/gt/intel_hangcheck.c` 的旧版取证逻辑存在以下问题，旧版输出不能作为对象关联的最终证据：

- ACTHD 输出和 PPGTT 查询截断到 32 位；硬件地址没有作为一次一致的 64 位快照使用。
- `engine->active.lock` 内执行 printk，且将锁内取得的 request/context/ring 裸指针带到锁外继续解引用。
- 以 PTE 中 DMA 地址转换 CPU PFN，再用 `pfn_to_page()` / `kmap()` 读取 batch；DMA 地址不能普遍等同 CPU PFN，且没有从所属 GEM/VMA 建立对象生命周期。
- 扫描整个 HWSP 页并依据 active-list 首个 request 解释结果，容易把不同 timeline 或 request 的状态错误关联。
- active-list 首项不是硬件当前执行对象的证明；`READ_ONCE` 读取 execlists 指针也不自动保证所指对象生命周期。

## 已实施的安全快照收敛

在 `drivers/gpu/drm/i915/gt/intel_hangcheck.c` 中，诊断已改为：

- 只采集一次完整 64 位 ACTHD、RING_START/HEAD/TAIL/CTL 和 status page 的 preempt 值；末尾复读 ACTHD/HEAD 并记录采样是否稳定。
- active request 列表最多采样 8 个软件候选。在 `engine->active.lock` 内复制 seqno、HWSP 读值、signaled/started/completed 状态及 ring GGTT offset；锁外只打印标量，不再保留或解引用 request/context/ring 裸指针，也不在锁内打印。
- 所有软件候选日志均明确说明 ring 匹配不能证明硬件正在执行该 request。
- 删除 PPGTT 页表遍历、DMA→PFN→CPU 页读取、HWSP 整页扫描及关联不明的 ring/batch 内容 dump。
- inflight/pending 只记录是否存在，不输出未持引用的 request 指针。

这些改动只提高诊断采样的安全性和可解释性；当前仍不能从 hangcheck 快照将 ACTHD 关联到实际 request、context 或 batch，也没有确认根因。

## 验证与结论状态

代码静态检查已执行 `git diff --check`，通过；确认 hangcheck 中已无 `pfn_to_page()`、`pfn_valid()`、`kmap()`、HWSP 页扫描、锁外 request/context/ring 解引用或锁内 printk。尚未编译、尚未运行 GitHub Actions、尚未做新的实机复现。按项目约束不在本地编译；变更进入 CI 可见的提交后，使用 `/www/project/ainas-build/.github/workflows/test-drm4-radeon.yml` 验证 broadwellnk/geminilake/r1000 的 7.2 构建，并检查 r1000 artifact 确实包含预期诊断代码。实机验证需重启后只复现一次，保存完整 dmesg 和可用的 `/sys/class/drm/card0/error` 原始快照，再据 request/context/batch 关联情况判断根因。

验证结果只标记为“已确认”或“仍未关联/仍未确认”；模块构建成功不代表 hang 已修复，也不代表硬件转码通过。

## 第七轮：让 error capture 产出 engine/ring 段 + 首次 hang 的 ring 指令流

### 为什么之前 error 文件没有 engine 段

实机快照（`60b2de6`）显示 vcs0 的候选请求 `seqno=8 hwsp=5 started=0 ring_match=1`：
`intel_engine_find_active_request()` 的三重过滤（未完成 / 已开始 / RING_START 匹配）
只挂在“已开始”上（HW status page 的 seqno 滞后）。返回 NULL 后
`gem_record_rings()` 直接 `continue`，不创建 `drm_i915_error_engine`，
于是 `error->engine == NULL`：没有 `vcs0 command stream:`、没有 ringbuffer/batch，
`ecode` 也因 `i915_error_generate_code()` 取不到 engine 而恒为 0。

### 本轮改动

- `gt/intel_engine_cs.c`：抽出 `find_request(engine, require_started)`；
  `intel_engine_find_active_request()` 语义不变，新增 error capture 专用的
  `intel_engine_find_hung_request()`（未完成 + RING_START 匹配，去掉“已开始”）。
- `i915_gpu_error.c` `gem_record_rings()`：active 查询失败时退化为 hung 候选，
  并给 `ee->candidate` 打标；**每个 uabi engine 都会**链接 `ee`，
  无请求时置 `ee->no_request` 并只记录寄存器/execlist。
  `error_primary_engine()` 保证 `ecode` 与 `, in comm [pid]` 仍指向带请求的 engine。
- `gt/intel_hangcheck.c` `hangcheck_dump_engine()`：对 RING_START 匹配的请求
  `intel_ring_get()` 保活后，在锁外打印 HW `HEAD..TAIL` 窗口（最多 64 dword），
  并解码 HEAD 处的 `MI_BATCH_BUFFER_START`（GGTT/PPGTT + 目标地址）或
  `MI_SEMAPHORE_WAIT`（比较值/地址/POLL）。

### 安全约束

- 锁内只复制标量并 `kref_get`，不在锁内 printk；出锁后不解引用 request/context/vma 裸指针；
- ring 内容读取前校验 `vaddr`、`size`、4 字节对齐、`head < size`、`tail <= size`，
  回绕按两段处理，总量有上限；
- 不改变 hangcheck 判定与复位路径。

### 判读

- HEAD = `MI_BATCH_BUFFER_START`：GPU 卡在 batch 内部，用 error 文件里的 batch 内容
  与 ACTHD 定位具体命令；
- HEAD = `MI_SEMAPHORE_WAIT`：看比较值是否已被目标地址满足，区分提交/内存顺序问题；
- HEAD = 媒体命令本身：命令流或固件问题。
- `candidate` 只是软件候选（未 started），其 batch 与 ACTHD 不一致时以 ring 窗口为准。

本轮仍只做取证，不声称修复。

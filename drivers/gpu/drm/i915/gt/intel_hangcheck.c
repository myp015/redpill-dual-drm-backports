/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "i915_drv.h"
#include "intel_engine.h"
#include "intel_gt.h"
#include "intel_lrc_reg.h"
#include "intel_reset.h"

struct hangcheck {
	u64 acthd;
	u32 ring;
	u32 head;
	enum intel_engine_hangcheck_action action;
	unsigned long action_timestamp;
	int deadlock;
	struct intel_instdone instdone;
	bool wedged:1;
	bool stalled:1;
};

static bool instdone_unchanged(u32 current_instdone, u32 *old_instdone)
{
	u32 tmp = current_instdone | *old_instdone;
	bool unchanged;

	unchanged = tmp == *old_instdone;
	*old_instdone |= tmp;

	return unchanged;
}

static bool subunits_stuck(struct intel_engine_cs *engine)
{
	struct drm_i915_private *dev_priv = engine->i915;
	struct intel_instdone instdone;
	struct intel_instdone *accu_instdone = &engine->hangcheck.instdone;
	bool stuck;
	int slice;
	int subslice;

	intel_engine_get_instdone(engine, &instdone);

	/* There might be unstable subunit states even when
	 * actual head is not moving. Filter out the unstable ones by
	 * accumulating the undone -> done transitions and only
	 * consider those as progress.
	 */
	stuck = instdone_unchanged(instdone.instdone,
				   &accu_instdone->instdone);
	stuck &= instdone_unchanged(instdone.slice_common,
				    &accu_instdone->slice_common);

	for_each_instdone_slice_subslice(dev_priv, slice, subslice) {
		stuck &= instdone_unchanged(instdone.sampler[slice][subslice],
					    &accu_instdone->sampler[slice][subslice]);
		stuck &= instdone_unchanged(instdone.row[slice][subslice],
					    &accu_instdone->row[slice][subslice]);
	}

	return stuck;
}

static enum intel_engine_hangcheck_action
head_stuck(struct intel_engine_cs *engine, u64 acthd)
{
	if (acthd != engine->hangcheck.acthd) {

		/* Clear subunit states on head movement */
		memset(&engine->hangcheck.instdone, 0,
		       sizeof(engine->hangcheck.instdone));

		return ENGINE_ACTIVE_HEAD;
	}

	if (!subunits_stuck(engine))
		return ENGINE_ACTIVE_SUBUNITS;

	return ENGINE_DEAD;
}

static enum intel_engine_hangcheck_action
engine_stuck(struct intel_engine_cs *engine, u64 acthd)
{
	enum intel_engine_hangcheck_action ha;
	u32 tmp;

	ha = head_stuck(engine, acthd);
	if (ha != ENGINE_DEAD)
		return ha;

	if (IS_GEN(engine->i915, 2))
		return ENGINE_DEAD;

	/* Is the chip hanging on a WAIT_FOR_EVENT?
	 * If so we can simply poke the RB_WAIT bit
	 * and break the hang. This should work on
	 * all but the second generation chipsets.
	 */
	tmp = ENGINE_READ(engine, RING_CTL);
	if (tmp & RING_WAIT) {
		intel_gt_handle_error(engine->gt, engine->mask, 0,
				      "stuck wait on %s", engine->name);
		ENGINE_WRITE(engine, RING_CTL, tmp);
		return ENGINE_WAIT_KICK;
	}

	return ENGINE_DEAD;
}

static void hangcheck_load_sample(struct intel_engine_cs *engine,
				  struct hangcheck *hc)
{
	hc->acthd = intel_engine_get_active_head(engine);
	hc->ring = ENGINE_READ(engine, RING_START);
	hc->head = ENGINE_READ(engine, RING_HEAD);
}

static void hangcheck_store_sample(struct intel_engine_cs *engine,
				   const struct hangcheck *hc)
{
	engine->hangcheck.acthd = hc->acthd;
	engine->hangcheck.last_ring = hc->ring;
	engine->hangcheck.last_head = hc->head;
}

static enum intel_engine_hangcheck_action
hangcheck_get_action(struct intel_engine_cs *engine,
		     const struct hangcheck *hc)
{
	if (intel_engine_is_idle(engine))
		return ENGINE_IDLE;

	if (engine->hangcheck.last_ring != hc->ring)
		return ENGINE_ACTIVE_SEQNO;

	if (engine->hangcheck.last_head != hc->head)
		return ENGINE_ACTIVE_SEQNO;

	return engine_stuck(engine, hc->acthd);
}

static void hangcheck_accumulate_sample(struct intel_engine_cs *engine,
					struct hangcheck *hc)
{
	unsigned long timeout = I915_ENGINE_DEAD_TIMEOUT;

	hc->action = hangcheck_get_action(engine, hc);

	/* We always increment the progress
	 * if the engine is busy and still processing
	 * the same request, so that no single request
	 * can run indefinitely (such as a chain of
	 * batches). The only time we do not increment
	 * the hangcheck score on this ring, if this
	 * engine is in a legitimate wait for another
	 * engine. In that case the waiting engine is a
	 * victim and we want to be sure we catch the
	 * right culprit. Then every time we do kick
	 * the ring, make it as a progress as the seqno
	 * advancement might ensure and if not, it
	 * will catch the hanging engine.
	 */

	switch (hc->action) {
	case ENGINE_IDLE:
	case ENGINE_ACTIVE_SEQNO:
		/* Clear head and subunit states on seqno movement */
		hc->acthd = 0;

		memset(&engine->hangcheck.instdone, 0,
		       sizeof(engine->hangcheck.instdone));

		/* Intentional fall through */
	case ENGINE_WAIT_KICK:
	case ENGINE_WAIT:
		engine->hangcheck.action_timestamp = jiffies;
		break;

	case ENGINE_ACTIVE_HEAD:
	case ENGINE_ACTIVE_SUBUNITS:
		/*
		 * Seqno stuck with still active engine gets leeway,
		 * in hopes that it is just a long shader.
		 */
		timeout = I915_SEQNO_DEAD_TIMEOUT;
		break;

	case ENGINE_DEAD:
		break;

	default:
		MISSING_CASE(hc->action);
	}

	hc->stalled = time_after(jiffies,
				 engine->hangcheck.action_timestamp + timeout);
	hc->wedged = time_after(jiffies,
				 engine->hangcheck.action_timestamp +
				 I915_ENGINE_WEDGED_TIMEOUT);
}

/*
 * Task#208 诊断：vcs0 挂死时 capture 出来的 ecode 恒为 0，根因是
 * intel_engine_find_active_request() 在 active 队列里一个请求都没选中
 * （completed / 未 started / match_ring 失配三选一）。这里把每个请求
 * 的判据原样打出来，首次 hang 才打，避免复位循环刷屏。
 */
/* Task#208 第九轮：把"取哪个请求"和"采样它的哪些字段"分开。
 * 硬件真正在跑的请求可能不在 active.requests 里（preempt-to-busy 已把它
 * unwind 回 priolist），必须能从 execlists->active 槽单独采样。
 */
struct hangcheck_sample {
	struct i915_request *rq;
	struct intel_context *ce;
	u64 lrc_desc, vm_total;
	bool vm_4lvl;
	u32 ctx_ggtt, ring_ggtt;
	u32 lrc_head, lrc_tail, lrc_start, lrc_cc, lrc_rbc;
	u32 rq_head, rq_infix, rq_postfix, rq_tail, rq_wa_tail;
	u32 ring_head, ring_tail, ring_size, ring_eff, ring_wrap;
	u32 tl_seqno, tl_hwsp_offset, tl_hwsp_ggtt;
	s64 tl_hwsp_delta;
};

static void
hangcheck_sample_request(struct i915_request *rq, struct hangcheck_sample *s)
{
	memset(s, 0, sizeof(*s));
	s->rq = rq;
	s->ce = rq->hw_context;
	if (s->ce) {
		s->lrc_desc = s->ce->lrc_desc;
		s->ctx_ggtt = s->ce->state ? i915_ggtt_offset(s->ce->state) : 0;
		s->vm_total = s->ce->vm->total;
		s->vm_4lvl = i915_vm_is_4lvl(s->ce->vm);
		if (s->ce->lrc_reg_state) {
			s->lrc_head = s->ce->lrc_reg_state[CTX_RING_HEAD + 1];
			s->lrc_tail = s->ce->lrc_reg_state[CTX_RING_TAIL + 1];
			s->lrc_start = s->ce->lrc_reg_state[CTX_RING_BUFFER_START + 1];
			s->lrc_cc = s->ce->lrc_reg_state[CTX_CONTEXT_CONTROL + 1];
			s->lrc_rbc = s->ce->lrc_reg_state[CTX_RING_BUFFER_CONTROL + 1];
		}
	}
	if (rq->ring) {
		s->ring_ggtt = i915_ggtt_offset(rq->ring->vma);
		s->ring_head = rq->ring->head;
		s->ring_tail = rq->ring->tail;
		s->ring_size = rq->ring->size;
		s->ring_eff = rq->ring->effective_size;
		s->ring_wrap = rq->ring->wrap;
	}
	if (rq->timeline) {
		s->tl_seqno = rq->timeline->seqno;
		s->tl_hwsp_offset = rq->timeline->hwsp_offset;
		s->tl_hwsp_ggtt = i915_ggtt_offset(rq->timeline->hwsp_ggtt);
		s->tl_hwsp_delta = (s64)(unsigned long)rq->hwsp_seqno -
				   (s64)(unsigned long)rq->timeline->hwsp_seqno;
	}
	s->rq_head = rq->head;
	s->rq_infix = rq->infix;
	s->rq_postfix = rq->postfix;
	s->rq_tail = rq->tail;
	s->rq_wa_tail = rq->wa_tail;
}

static void hangcheck_dump_engine(struct intel_engine_cs *engine)
{
	struct drm_i915_private *i915 = engine->i915;
	struct intel_engine_execlists * const execlists = &engine->execlists;
	struct i915_request *rq, *first = NULL, *inflight = NULL;
	struct hangcheck_sample sample;
	unsigned long flags;
	u32 ring_dump[32], req_dump[16];
	int i;
	u32 ring_start, hw_head;
	int count = 0;
	int active_off;

	memset(&sample, 0, sizeof(sample));

	ring_start = ENGINE_READ(engine, RING_START);
	hw_head = ENGINE_READ(engine, RING_HEAD);
	dev_info(i915->drm.dev,
		 "hangcheck[%s]: ACTHD=%08x RING_START=%08x HEAD=%08x TAIL=%08x CTL=%08x\n",
		 engine->name,
		 lower_32_bits(intel_engine_get_active_head(engine)),
		 ring_start,
		 hw_head,
		 ENGINE_READ(engine, RING_TAIL),
		 ENGINE_READ(engine, RING_CTL));
	dev_info(i915->drm.dev,
		 "hangcheck[%s]: hwsp cpu=%llx ggtt=%08x\n",
		 engine->name,
		 (u64)(unsigned long)engine->status_page.addr,
		 i915_ggtt_offset(engine->status_page.vma));

	/*
	 * Task#208 第十轮：第九轮的取样对象没错，错在被
	 * i915_request_completed() 挡掉。实测 rcs0 上
	 * *execlists->active == execlists->inflight[0] == ffff880637206f80
	 * （seqno 14，硬件 RING_START=0x7f2000 HEAD=0x1b8 ACTHD=0x70b128），
	 * 但该请求已被提前 fence signal 标记完成 —— 僵尸请求：软件认为它
	 * 完成了，硬件仍在执行。因此不能按「已完成」跳过，僵尸请求正是
	 * 要看的现场。旁证：error capture 只有 vcs0 段（seqno 2 未完成），
	 * rcs0 段完全缺失。
	 * 取源以 *active 为主（语义上就是硬件正在跑的槽），inflight[0] 兜底
	 * （active 是指针，会被 execlists_schedule_out(*active++) 推进）。
	 *
	 * 这里不调 execlists_active()：它带 GEM_BUG_ON(active - inflight >
	 * num_ports)，hang 现场 active 可能已越过哨兵。锁序：tasklet lock 必须
	 * 先于 active.lock 取得/释放，不能嵌套在它里面。
	 */
	execlists_active_lock_bh(execlists);
	inflight = READ_ONCE(*execlists->active);
	if (!inflight)
		inflight = READ_ONCE(execlists->inflight[0]);
	active_off = (int)(execlists->active - execlists->inflight);
	if (inflight)
		i915_request_get(inflight);
	execlists_active_unlock_bh(execlists);

	if (inflight) {
		first = inflight;
		hangcheck_sample_request(first, &sample);
		dev_info(i915->drm.dev,
			 "hangcheck[%s]: sample=inflight rq=%p seqno=%u ring=%08x act=%d done=%d\n",
			 engine->name, first, first->fence.seqno, sample.ring_ggtt,
			 active_off, i915_request_completed(first));
	}

	spin_lock_irqsave(&engine->active.lock, flags);
	list_for_each_entry(rq, &engine->active.requests, sched.link) {
		u32 ring = rq->ring ? i915_ggtt_offset(rq->ring->vma) : 0;
		u32 off = rq->timeline ? rq->timeline->hwsp_offset : 0;
		u32 ggtt = rq->timeline ?
			i915_ggtt_offset(rq->timeline->hwsp_ggtt) : 0;

		if (count++ >= 8) {
			dev_info(i915->drm.dev, "hangcheck[%s]: ... (more)\n",
				 engine->name);
			break;
		}

		/* 打出每个请求的 timeline 槽归属：ring 里的 MI_SEMAPHORE_WAIT
		 * 等的是某个具体 cacheline（第八阶段停在 0x100），这里钉死它属于谁。
		 */
		dev_info(i915->drm.dev,
			 "hangcheck[%s]: rq=%p[%s] seqno=%u hwsp=%llx:%u tl{seq=%u off=%08x ggtt=%08x} ring=%08x match=%d sig=%d st=%d done=%d\n",
			 engine->name, rq,
			 rq == inflight ? "inflight" : "active",
			 rq->fence.seqno,
			 (u64)(unsigned long)rq->hwsp_seqno,
			 READ_ONCE(*rq->hwsp_seqno),
			 rq->timeline ? rq->timeline->seqno : 0,
			 off, ggtt,
			 ring, ring == ring_start,
			 i915_request_signaled(rq),
			 i915_request_started(rq),
			 i915_request_completed(rq));

		if (!first) {
			first = rq;
			hangcheck_sample_request(rq, &sample);
		}
	}
	spin_unlock_irqrestore(&engine->active.lock, flags);

	dev_info(i915->drm.dev,
		 "hangcheck[%s]: %d active request(s); inflight %p,%p pending %p,%p prio=%d\n",
		 engine->name, count,
		 READ_ONCE(execlists->inflight[0]),
		 READ_ONCE(execlists->inflight[1]),
		 READ_ONCE(execlists->pending[0]),
		 READ_ONCE(execlists->pending[1]),
		 READ_ONCE(execlists->queue_priority_hint));

	if (!first)
		goto out;

	dev_info(i915->drm.dev,
		 "hangcheck[%s]: ce=%p lrc_desc=%016llx ctx_ggtt=%08x lrc_head=%08x lrc_tail=%08x lrc_start=%08x\n",
		 engine->name, sample.ce, sample.lrc_desc, sample.ctx_ggtt,
		 sample.lrc_head, sample.lrc_tail, sample.lrc_start);
	dev_info(i915->drm.dev,
		 "hangcheck[%s]: rq head=%08x infix=%08x postfix=%08x tail=%08x wa_tail=%08x\n",
		 engine->name, sample.rq_head, sample.rq_infix, sample.rq_postfix,
		 sample.rq_tail, sample.rq_wa_tail);
	dev_info(i915->drm.dev,
		 "hangcheck[%s]: ring head=%08x tail=%08x size=%08x eff=%08x wrap=%08x ggtt=%08x\n",
		 engine->name, sample.ring_head, sample.ring_tail, sample.ring_size,
		 sample.ring_eff, sample.ring_wrap, sample.ring_ggtt);
	dev_info(i915->drm.dev,
		 "hangcheck[%s]: vm total=%llx 4lvl=%d lrc_cc=%08x lrc_rbc=%08x\n",
		 engine->name, sample.vm_total, sample.vm_4lvl,
		 sample.lrc_cc, sample.lrc_rbc);
	dev_info(i915->drm.dev,
		 "hangcheck[%s]: tl seqno=%u hwsp_off=%08x hwsp_ggtt=%08x hwsp_delta=%lld\n",
		 engine->name, sample.tl_seqno, sample.tl_hwsp_offset,
		 sample.tl_hwsp_ggtt, sample.tl_hwsp_delta);
	{
		struct i915_request *f0 = READ_ONCE(execlists->inflight[0]);
		struct i915_request *f1 = READ_ONCE(execlists->inflight[1]);

		dev_info(i915->drm.dev,
			 "hangcheck[%s]: inflight[0]=%p(seqno %u) [1]=%p(seqno %u) active_off=%d\n",
			 engine->name, f0, f0 ? f0->fence.seqno : 0,
			 f1, f1 ? f1->fence.seqno : 0, active_off);
	}

	/*
	 * Task#208 第五轮：前四轮已经把 ring 指令流与 context 都排除了，
	 * 剩下的可能是硬件在用户态 batch（PPGTT 地址）里挂住。这里核对
	 * context image 里写的 PML4 与软件 ppgtt 根是否一致，并沿页表链
	 * 查 ACTHD 是否真的映射（取指失败 vs 指令等待）。
	 */
	if (sample.ce && sample.ce->vm && sample.ce->vm != &i915->ggtt.vm) {
		struct intel_context *ce = sample.ce; /* 段内沿用原写法 */
		struct i915_ppgtt *ppgtt = i915_vm_to_ppgtt(ce->vm);
		u64 acthd = lower_32_bits(intel_engine_get_active_head(engine));
		u64 pml4_dma = px_dma(ppgtt->pd);
		u64 ctx_pml4 = 0;

		if (ce->lrc_reg_state)
			ctx_pml4 = ((u64)ce->lrc_reg_state[CTX_PDP0_UDW + 1] << 32) |
				   ce->lrc_reg_state[CTX_PDP0_LDW + 1];

		dev_info(i915->drm.dev,
			 "hangcheck[%s]: ppgtt top=%u pml4_dma=%llx ctx_pml4=%llx %s dma_mask=%llx\n",
			 engine->name, ce->vm->top,
			 (unsigned long long)pml4_dma,
			 (unsigned long long)ctx_pml4,
			 ctx_pml4 == pml4_dma ? "match" : "MISMATCH",
			 ce->vm->dma && ce->vm->dma->dma_mask ?
			 (unsigned long long)*ce->vm->dma->dma_mask : 0ull);

		if (ppgtt->pd->pt.base.page) {
			const u64 *v = kmap(ppgtt->pd->pt.base.page);

			dev_info(i915->drm.dev,
				 "hangcheck[%s]: pml4[0..7]=%016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx\n",
				 engine->name,
				 (unsigned long long)v[0], (unsigned long long)v[1],
				 (unsigned long long)v[2], (unsigned long long)v[3],
				 (unsigned long long)v[4], (unsigned long long)v[5],
				 (unsigned long long)v[6], (unsigned long long)v[7]);
			kunmap(ppgtt->pd->pt.base.page);
		}

		if (ce->vm->top == 3) {
			unsigned int i3 = (acthd >> 39) & 511;
			unsigned int i2 = (acthd >> 30) & 511;
			unsigned int i1 = (acthd >> 21) & 511;
			unsigned int i0 = (acthd >> 12) & 511;
			struct i915_page_directory *pdp, *pd;
			struct i915_page_table *pt;

			pdp = i915_pd_entry(ppgtt->pd, i3);
			dev_info(i915->drm.dev,
				 "hangcheck[%s]: acthd=%llx pml4e[%u]=%p pdp_dma=%llx\n",
				 engine->name, (unsigned long long)acthd, i3, pdp,
				 pdp ? (unsigned long long)px_dma(pdp) : 0ull);
			if (pdp) {
				pd = i915_pd_entry(pdp, i2);
				dev_info(i915->drm.dev,
					 "hangcheck[%s]: pdpe[%u]=%p pd_dma=%llx\n",
					 engine->name, i2, pd,
					 pd ? (unsigned long long)px_dma(pd) : 0ull);
				if (pd) {
					pt = i915_pt_entry(pd, i1);
					dev_info(i915->drm.dev,
						 "hangcheck[%s]: pde[%u]=%p pt_dma=%llx\n",
						 engine->name, i1, pt,
						 pt ? (unsigned long long)px_dma(pt) : 0ull);
					if (pt && pt->base.page) {
						const u64 *p = kmap(pt->base.page);
						u64 pte = p[i0];
						u64 pte_phys = pte & 0x000ffffffffff000ull;

						dev_info(i915->drm.dev,
							 "hangcheck[%s]: pte[%u]=%016llx present=%d phys=%llx\n",
							 engine->name, i0,
							 (unsigned long long)pte,
							 (int)(pte & 1),
							 (unsigned long long)pte_phys);
						kunmap(pt->base.page);

						/*
						 * Task#208 第六轮：PTE 已 present ⇒ 不是缺页。
						 * 直接把 ACTHD 所在页的指令打出来，看硬件停的
						 * 是 MI_WAIT_FOR_EVENT、MI_SEMAPHORE_WAIT 还是
						 * 别的媒体指令。
						 */
						if ((pte & 1) &&
						    pfn_valid(pte_phys >> PAGE_SHIFT)) {
							struct page *bp = pfn_to_page(pte_phys >> PAGE_SHIFT);
							const u32 *b = kmap(bp);
							u32 o = (u32)(acthd & ~3u & (PAGE_SIZE - 1));
							u32 k;

							/* 第九轮：从 4 行扩到 8 行，看 ACTHD 前后的指令上下文 */
							for (k = 0; k < 8; k++) {
								u32 base = (o + k * 32) & (PAGE_SIZE - 1);

								dev_info(i915->drm.dev,
									 "hangcheck[%s]: @batch%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
									 engine->name, base,
									 b[((base >> 2) + 0) & 1023],
									 b[((base >> 2) + 1) & 1023],
									 b[((base >> 2) + 2) & 1023],
									 b[((base >> 2) + 3) & 1023],
									 b[((base >> 2) + 4) & 1023],
									 b[((base >> 2) + 5) & 1023],
									 b[((base >> 2) + 6) & 1023],
									 b[((base >> 2) + 7) & 1023]);
							}
							kunmap(bp);
						}
					}
				}
			}
		}
	}

	/*
	 * Task#208 第六轮：HWSP 页里各 timeline 的 seqno（ring 里的
	 * MI_SEMAPHORE_WAIT 就在等这一页某个 cacheline 的值），以及
	 * 引擎 status page 的 preempt 地址（fini breadcrumb 结尾的
	 * busywait 目标，等它归零）。
	 */
	if (first && first->hwsp_seqno) {
		const u32 *h = (const u32 *)
			((unsigned long)first->hwsp_seqno & PAGE_MASK);
		const u32 *sp = (const u32 *)engine->status_page.addr;
		int k;

		dev_info(i915->drm.dev,
			 "hangcheck[%s]: hwsp page=%p self=%03x val=%08x preempt_off=%03x preempt=%08x\n",
			 engine->name, h,
			 (u32)((unsigned long)first->hwsp_seqno & ~PAGE_MASK),
			 READ_ONCE(*first->hwsp_seqno),
			 (u32)I915_GEM_HWS_PREEMPT_ADDR,
			 sp ? READ_ONCE(sp[I915_GEM_HWS_PREEMPT_ADDR / 4]) : ~0u);
		for (k = 0; k < 64; k++) {
			u32 v = READ_ONCE(h[k * 16]);

			if (v)
				dev_info(i915->drm.dev,
					 "hangcheck[%s]: hwsp[%03x]=%08x\n",
					 engine->name, k * 0x40, v);
		}
	}

	if (sample.rq && sample.rq->ring && sample.rq->ring->vaddr &&
	    sample.ring_size) {
		u32 mask = sample.ring_size - 1;
		u32 hw = hw_head & mask & ~3u;
		u32 rq_pos = sample.rq_head & mask & ~3u;

		for (i = 0; i < ARRAY_SIZE(ring_dump); i++)
			ring_dump[i] = *(const u32 *)
				((const u8 *)sample.rq->ring->vaddr +
				 ((hw + i * 4) & mask));
		for (i = 0; i < ARRAY_SIZE(req_dump); i++)
			req_dump[i] = *(const u32 *)
				((const u8 *)sample.rq->ring->vaddr +
				 ((rq_pos + i * 4) & mask));

		/* ring 前半段：硬件 HEAD 之前的指令流（含本请求的 bb_start 目标） */
		for (i = 0; i < 0x100; i += 32) {
			const u32 *s = (const u32 *)
				((const u8 *)sample.rq->ring->vaddr + i);

			dev_info(i915->drm.dev,
				 "hangcheck[%s]: @%04x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
				 engine->name, i,
				 s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
		}

		dev_info(i915->drm.dev,
			 "hangcheck[%s]: @hw%04x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			 engine->name, hw,
			 ring_dump[0], ring_dump[1], ring_dump[2], ring_dump[3],
			 ring_dump[4], ring_dump[5], ring_dump[6], ring_dump[7]);
		dev_info(i915->drm.dev,
			 "hangcheck[%s]: @hw%04x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			 engine->name, (hw + 32) & mask,
			 ring_dump[8], ring_dump[9], ring_dump[10], ring_dump[11],
			 ring_dump[12], ring_dump[13], ring_dump[14], ring_dump[15]);
		dev_info(i915->drm.dev,
			 "hangcheck[%s]: @hw%04x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			 engine->name, (hw + 64) & mask,
			 ring_dump[16], ring_dump[17], ring_dump[18], ring_dump[19],
			 ring_dump[20], ring_dump[21], ring_dump[22], ring_dump[23]);
		dev_info(i915->drm.dev,
			 "hangcheck[%s]: @hw%04x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			 engine->name, (hw + 96) & mask,
			 ring_dump[24], ring_dump[25], ring_dump[26], ring_dump[27],
			 ring_dump[28], ring_dump[29], ring_dump[30], ring_dump[31]);
		dev_info(i915->drm.dev,
			 "hangcheck[%s]: @rq%04x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			 engine->name, rq_pos,
			 req_dump[0], req_dump[1], req_dump[2], req_dump[3],
			 req_dump[4], req_dump[5], req_dump[6], req_dump[7]);
		dev_info(i915->drm.dev,
			 "hangcheck[%s]: @rq%04x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			 engine->name, (rq_pos + 32) & mask,
			 req_dump[8], req_dump[9], req_dump[10], req_dump[11],
			 req_dump[12], req_dump[13], req_dump[14], req_dump[15]);
	}

	/* 硬件侧：CSB 与 ELSP 回显（只读，与复位路径无交互） */
	dev_info(i915->drm.dev,
		 "hangcheck[%s]: csb head=%u write=%08x status=%08x %08x %08x %08x\n",
		 engine->name, execlists->csb_head,
		 READ_ONCE(*execlists->csb_write),
		 READ_ONCE(execlists->csb_status[0]),
		 READ_ONCE(execlists->csb_status[1]),
		 READ_ONCE(execlists->csb_status[2]),
		 READ_ONCE(execlists->csb_status[3]));
	dev_info(i915->drm.dev,
		 "hangcheck[%s]: ELSP_STATUS=%08x:%08x ELSP_SQ_CONTENTS=%08x:%08x\n",
		 engine->name,
		 ENGINE_READ(engine, RING_EXECLIST_STATUS_HI),
		 ENGINE_READ(engine, RING_EXECLIST_STATUS_LO),
		 intel_uncore_read(engine->uncore,
				   _MMIO(engine->mmio_base + 0x514)),
		 ENGINE_READ(engine, RING_EXECLIST_SQ_CONTENTS));

out:
	if (inflight)
		i915_request_put(inflight);
}

/* Task#208 第九轮：每次开机最多抓几次 hang 现场（原 first_error 短路
 * 在首次复位后永久生效，实测导致第二轮实验拿不到任何 dump）。
 */
static int hangcheck_dumps;

static void hangcheck_declare_hang(struct intel_gt *gt,
				   intel_engine_mask_t hung,
				   intel_engine_mask_t stuck)
{
	struct intel_engine_cs *engine;
	intel_engine_mask_t tmp;
	char msg[80];
	int len;

	/* If some rings hung but others were still busy, only
	 * blame the hanging rings in the synopsis.
	 */
	if (stuck != hung)
		hung &= ~stuck;
	len = scnprintf(msg, sizeof(msg),
			"%s on ", stuck == hung ? "no progress" : "hang");
	for_each_engine_masked(engine, gt->i915, hung, tmp)
		len += scnprintf(msg + len, sizeof(msg) - len,
				 "%s, ", engine->name);
	msg[len-2] = '\0';

	/* Task#208 第九轮：首次复位后 first_error 即非空，后续 hang 会被永久
	 * 短路（只有重启才清零）；改为每模块最多抓 3 次现场。
	 */
	if (++hangcheck_dumps <= 3)
		for_each_engine_masked(engine, gt->i915, hung, tmp)
			hangcheck_dump_engine(engine);

	return intel_gt_handle_error(gt, hung, I915_ERROR_CAPTURE, "%s", msg);
}

/*
 * This is called when the chip hasn't reported back with completed
 * batchbuffers in a long time. We keep track per ring seqno progress and
 * if there are no progress, hangcheck score for that ring is increased.
 * Further, acthd is inspected to see if the ring is stuck. On stuck case
 * we kick the ring. If we see no progress on three subsequent calls
 * we assume chip is wedged and try to fix it by resetting the chip.
 */
static void hangcheck_elapsed(struct work_struct *work)
{
	struct intel_gt *gt =
		container_of(work, typeof(*gt), hangcheck.work.work);
	intel_engine_mask_t hung = 0, stuck = 0, wedged = 0;
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	intel_wakeref_t wakeref;

	if (!i915_modparams.enable_hangcheck)
		return;

	if (!READ_ONCE(gt->awake))
		return;

	if (intel_gt_is_wedged(gt))
		return;

	wakeref = intel_runtime_pm_get_if_in_use(&gt->i915->runtime_pm);
	if (!wakeref)
		return;

	/* As enabling the GPU requires fairly extensive mmio access,
	 * periodically arm the mmio checker to see if we are triggering
	 * any invalid access.
	 */
	intel_uncore_arm_unclaimed_mmio_detection(gt->uncore);

	for_each_engine(engine, gt->i915, id) {
		struct hangcheck hc;

		intel_engine_signal_breadcrumbs(engine);

		hangcheck_load_sample(engine, &hc);
		hangcheck_accumulate_sample(engine, &hc);
		hangcheck_store_sample(engine, &hc);

		if (hc.stalled) {
			hung |= engine->mask;
			if (hc.action != ENGINE_DEAD)
				stuck |= engine->mask;
		}

		if (hc.wedged)
			wedged |= engine->mask;
	}

	if (GEM_SHOW_DEBUG() && (hung | stuck)) {
		struct drm_printer p = drm_debug_printer("hangcheck");

		for_each_engine(engine, gt->i915, id) {
			if (intel_engine_is_idle(engine))
				continue;

			intel_engine_dump(engine, &p, "%s\n", engine->name);
		}
	}

	if (wedged) {
		dev_err(gt->i915->drm.dev,
			"GPU recovery timed out,"
			" cancelling all in-flight rendering.\n");
		GEM_TRACE_DUMP();
		intel_gt_set_wedged(gt);
	}

	if (hung)
		hangcheck_declare_hang(gt, hung, stuck);

	intel_runtime_pm_put(&gt->i915->runtime_pm, wakeref);

	/* Reset timer in case GPU hangs without another request being added */
	intel_gt_queue_hangcheck(gt);
}

void intel_gt_queue_hangcheck(struct intel_gt *gt)
{
	unsigned long delay;

	if (unlikely(!i915_modparams.enable_hangcheck))
		return;

	/*
	 * Don't continually defer the hangcheck so that it is always run at
	 * least once after work has been scheduled on any ring. Otherwise,
	 * we will ignore a hung ring if a second ring is kept busy.
	 */

	delay = round_jiffies_up_relative(DRM_I915_HANGCHECK_JIFFIES);
	queue_delayed_work(system_long_wq, &gt->hangcheck.work, delay);
}

void intel_engine_init_hangcheck(struct intel_engine_cs *engine)
{
	memset(&engine->hangcheck, 0, sizeof(engine->hangcheck));
	engine->hangcheck.action_timestamp = jiffies;
}

void intel_gt_init_hangcheck(struct intel_gt *gt)
{
	INIT_DELAYED_WORK(&gt->hangcheck.work, hangcheck_elapsed);
}

#if IS_ENABLED(CONFIG_DRM_I915_SELFTEST)
#include "selftest_hangcheck.c"
#endif

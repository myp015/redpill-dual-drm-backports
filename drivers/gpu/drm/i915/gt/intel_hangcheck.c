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
 * Task#208：采集有界快照。active 链表项只是软件候选，不能证明 ACTHD
 * 对应的硬件执行上下文。
 */
static void hangcheck_dump_engine(struct intel_engine_cs *engine)
{
	struct drm_i915_private *i915 = engine->i915;
	struct intel_engine_execlists * const execlists = &engine->execlists;
	struct {
		u64 seqno;
		u32 hwsp;
		u32 ring;
		bool signaled;
		bool started;
		bool completed;
	} rq_snapshot[8];
	unsigned long flags;
	u64 acthd, acthd_after;
	u32 ring_start, hw_head, ring_tail, ring_ctl, hw_head_after;
	unsigned int count = 0, i;
	bool more = false;
	u32 preempt = ~0u;

	acthd = intel_engine_get_active_head(engine);
	ring_start = ENGINE_READ(engine, RING_START);
	hw_head = ENGINE_READ(engine, RING_HEAD);
	ring_tail = ENGINE_READ(engine, RING_TAIL);
	ring_ctl = ENGINE_READ(engine, RING_CTL);
	if (engine->status_page.addr)
		preempt = intel_read_status_page(engine, I915_GEM_HWS_PREEMPT);

	spin_lock_irqsave(&engine->active.lock, flags);
	{
		struct i915_request *rq;

		list_for_each_entry(rq, &engine->active.requests, sched.link) {
			if (count == ARRAY_SIZE(rq_snapshot)) {
				more = true;
				break;
			}

			rq_snapshot[count].seqno = rq->fence.seqno;
			rq_snapshot[count].hwsp = hwsp_seqno(rq);
			rq_snapshot[count].signaled = i915_request_signaled(rq);
			rq_snapshot[count].started = i915_request_started(rq);
			rq_snapshot[count].completed = i915_request_completed(rq);
			rq_snapshot[count].ring = rq->ring ?
				i915_ggtt_offset(rq->ring->vma) : 0;
			count++;
		}
	}
	spin_unlock_irqrestore(&engine->active.lock, flags);

	dev_info(i915->drm.dev,
		 "hangcheck[%s]: snapshot ACTHD=%016llx RING_START=%08x HEAD=%08x TAIL=%08x CTL=%08x preempt=%08x\n",
		 engine->name, (unsigned long long)acthd, ring_start, hw_head,
		 ring_tail, ring_ctl, preempt);
	dev_info(i915->drm.dev,
		 "hangcheck[%s]: software request candidates shown=%u more=%d ring_match_does_not_prove_execution\n",
		 engine->name, count, more);

	for (i = 0; i < count; i++) {
		dev_info(i915->drm.dev,
			 "hangcheck[%s]: candidate[%u] seqno=%llu hwsp=%08x signaled=%d started=%d completed=%d ring=%08x ring_match=%d\n",
			 engine->name, i,
			 (unsigned long long)rq_snapshot[i].seqno,
			 rq_snapshot[i].hwsp, rq_snapshot[i].signaled,
			 rq_snapshot[i].started, rq_snapshot[i].completed,
			 rq_snapshot[i].ring,
			 rq_snapshot[i].ring == ring_start);
	}

	dev_info(i915->drm.dev,
		 "hangcheck[%s]: inflight_present=%d,%d pending_present=%d,%d prio=%d\n",
		 engine->name,
		 !!READ_ONCE(execlists->inflight[0]),
		 !!READ_ONCE(execlists->inflight[1]),
		 !!READ_ONCE(execlists->pending[0]),
		 !!READ_ONCE(execlists->pending[1]),
		 READ_ONCE(execlists->queue_priority_hint));
	dev_info(i915->drm.dev,
		 "hangcheck[%s]: hwsp_ggtt=%08x preempt_off=%03x csb head=%u write=%08x status=%08x %08x %08x %08x\n",
		 engine->name, i915_ggtt_offset(engine->status_page.vma),
		 (u32)I915_GEM_HWS_PREEMPT_ADDR, execlists->csb_head,
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

	acthd_after = intel_engine_get_active_head(engine);
	hw_head_after = ENGINE_READ(engine, RING_HEAD);
	dev_info(i915->drm.dev,
		 "hangcheck[%s]: end ACTHD=%016llx HEAD=%08x stable=%d\n",
		 engine->name, (unsigned long long)acthd_after, hw_head_after,
		 acthd == acthd_after && hw_head == hw_head_after);
}

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

	/* Task#208：只抓首次 hang 的现场，后续复位循环由 first_error 短路 */
	if (!READ_ONCE(gt->i915->gpu_error.first_error))
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

/*
 * Copyright 2009 Jerome Glisse.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 */
/*
 * Authors:
 *    Jerome Glisse <glisse@freedesktop.org>
 *    Dave Airlie
 */

#include <linux/atomic.h>
#include <linux/firmware.h>
#include <linux/hw_breakpoint.h>
#include <linux/kref.h>
#include <linux/moduleparam.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/thread_info.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <asm/pgtable.h>
#include <asm/ptrace.h>

#include <drm/drm_debugfs.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>

#include "radeon.h"
#include "radeon_reg.h"
#include "radeon_trace.h"

/* Task#209 取证桩的前向声明（实现在 radeon_fence_wait_seq_timeout 之前） */
static void radeon_fence_spy_check(struct radeon_device *rdev, const char *where);

/*
 * Fences
 * Fences mark an event in the GPUs pipeline and are used
 * for GPU/CPU synchronization.  When the fence is written,
 * it is expected that all buffers associated with that fence
 * are no longer in use by the associated ring on the GPU and
 * that the the relevant GPU caches have been flushed.  Whether
 * we use a scratch register or memory location depends on the asic
 * and whether writeback is enabled.
 */

/**
 * radeon_fence_write - write a fence value
 *
 * @rdev: radeon_device pointer
 * @seq: sequence number to write
 * @ring: ring index the fence is associated with
 *
 * Writes a fence value to memory or a scratch register (all asics).
 */
static void radeon_fence_write(struct radeon_device *rdev, u32 seq, int ring)
{
	struct radeon_fence_driver *drv = &rdev->fence_drv[ring];
	if (likely(rdev->wb.enabled || !drv->scratch_reg)) {
		if (drv->cpu_addr) {
			*drv->cpu_addr = cpu_to_le32(seq);
		}
	} else {
		WREG32(drv->scratch_reg, seq);
	}
}

/**
 * radeon_fence_read - read a fence value
 *
 * @rdev: radeon_device pointer
 * @ring: ring index the fence is associated with
 *
 * Reads a fence value from memory or a scratch register (all asics).
 * Returns the value of the fence read from memory or register.
 */
static u32 radeon_fence_read(struct radeon_device *rdev, int ring)
{
	struct radeon_fence_driver *drv = &rdev->fence_drv[ring];
	u32 seq = 0;

	if (likely(rdev->wb.enabled || !drv->scratch_reg)) {
		if (drv->cpu_addr) {
			seq = le32_to_cpu(*drv->cpu_addr);
		} else {
			seq = lower_32_bits(atomic64_read(&drv->last_seq));
		}
	} else {
		seq = RREG32(drv->scratch_reg);
	}
	return seq;
}

/**
 * radeon_fence_schedule_check - schedule lockup check
 *
 * @rdev: radeon_device pointer
 * @ring: ring index we should work with
 *
 * Queues a delayed work item to check for lockups.
 */
static void radeon_fence_schedule_check(struct radeon_device *rdev, int ring)
{
	/*
	 * Do not reset the timer here with mod_delayed_work,
	 * this can livelock in an interaction with TTM delayed destroy.
	 */
	queue_delayed_work(system_power_efficient_wq,
			   &rdev->fence_drv[ring].lockup_work,
			   RADEON_FENCE_JIFFIES_TIMEOUT);
}

/**
 * radeon_fence_emit - emit a fence on the requested ring
 *
 * @rdev: radeon_device pointer
 * @fence: radeon fence object
 * @ring: ring index the fence is associated with
 *
 * Emits a fence command on the requested ring (all asics).
 * Returns 0 on success, -ENOMEM on failure.
 */
int radeon_fence_emit(struct radeon_device *rdev,
		      struct radeon_fence **fence,
		      int ring)
{
	u64 seq;

	/* we are protected by the ring emission mutex */
	*fence = kmalloc(sizeof(struct radeon_fence), GFP_KERNEL);
	if ((*fence) == NULL) {
		return -ENOMEM;
	}
	(*fence)->rdev = rdev;
	(*fence)->seq = seq = ++rdev->fence_drv[ring].sync_seq[ring];
	(*fence)->ring = ring;
	(*fence)->is_vm_update = false;
	dma_fence_init(&(*fence)->base, &radeon_fence_ops,
		       &rdev->fence_queue.lock,
		       rdev->fence_context + ring,
		       seq);
	radeon_fence_ring_emit(rdev, ring, *fence);
	trace_radeon_fence_emit(rdev->ddev, ring, (*fence)->seq);
	radeon_fence_schedule_check(rdev, ring);
	return 0;
}

/**
 * radeon_fence_check_signaled - callback from fence_queue
 *
 * this function is called with fence_queue lock held, which is also used
 * for the fence locking itself, so unlocked variants are used for
 * fence_signal, and remove_wait_queue.
 */
static int radeon_fence_check_signaled(wait_queue_entry_t *wait, unsigned mode, int flags, void *key)
{
	struct radeon_fence *fence;
	u64 seq;

	fence = container_of(wait, struct radeon_fence, fence_wake);

	/*
	 * We cannot use radeon_fence_process here because we're already
	 * in the waitqueue, in a call from wake_up_all.
	 */
	seq = atomic64_read(&fence->rdev->fence_drv[fence->ring].last_seq);
	if (seq >= fence->seq) {
		int ret = dma_fence_signal_locked(&fence->base);

		if (!ret)
			DMA_FENCE_TRACE(&fence->base, "signaled from irq context\n");
		else
			DMA_FENCE_TRACE(&fence->base, "was already signaled\n");

		radeon_irq_kms_sw_irq_put(fence->rdev, fence->ring);
		__remove_wait_queue(&fence->rdev->fence_queue, &fence->fence_wake);
		dma_fence_put(&fence->base);
	} else
		DMA_FENCE_TRACE(&fence->base, "pending\n");
	return 0;
}

/**
 * radeon_fence_activity - check for fence activity
 *
 * @rdev: radeon_device pointer
 * @ring: ring index the fence is associated with
 *
 * Checks the current fence value and calculates the last
 * signalled fence value. Returns true if activity occured
 * on the ring, and the fence_queue should be waken up.
 */
static bool radeon_fence_activity(struct radeon_device *rdev, int ring)
{
	uint64_t seq, last_seq, last_emitted;
	unsigned count_loop = 0;
	bool wake = false;

	/* Note there is a scenario here for an infinite loop but it's
	 * very unlikely to happen. For it to happen, the current polling
	 * process need to be interrupted by another process and another
	 * process needs to update the last_seq btw the atomic read and
	 * xchg of the current process.
	 *
	 * More over for this to go in infinite loop there need to be
	 * continuously new fence signaled ie radeon_fence_read needs
	 * to return a different value each time for both the currently
	 * polling process and the other process that xchg the last_seq
	 * btw atomic read and xchg of the current process. And the
	 * value the other process set as last seq must be higher than
	 * the seq value we just read. Which means that current process
	 * need to be interrupted after radeon_fence_read and before
	 * atomic xchg.
	 *
	 * To be even more safe we count the number of time we loop and
	 * we bail after 10 loop just accepting the fact that we might
	 * have temporarly set the last_seq not to the true real last
	 * seq but to an older one.
	 */
	last_seq = atomic64_read(&rdev->fence_drv[ring].last_seq);
	do {
		last_emitted = rdev->fence_drv[ring].sync_seq[ring];
		seq = radeon_fence_read(rdev, ring);
		seq |= last_seq & 0xffffffff00000000LL;
		if (seq < last_seq) {
			seq &= 0xffffffff;
			seq |= last_emitted & 0xffffffff00000000LL;
		}

		if (seq <= last_seq || seq > last_emitted) {
			break;
		}
		/* If we loop over we don't want to return without
		 * checking if a fence is signaled as it means that the
		 * seq we just read is different from the previous on.
		 */
		wake = true;
		last_seq = seq;
		if ((count_loop++) > 10) {
			/* We looped over too many time leave with the
			 * fact that we might have set an older fence
			 * seq then the current real last seq as signaled
			 * by the hw.
			 */
			break;
		}
	} while (atomic64_xchg(&rdev->fence_drv[ring].last_seq, seq) > seq);

	if (seq < last_emitted)
		radeon_fence_schedule_check(rdev, ring);

	return wake;
}

/**
 * radeon_fence_check_lockup - check for hardware lockup
 *
 * @work: delayed work item
 *
 * Checks for fence activity and if there is none probe
 * the hardware if a lockup occured.
 */
static void radeon_fence_check_lockup(struct work_struct *work)
{
	struct radeon_fence_driver *fence_drv;
	struct radeon_device *rdev;
	int ring;

	fence_drv = container_of(work, struct radeon_fence_driver,
				 lockup_work.work);
	rdev = fence_drv->rdev;
	ring = fence_drv - &rdev->fence_drv[0];

	if (!down_read_trylock(&rdev->exclusive_lock)) {
		/* just reschedule the check if a reset is going on */
		radeon_fence_schedule_check(rdev, ring);
		return;
	}

	if (fence_drv->delayed_irq && rdev->ddev->irq_enabled) {
		unsigned long irqflags;

		fence_drv->delayed_irq = false;
		spin_lock_irqsave(&rdev->irq.lock, irqflags);
		radeon_irq_set(rdev);
		spin_unlock_irqrestore(&rdev->irq.lock, irqflags);
	}

	radeon_fence_spy_check(rdev, "lockup-enter");

	if (radeon_fence_activity(rdev, ring)) {
		radeon_fence_spy_check(rdev, "lockup-before-wake");
		wake_up_all(&rdev->fence_queue);
		radeon_fence_spy_check(rdev, "lockup-after-wake");

	} else if (radeon_ring_is_lockup(rdev, ring, &rdev->ring[ring])) {

		/* good news we believe it's a lockup */
		dev_warn(rdev->dev, "GPU lockup (current fence id "
			 "0x%016llx last fence id 0x%016llx on ring %d)\n",
			 (uint64_t)atomic64_read(&fence_drv->last_seq),
			 fence_drv->sync_seq[ring], ring);

		/* remember that we need an reset */
		rdev->needs_reset = true;
		radeon_fence_spy_check(rdev, "lockup-reset-before-wake");
		wake_up_all(&rdev->fence_queue);
		radeon_fence_spy_check(rdev, "lockup-reset-after-wake");
	}
	up_read(&rdev->exclusive_lock);
}

/**
 * radeon_fence_process - process a fence
 *
 * @rdev: radeon_device pointer
 * @ring: ring index the fence is associated with
 *
 * Checks the current fence value and wakes the fence queue
 * if the sequence number has increased (all asics).
 */
void radeon_fence_process(struct radeon_device *rdev, int ring)
{
	if (radeon_fence_activity(rdev, ring)) {
		radeon_fence_spy_check(rdev, "process-before-wake");
		wake_up_all(&rdev->fence_queue);
		radeon_fence_spy_check(rdev, "process-after-wake");
	}
}

/**
 * radeon_fence_seq_signaled - check if a fence sequence number has signaled
 *
 * @rdev: radeon device pointer
 * @seq: sequence number
 * @ring: ring index the fence is associated with
 *
 * Check if the last signaled fence sequnce number is >= the requested
 * sequence number (all asics).
 * Returns true if the fence has signaled (current fence value
 * is >= requested value) or false if it has not (current fence
 * value is < the requested value.  Helper function for
 * radeon_fence_signaled().
 */
static bool radeon_fence_seq_signaled(struct radeon_device *rdev,
				      u64 seq, unsigned ring)
{
	if (atomic64_read(&rdev->fence_drv[ring].last_seq) >= seq) {
		return true;
	}
	/* poll new last sequence at least once */
	radeon_fence_process(rdev, ring);
	if (atomic64_read(&rdev->fence_drv[ring].last_seq) >= seq) {
		return true;
	}
	return false;
}

static bool radeon_fence_is_signaled(struct dma_fence *f)
{
	struct radeon_fence *fence = to_radeon_fence(f);
	struct radeon_device *rdev = fence->rdev;
	unsigned ring = fence->ring;
	u64 seq = fence->seq;

	if (atomic64_read(&rdev->fence_drv[ring].last_seq) >= seq) {
		return true;
	}

	if (down_read_trylock(&rdev->exclusive_lock)) {
		radeon_fence_process(rdev, ring);
		up_read(&rdev->exclusive_lock);

		if (atomic64_read(&rdev->fence_drv[ring].last_seq) >= seq) {
			return true;
		}
	}
	return false;
}

/**
 * radeon_fence_enable_signaling - enable signalling on fence
 * @fence: fence
 *
 * This function is called with fence_queue lock held, and adds a callback
 * to fence_queue that checks if this fence is signaled, and if so it
 * signals the fence and removes itself.
 */
static bool radeon_fence_enable_signaling(struct dma_fence *f)
{
	struct radeon_fence *fence = to_radeon_fence(f);
	struct radeon_device *rdev = fence->rdev;

	if (atomic64_read(&rdev->fence_drv[fence->ring].last_seq) >= fence->seq)
		return false;

	if (down_read_trylock(&rdev->exclusive_lock)) {
		radeon_irq_kms_sw_irq_get(rdev, fence->ring);

		if (radeon_fence_activity(rdev, fence->ring)) {
			radeon_fence_spy_check(rdev, "enable-sig-before-wake");
			wake_up_all_locked(&rdev->fence_queue);
			radeon_fence_spy_check(rdev, "enable-sig-after-wake");
		}

		/* did fence get signaled after we enabled the sw irq? */
		if (atomic64_read(&rdev->fence_drv[fence->ring].last_seq) >= fence->seq) {
			radeon_irq_kms_sw_irq_put(rdev, fence->ring);
			up_read(&rdev->exclusive_lock);
			return false;
		}

		up_read(&rdev->exclusive_lock);
	} else {
		/* we're probably in a lockup, lets not fiddle too much */
		if (radeon_irq_kms_sw_irq_get_delayed(rdev, fence->ring))
			rdev->fence_drv[fence->ring].delayed_irq = true;
		radeon_fence_schedule_check(rdev, fence->ring);
	}

	fence->fence_wake.flags = 0;
	fence->fence_wake.private = NULL;
	fence->fence_wake.func = radeon_fence_check_signaled;
	__add_wait_queue(&rdev->fence_queue, &fence->fence_wake);
	dma_fence_get(f);

	DMA_FENCE_TRACE(&fence->base, "armed on ring %i!\n", fence->ring);
	return true;
}

/**
 * radeon_fence_signaled - check if a fence has signaled
 *
 * @fence: radeon fence object
 *
 * Check if the requested fence has signaled (all asics).
 * Returns true if the fence has signaled or false if it has not.
 */
bool radeon_fence_signaled(struct radeon_fence *fence)
{
	if (!fence)
		return true;

	if (radeon_fence_seq_signaled(fence->rdev, fence->seq, fence->ring)) {
		int ret;

		ret = dma_fence_signal(&fence->base);
		if (!ret)
			DMA_FENCE_TRACE(&fence->base, "signaled from radeon_fence_signaled\n");
		return true;
	}
	return false;
}

/**
 * radeon_fence_any_seq_signaled - check if any sequence number is signaled
 *
 * @rdev: radeon device pointer
 * @seq: sequence numbers
 *
 * Check if the last signaled fence sequnce number is >= the requested
 * sequence number (all asics).
 * Returns true if any has signaled (current value is >= requested value)
 * or false if it has not. Helper function for radeon_fence_wait_seq.
 */
static bool radeon_fence_any_seq_signaled(struct radeon_device *rdev, u64 *seq)
{
	unsigned i;

	for (i = 0; i < RADEON_NUM_RINGS; ++i) {
		if (seq[i] && radeon_fence_seq_signaled(rdev, seq[i], i))
			return true;
	}
	return false;
}

/*
 * Task#209 回归取证 v2（仅诊断版本，不改变等待语义）
 *
 * 现象：4.4.302 实机在 radeon_ib_ring_tests 的 fence 等待路径上，把 radeon
 * 模块 .text 中 radeon_fence_any_seq_signaled 的 8 字节指令槽写成栈地址
 * （原版与 Task#209 补丁版都会中，三台构建一致落在 +0x4b：该处正是
 *  `call radeon_fence_activity` 的返回地址槽），随后执行到坏指令触发
 * 内核 oops，udevd worker 被杀、drm_global_mutex 再不复位。
 *
 * 已由 oops 数据反推出的写入特征：写入值 = 某等待项 task_list 的地址
 * （任务栈上），目标 = 代码地址。这正是 list_add/list_del 在“队列头/
 * 节点的 next/prev 已被污染成代码地址（例如残留的返回地址）”时的典型表现。
 *
 * 因此这里在每个等待检查点做三件事：
 *   1) 用模块加载时的函数体快照比对 radeon_fence_any_seq_signaled，
 *      **在真正执行该函数之前**比对，避免先执行坏指令再取证；
 *   2) 打印 fence_queue 队列头的 next/prev 并按 code/dmap/bad 分类；
 *   3) 沿队列最多走 8 个节点：节点 base 是否落在当前任务栈内、func 是否落在
 *      代码段、private 指向的任务 comm、next/prev 的分类，全部用
 *      probe_kernel_read() 安全读取（坏地址只返回 -EFAULT，不会二次 oops）。
 * 关键检查点已埋在最可能做链表写的位置两侧：`process-before-wake` /
 * `process-after-wake`、`lockup-before-wake` / `lockup-after-wake`，
 * 可以判定野写是否发生在 wake_up_all() 的链表遍历里。
 * 心跳类检查点（cond-before / post-wait-* / post-recheck / lockup-enter）
 * 限速到 50ms 一条，建立时间轴又不淹没现场；带 wake 的检查点一律打印。
 * 日志条数上限 RADEON_SPY_LOG_MAX，耗尽后仍继续做 .text 比对。
 */
#define RADEON_SPY_LEN		0x60
#define RADEON_SPY_LOG_MAX	600
#define RADEON_SPY_MAX_NODES	8
#define RADEON_SPY_HB_MS	50	/* 心跳类检查点限速：每 50ms 最多一条 */
static void *radeon_spy_ref;
static int radeon_spy_hit;
static int radeon_spy_lines;
static unsigned long radeon_spy_last_hb;
static int radeon_spy_param = 1;
module_param_named(t209_spy, radeon_spy_param, int, 0644);
MODULE_PARM_DESC(t209_spy, "Task#209 fence 野写取证（默认 1）");

/*
 * Task#209 第二轮取证：写断点。
 *
 * 采样式检查点只能证明「野写发生在哪个窗口」，无法指出「谁写的」。这里改用
 * x86 调试寄存器在野写目标上挂 W 型硬件断点（DR0-DR3，全 CPU 生效）：任何
 * 上下文对该地址的写都会触发 #DB，处理器把写入指令之后的 RIP 与当时的寄存
 * 器、当前内核栈一并交给我们，于是可以直接读出写入者是谁。
 *
 * 目标地址是 radeon_fence_any_seq_signaled 里 `call radeon_fence_activity`
 * 的返回地址槽（实测在 +0x4B），被写坏的就是这 8 字节。x86 的 LEN=8 数据断点
 * 要求地址对齐，该地址未对齐，因此改用 4 字节窗口覆盖其高 4 字节：
 * 任何覆盖该槽的 4/8 字节写都会与窗口重叠而被命中。
 *
 * 野写也可能走直接映射别名（模块 .text 的物理页在 PAGE_OFFSET 处的映射），
 * 硬件断点比较的是线性地址，所以同时在别名上挂同样的断点。
 */
#define RADEON_SPY_BP_MAX	32
#define RADEON_SPY_BP_FRAMES	8

struct radeon_spy_bp_rec {
	unsigned long ip;	/* 命中后的 RIP（写入指令的下一条） */
	unsigned long sp;
	unsigned long bp;
	unsigned long ax, bx, cx, dx, si, di;
	unsigned long frame[RADEON_SPY_BP_FRAMES];
	int pid;
	int cpu;
	char comm[TASK_COMM_LEN];
};

static struct radeon_spy_bp_rec radeon_spy_bp[RADEON_SPY_BP_MAX];
static atomic_t radeon_spy_bp_cnt = ATOMIC_INIT(0);
static int radeon_spy_bp_printed;
static struct perf_event * __percpu *radeon_spy_bp_ev[2];
static int radeon_spy_bp_tried;
static int radeon_spy_bp_param = 1;
module_param_named(t209_bp, radeon_spy_bp_param, int, 0644);
MODULE_PARM_DESC(t209_bp, "Task#209 野写硬件写断点取证（默认 1）");

/* 沿帧指针链抓调用者返回地址；只走当前任务自己的内核栈，越界即停 */
static void radeon_spy_bp_walk(struct radeon_spy_bp_rec *r, unsigned long fp)
{
	unsigned long lo = (unsigned long)task_stack_page(current);
	unsigned long hi = lo + THREAD_SIZE;
	int i;

	for (i = 0; i < RADEON_SPY_BP_FRAMES; i++) {
		unsigned long next, ret;

		if (fp < lo || fp + 16 > hi || (fp & 7))
			break;
		next = *(unsigned long *)fp;
		ret = *(unsigned long *)(fp + 8);
		r->frame[i] = ret;
		if (next <= fp)
			break;
		fp = next;
	}
}

/* #DB 上下文：只记录，不打印（printk 在异常上下文里有死锁风险） */
static void radeon_spy_bp_handler(struct perf_event *bp,
				  struct perf_sample_data *data,
				  struct pt_regs *regs)
{
	int idx = atomic_inc_return(&radeon_spy_bp_cnt) - 1;
	struct radeon_spy_bp_rec *r;

	if (idx >= RADEON_SPY_BP_MAX)
		return;
	r = &radeon_spy_bp[idx];
	r->ip = instruction_pointer(regs);
	r->sp = regs->sp;
	r->bp = regs->bp;
	r->ax = regs->ax;
	r->bx = regs->bx;
	r->cx = regs->cx;
	r->dx = regs->dx;
	r->si = regs->si;
	r->di = regs->di;
	r->pid = current->pid;
	r->cpu = smp_processor_id();
	memcpy(r->comm, current->comm, TASK_COMM_LEN);
	radeon_spy_bp_walk(r, regs->bp);
}

/* 进程上下文（检查点）里把尚未打印的记录打出来 */
static void radeon_spy_bp_dump(void)
{
	int n = atomic_read(&radeon_spy_bp_cnt);
	int i, j;

	if (n > RADEON_SPY_BP_MAX)
		n = RADEON_SPY_BP_MAX;
	for (i = radeon_spy_bp_printed; i < n; i++) {
		struct radeon_spy_bp_rec *r = &radeon_spy_bp[i];

		pr_err("T209BP[%d] ip=%pS sp=%lx bp=%lx ax=%lx bx=%lx cx=%lx dx=%lx si=%lx di=%lx cpu=%d pid=%d comm=%s\n",
		       i, (void *)r->ip, r->sp, r->bp, r->ax, r->bx,
		       r->cx, r->dx, r->si, r->di, r->cpu, r->pid, r->comm);
		for (j = 0; j < RADEON_SPY_BP_FRAMES && r->frame[j]; j++)
			pr_err("T209BP[%d]   调用者%d=%pS(%lx)\n",
			       i, j, (void *)r->frame[j], r->frame[j]);
	}
	radeon_spy_bp_printed = n;
}

static void radeon_spy_bp_install_one(int slot, unsigned long addr)
{
	struct perf_event_attr attr;

	if (radeon_spy_bp_ev[slot])
		return;
	hw_breakpoint_init(&attr);
	attr.bp_addr = addr;
	attr.bp_len = HW_BREAKPOINT_LEN_4;
	attr.bp_type = HW_BREAKPOINT_W;
	radeon_spy_bp_ev[slot] = register_wide_hw_breakpoint(&attr,
							     radeon_spy_bp_handler,
							     NULL);
	if (IS_ERR_OR_NULL(radeon_spy_bp_ev[slot])) {
		long err = IS_ERR(radeon_spy_bp_ev[slot]) ?
			   PTR_ERR(radeon_spy_bp_ev[slot]) : -ENOSYS;

		pr_err("T209BP 断点%d 注册失败 addr=%lx err=%ld\n", slot, addr, err);
		radeon_spy_bp_ev[slot] = NULL;
		return;
	}
	pr_err("T209BP 断点%d 已挂 addr=%lx\n", slot, addr);
}

/*
 * 野写打中的是 any_seq_signaled 里 `call radeon_fence_activity` 的返回地址槽。
 * 偏移不写死：在函数体里找那条目标为 radeon_fence_activity 的 e8 相对调用，
 * 返回地址的偏移就是它的位置+5。这样编译器改动不会让断点挂错地方。
 */
static unsigned long radeon_spy_bp_find_off(unsigned long fn)
{
	const unsigned char *p = (const unsigned char *)fn;
	int i;

	for (i = 0; i + 5 <= RADEON_SPY_LEN; i++) {
		s32 rel;

		if (p[i] != 0xe8)
			continue;
		memcpy(&rel, p + i + 1, sizeof(rel));
		if (fn + i + 5 + rel ==
		    (unsigned long)(void *)radeon_fence_activity)
			return i + 5;
	}
	return 0;
}

/*
 * 在野写目标上挂写断点。窗口取目标地址之后第一个 4 字节对齐处：
 * 野写要覆盖那 8 字节返回地址槽，必然与该窗口重叠。
 */
static void radeon_spy_bp_install(void)
{
	unsigned long fn = (unsigned long)(void *)radeon_fence_any_seq_signaled;
	unsigned long off = radeon_spy_bp_find_off(fn);
	unsigned long tgt = fn + off;
	unsigned long win;
	unsigned int level = 0;
	pte_t *pte;

	if (!radeon_spy_bp_param || radeon_spy_bp_tried)
		return;
	radeon_spy_bp_tried = 1;

	if (!off) {
		pr_err("T209BP 未能在 any_seq_signaled 里找到目标调用，放弃挂断点\n");
		return;
	}
	win = (tgt + 4) & ~3UL;
	pr_err("T209BP 目标 fn=%lx off=%lx tgt=%lx window=%lx\n",
	       fn, off, tgt, win);

	radeon_spy_bp_install_one(0, win);

	/* 同一物理页在直接映射里的别名：野写也可能从别名的线性地址打进来 */
	pte = lookup_address(win & PAGE_MASK, &level);
	if (pte && (pte_val(*pte) & _PAGE_PRESENT)) {
		unsigned long pa = (unsigned long)pte_val(*pte) & PTE_PFN_MASK;
		unsigned long alias = (unsigned long)__va(pa) +
				      (win & ~PAGE_MASK);

		radeon_spy_bp_install_one(1, alias);
	} else {
		pr_err("T209BP 别名解析失败 win=%lx pte=%p level=%u\n",
		       win, pte, level);
	}
}

static void radeon_spy_bp_remove(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(radeon_spy_bp_ev); i++) {
		if (!radeon_spy_bp_ev[i])
			continue;
		unregister_wide_hw_breakpoint(radeon_spy_bp_ev[i]);
		radeon_spy_bp_ev[i] = NULL;
	}
}

/*
 * 一切不可信的指针都用 probe_kernel_read() 读取：它在坏地址上走 fault
 * fixup 只返回 -EFAULT，不会二次 oops；4.4 的 linux/mm.h 里没有模块可见
 * 的 pfn_valid() 声明，不能用它做校验。
 */
static bool radeon_spy_is_dmap(unsigned long v)
{
	return v >= 0xffff880000000000UL && v < 0xffffc80000000000UL;
}

static const char *radeon_spy_kind(unsigned long v)
{
	if (!v)
		return "nil";
	if (v >= 0xffffffff80000000UL)
		return "code";		/* 内核/模块代码段 */
	if (radeon_spy_is_dmap(v))
		return "dmap";		/* 直接映射：栈/堆 */
	return "bad";
}

/* 等待项的 private 一般是 task_struct*，把 comm 读出来便于认出残留项属于谁 */
static void radeon_spy_task_comm(unsigned long task, char *out, size_t outlen)
{
	char buf[TASK_COMM_LEN];

	out[0] = '\0';
	if (!radeon_spy_is_dmap(task))
		return;
	if (probe_kernel_read(buf, (void *)(task + offsetof(struct task_struct, comm)),
			      sizeof(buf)))
		return;
	buf[sizeof(buf) - 1] = '\0';
	strncpy(out, buf, outlen - 1);
	out[outlen - 1] = '\0';
}

static void radeon_spy_dump_queue(struct radeon_device *rdev, const char *where)
{
	struct list_head *head = &rdev->fence_queue.task_list;
	unsigned long hn = (unsigned long)head->next;
	unsigned long hp = (unsigned long)head->prev;
	unsigned long lo = (unsigned long)task_stack_page(current);
	unsigned long hi = lo + THREAD_SIZE;
	unsigned long cur;
	int i;

	if (radeon_spy_lines >= RADEON_SPY_LOG_MAX)
		return;
	radeon_spy_lines++;

	pr_err("T209SPY[%s] q=%lx nx=%lx(%s) pv=%lx(%s) pid=%d comm=%s irq=%d\n",
	       where, (unsigned long)head, hn, radeon_spy_kind(hn),
	       hp, radeon_spy_kind(hp), current->pid, current->comm,
	       in_interrupt());

	if (hn == (unsigned long)head && hp == (unsigned long)head)
		return;			/* 空队列 */
	if ((hn == (unsigned long)head) != (hp == (unsigned long)head))
		pr_err("T209SPY[%s] !!! 队列头 next/prev 单边自指\n", where);

	for (i = 0, cur = hn; i < RADEON_SPY_MAX_NODES; i++) {
		unsigned long base = cur - offsetof(wait_queue_entry_t, task_list);
		wait_queue_entry_t node;
		char comm[TASK_COMM_LEN];

		if (cur == (unsigned long)head)
			break;
		if (probe_kernel_read(&node, (void *)base, sizeof(node))) {
			pr_err("T209SPY[%s] !!! 项%d 节点=%lx 不可读（base=%lx）\n",
			       where, i, cur, base);
			break;
		}
		radeon_spy_task_comm((unsigned long)node.private, comm, sizeof(comm));
		pr_err("T209SPY[%s]   项%d 节点=%lx base=%lx 本栈=%d fl=%x func=%lx(%s) priv=%lx(%s) next=%lx(%s) prev=%lx(%s)\n",
		       where, i, cur, base, (base >= lo && base < hi),
		       node.flags,
		       (unsigned long)node.func,
		       radeon_spy_kind((unsigned long)node.func),
		       (unsigned long)node.private, comm[0] ? comm : "-",
		       (unsigned long)node.task_list.next,
		       radeon_spy_kind((unsigned long)node.task_list.next),
		       (unsigned long)node.task_list.prev,
		       radeon_spy_kind((unsigned long)node.task_list.prev));
		if (node.task_list.next == cur ||
		    node.task_list.next == head)
			break;		/* 自指或已回到队头，避免绕坏指针打转 */
		cur = (unsigned long)node.task_list.next;
	}
}

static void radeon_fence_spy_check(struct radeon_device *rdev, const char *where)
{
	unsigned long fn = (unsigned long)(void *)radeon_fence_any_seq_signaled;

	/* 写断点先出结果：任何上下文命中过就立刻打印 */
	radeon_spy_bp_dump();

	if (!radeon_spy_param)
		return;

	/* .text 快照比对：即便日志配额用尽也必须做 */
	if (radeon_spy_ref && !radeon_spy_hit &&
	    memcmp((void *)fn, radeon_spy_ref, RADEON_SPY_LEN) != 0) {
		radeon_spy_hit = 1;
		pr_err("T209SPY[%s] .text 被改写: fn=%lx pid=%d comm=%s irq=%d\n",
		       where, fn, current->pid, current->comm, in_interrupt());
		print_hex_dump(KERN_ERR, "T209SPY ref : ", DUMP_PREFIX_OFFSET, 16, 1,
			       radeon_spy_ref, RADEON_SPY_LEN, false);
		print_hex_dump(KERN_ERR, "T209SPY live: ", DUMP_PREFIX_OFFSET, 16, 1,
			       (void *)fn, RADEON_SPY_LEN, false);
		dump_stack();
	}

	/*
	 * 分片等待每秒会走几百次心跳检查点，全部打印会淹没现场并耗光日志配额，
	 * 因此对心跳类检查点限速到 50ms 一条；凡是名字里带 wake 的唤醒路径
	 * 检查点一律打印，它们是定位野写发生窗口的关键。
	 */
	if (!strstr(where, "wake") &&
	    time_before(jiffies,
			radeon_spy_last_hb + msecs_to_jiffies(RADEON_SPY_HB_MS)))
		return;
	radeon_spy_last_hb = jiffies;

	radeon_spy_dump_queue(rdev, where);
}

/* 等待条件包一层：先取证、再执行 any_seq（防止执行已被改写的指令） */
static bool radeon_fence_wait_cond(struct radeon_device *rdev, u64 *seq)
{
	bool signaled;

	radeon_fence_spy_check(rdev, "cond-before");
	signaled = radeon_fence_any_seq_signaled(rdev, seq) || rdev->needs_reset;
	/* 把野写窗口再切一刀：any_seq 返回后立刻复核 .text */
	radeon_fence_spy_check(rdev, "cond-after");
	return signaled;
}

/**
 * radeon_fence_wait_seq_timeout - wait for a specific sequence numbers
 *
 * @rdev: radeon device pointer
 * @target_seq: sequence number(s) we want to wait for
 * @intr: use interruptable sleep
 * @timeout: maximum time to wait, or MAX_SCHEDULE_TIMEOUT for infinite wait
 *
 * Wait for the requested sequence number(s) to be written by any ring
 * (all asics).  Sequnce number array is indexed by ring id.
 * @intr selects whether to use interruptable (true) or non-interruptable
 * (false) sleep when waiting for the sequence number.  Helper function
 * for radeon_fence_wait_*().
 * Returns remaining time if the sequence number has passed, 0 when
 * the wait timeout, or an error for all other cases.
 * -EDEADLK is returned when a GPU lockup has been detected.
 */
static long radeon_fence_wait_seq_timeout(struct radeon_device *rdev,
					  u64 *target_seq, bool intr,
					  long timeout)
{
	long r;
	int i;

	radeon_fence_spy_check(rdev, "enter");
	if (radeon_fence_any_seq_signaled(rdev, target_seq))
		return timeout;
	radeon_fence_spy_check(rdev, "post-precheck");

	/* enable IRQs and tracing */
	for (i = 0; i < RADEON_NUM_RINGS; ++i) {
		if (!target_seq[i])
			continue;

		trace_radeon_fence_wait_begin(rdev->ddev, i, target_seq[i]);
		radeon_irq_kms_sw_irq_get(rdev, i);
	}

	/*
	 * Task#209：按短分片循环求值。无 fence 中断的环（TN/ARUBA 的 VCE）
	 * 在两次 lockup_work 之间没有任何唤醒，单次长等待会白睡到下一个
	 * HZ/2 tick。条件求值本身会主动读硬件 fence 值，分片只是让它有机会
	 * 被重新求值。
	 */
	do {
		long slice = min_t(long, timeout,
				   RADEON_FENCE_WAIT_SLICE_JIFFIES);

		if (intr) {
			r = wait_event_interruptible_timeout(rdev->fence_queue,
				radeon_fence_wait_cond(rdev, target_seq), slice);
		} else {
			r = wait_event_timeout(rdev->fence_queue,
				radeon_fence_wait_cond(rdev, target_seq), slice);
		}
		radeon_fence_spy_check(rdev, r > 0 ? "post-wait-sig" :
				       (r < 0 ? "post-wait-intr" : "post-wait-to"));

		if (unlikely(r < 0))
			break;		/* 被信号打断 */

		if (r > 0)
			break;		/* 条件已满足 */

		/* 本片刚好用完：主动重求值，覆盖 race */
		if (radeon_fence_any_seq_signaled(rdev, target_seq) ||
		    rdev->needs_reset)
			break;
		radeon_fence_spy_check(rdev, "post-recheck");

		if (timeout == MAX_SCHEDULE_TIMEOUT)
			continue;

		timeout -= slice;
		if (timeout <= 0) {
			r = 0;		/* 总超时 */
			break;
		}
	} while (1);

	if (rdev->needs_reset)
		r = -EDEADLK;

	for (i = 0; i < RADEON_NUM_RINGS; ++i) {
		if (!target_seq[i])
			continue;

		radeon_irq_kms_sw_irq_put(rdev, i);
		trace_radeon_fence_wait_end(rdev->ddev, i, target_seq[i]);
	}

	return r;
}

/**
 * radeon_fence_wait_timeout - wait for a fence to signal with timeout
 *
 * @fence: radeon fence object
 * @intr: use interruptible sleep
 *
 * Wait for the requested fence to signal (all asics).
 * @intr selects whether to use interruptable (true) or non-interruptable
 * (false) sleep when waiting for the fence.
 * @timeout: maximum time to wait, or MAX_SCHEDULE_TIMEOUT for infinite wait
 * Returns remaining time if the sequence number has passed, 0 when
 * the wait timeout, or an error for all other cases.
 */
long radeon_fence_wait_timeout(struct radeon_fence *fence, bool intr, long timeout)
{
	uint64_t seq[RADEON_NUM_RINGS] = {};
	long r;
	int r_sig;

	/*
	 * This function should not be called on !radeon fences.
	 * If this is the case, it would mean this function can
	 * also be called on radeon fences belonging to another card.
	 * exclusive_lock is not held in that case.
	 */
	if (WARN_ON_ONCE(!to_radeon_fence(&fence->base)))
		return dma_fence_wait(&fence->base, intr);

	seq[fence->ring] = fence->seq;
	r = radeon_fence_wait_seq_timeout(fence->rdev, seq, intr, timeout);
	if (r <= 0) {
		return r;
	}

	r_sig = dma_fence_signal(&fence->base);
	if (!r_sig)
		DMA_FENCE_TRACE(&fence->base, "signaled from fence_wait\n");
	return r;
}

/**
 * radeon_fence_wait - wait for a fence to signal
 *
 * @fence: radeon fence object
 * @intr: use interruptible sleep
 *
 * Wait for the requested fence to signal (all asics).
 * @intr selects whether to use interruptable (true) or non-interruptable
 * (false) sleep when waiting for the fence.
 * Returns 0 if the fence has passed, error for all other cases.
 */
int radeon_fence_wait(struct radeon_fence *fence, bool intr)
{
	long r = radeon_fence_wait_timeout(fence, intr, MAX_SCHEDULE_TIMEOUT);
	if (r > 0) {
		return 0;
	} else {
		return r;
	}
}

/**
 * radeon_fence_wait_any - wait for a fence to signal on any ring
 *
 * @rdev: radeon device pointer
 * @fences: radeon fence object(s)
 * @intr: use interruptable sleep
 *
 * Wait for any requested fence to signal (all asics).  Fence
 * array is indexed by ring id.  @intr selects whether to use
 * interruptable (true) or non-interruptable (false) sleep when
 * waiting for the fences. Used by the suballocator.
 * Returns 0 if any fence has passed, error for all other cases.
 */
int radeon_fence_wait_any(struct radeon_device *rdev,
			  struct radeon_fence **fences,
			  bool intr)
{
	uint64_t seq[RADEON_NUM_RINGS];
	unsigned i, num_rings = 0;
	long r;

	for (i = 0; i < RADEON_NUM_RINGS; ++i) {
		seq[i] = 0;

		if (!fences[i]) {
			continue;
		}

		seq[i] = fences[i]->seq;
		++num_rings;
	}

	/* nothing to wait for ? */
	if (num_rings == 0)
		return -ENOENT;

	r = radeon_fence_wait_seq_timeout(rdev, seq, intr, MAX_SCHEDULE_TIMEOUT);
	if (r < 0) {
		return r;
	}
	return 0;
}

/**
 * radeon_fence_wait_next - wait for the next fence to signal
 *
 * @rdev: radeon device pointer
 * @ring: ring index the fence is associated with
 *
 * Wait for the next fence on the requested ring to signal (all asics).
 * Returns 0 if the next fence has passed, error for all other cases.
 * Caller must hold ring lock.
 */
int radeon_fence_wait_next(struct radeon_device *rdev, int ring)
{
	uint64_t seq[RADEON_NUM_RINGS] = {};
	long r;

	seq[ring] = atomic64_read(&rdev->fence_drv[ring].last_seq) + 1ULL;
	if (seq[ring] >= rdev->fence_drv[ring].sync_seq[ring]) {
		/* nothing to wait for, last_seq is
		   already the last emited fence */
		return -ENOENT;
	}
	r = radeon_fence_wait_seq_timeout(rdev, seq, false, MAX_SCHEDULE_TIMEOUT);
	if (r < 0)
		return r;
	return 0;
}

/**
 * radeon_fence_wait_empty - wait for all fences to signal
 *
 * @rdev: radeon device pointer
 * @ring: ring index the fence is associated with
 *
 * Wait for all fences on the requested ring to signal (all asics).
 * Returns 0 if the fences have passed, error for all other cases.
 * Caller must hold ring lock.
 */
int radeon_fence_wait_empty(struct radeon_device *rdev, int ring)
{
	uint64_t seq[RADEON_NUM_RINGS] = {};
	long r;

	seq[ring] = rdev->fence_drv[ring].sync_seq[ring];
	if (!seq[ring])
		return 0;

	r = radeon_fence_wait_seq_timeout(rdev, seq, false, MAX_SCHEDULE_TIMEOUT);
	if (r < 0) {
		if (r == -EDEADLK)
			return -EDEADLK;

		dev_err(rdev->dev, "error waiting for ring[%d] to become idle (%ld)\n",
			ring, r);
	}
	return 0;
}

/**
 * radeon_fence_ref - take a ref on a fence
 *
 * @fence: radeon fence object
 *
 * Take a reference on a fence (all asics).
 * Returns the fence.
 */
struct radeon_fence *radeon_fence_ref(struct radeon_fence *fence)
{
	dma_fence_get(&fence->base);
	return fence;
}

/**
 * radeon_fence_unref - remove a ref on a fence
 *
 * @fence: radeon fence object
 *
 * Remove a reference on a fence (all asics).
 */
void radeon_fence_unref(struct radeon_fence **fence)
{
	struct radeon_fence *tmp = *fence;

	*fence = NULL;
	if (tmp) {
		dma_fence_put(&tmp->base);
	}
}

/**
 * radeon_fence_count_emitted - get the count of emitted fences
 *
 * @rdev: radeon device pointer
 * @ring: ring index the fence is associated with
 *
 * Get the number of fences emitted on the requested ring (all asics).
 * Returns the number of emitted fences on the ring.  Used by the
 * dynpm code to ring track activity.
 */
unsigned radeon_fence_count_emitted(struct radeon_device *rdev, int ring)
{
	uint64_t emitted;

	/* We are not protected by ring lock when reading the last sequence
	 * but it's ok to report slightly wrong fence count here.
	 */
	radeon_fence_process(rdev, ring);
	emitted = rdev->fence_drv[ring].sync_seq[ring]
		- atomic64_read(&rdev->fence_drv[ring].last_seq);
	/* to avoid 32bits warp around */
	if (emitted > 0x10000000) {
		emitted = 0x10000000;
	}
	return (unsigned)emitted;
}

/**
 * radeon_fence_need_sync - do we need a semaphore
 *
 * @fence: radeon fence object
 * @dst_ring: which ring to check against
 *
 * Check if the fence needs to be synced against another ring
 * (all asics).  If so, we need to emit a semaphore.
 * Returns true if we need to sync with another ring, false if
 * not.
 */
bool radeon_fence_need_sync(struct radeon_fence *fence, int dst_ring)
{
	struct radeon_fence_driver *fdrv;

	if (!fence) {
		return false;
	}

	if (fence->ring == dst_ring) {
		return false;
	}

	/* we are protected by the ring mutex */
	fdrv = &fence->rdev->fence_drv[dst_ring];
	if (fence->seq <= fdrv->sync_seq[fence->ring]) {
		return false;
	}

	return true;
}

/**
 * radeon_fence_note_sync - record the sync point
 *
 * @fence: radeon fence object
 * @dst_ring: which ring to check against
 *
 * Note the sequence number at which point the fence will
 * be synced with the requested ring (all asics).
 */
void radeon_fence_note_sync(struct radeon_fence *fence, int dst_ring)
{
	struct radeon_fence_driver *dst, *src;
	unsigned i;

	if (!fence) {
		return;
	}

	if (fence->ring == dst_ring) {
		return;
	}

	/* we are protected by the ring mutex */
	src = &fence->rdev->fence_drv[fence->ring];
	dst = &fence->rdev->fence_drv[dst_ring];
	for (i = 0; i < RADEON_NUM_RINGS; ++i) {
		if (i == dst_ring) {
			continue;
		}
		dst->sync_seq[i] = max(dst->sync_seq[i], src->sync_seq[i]);
	}
}

/**
 * radeon_fence_driver_start_ring - make the fence driver
 * ready for use on the requested ring.
 *
 * @rdev: radeon device pointer
 * @ring: ring index to start the fence driver on
 *
 * Make the fence driver ready for processing (all asics).
 * Not all asics have all rings, so each asic will only
 * start the fence driver on the rings it has.
 * Returns 0 for success, errors for failure.
 */
int radeon_fence_driver_start_ring(struct radeon_device *rdev, int ring)
{
	uint64_t index;
	int r;

	/* Task#209 取证：写断点要在任何 fence 等待之前挂好（内部只尝试一次） */
	radeon_spy_bp_install();

	/* Task#209 取证：在任何 fence 等待之前抓一次函数体快照（含 alternatives 改写后的结果） */
	if (!radeon_spy_ref && radeon_spy_param) {
		radeon_spy_ref = kmalloc(RADEON_SPY_LEN, GFP_KERNEL);
		if (radeon_spy_ref) {
			memcpy(radeon_spy_ref, (void *)radeon_fence_any_seq_signaled,
			       RADEON_SPY_LEN);
			print_hex_dump(KERN_ERR, "T209SPY init: ", DUMP_PREFIX_OFFSET,
				       16, 1, radeon_spy_ref, RADEON_SPY_LEN, false);
		}
	}

	radeon_scratch_free(rdev, rdev->fence_drv[ring].scratch_reg);
	if (rdev->wb.use_event || !radeon_ring_supports_scratch_reg(rdev, &rdev->ring[ring])) {
		rdev->fence_drv[ring].scratch_reg = 0;
		if (ring != R600_RING_TYPE_UVD_INDEX) {
			index = R600_WB_EVENT_OFFSET + ring * 4;
			rdev->fence_drv[ring].cpu_addr = &rdev->wb.wb[index/4];
			rdev->fence_drv[ring].gpu_addr = rdev->wb.gpu_addr +
							 index;

		} else {
			/* put fence directly behind firmware */
			index = ALIGN(rdev->uvd_fw->size, 8);
			rdev->fence_drv[ring].cpu_addr = rdev->uvd.cpu_addr + index;
			rdev->fence_drv[ring].gpu_addr = rdev->uvd.gpu_addr + index;
		}

	} else {
		r = radeon_scratch_get(rdev, &rdev->fence_drv[ring].scratch_reg);
		if (r) {
			dev_err(rdev->dev, "fence failed to get scratch register\n");
			return r;
		}
		index = RADEON_WB_SCRATCH_OFFSET +
			rdev->fence_drv[ring].scratch_reg -
			rdev->scratch.reg_base;
		rdev->fence_drv[ring].cpu_addr = &rdev->wb.wb[index/4];
		rdev->fence_drv[ring].gpu_addr = rdev->wb.gpu_addr + index;
	}
	radeon_fence_write(rdev, atomic64_read(&rdev->fence_drv[ring].last_seq), ring);
	rdev->fence_drv[ring].initialized = true;
	dev_info(rdev->dev, "fence driver on ring %d use gpu addr 0x%016llx and cpu addr 0x%p\n",
		 ring, rdev->fence_drv[ring].gpu_addr, rdev->fence_drv[ring].cpu_addr);
	return 0;
}

/**
 * radeon_fence_driver_init_ring - init the fence driver
 * for the requested ring.
 *
 * @rdev: radeon device pointer
 * @ring: ring index to start the fence driver on
 *
 * Init the fence driver for the requested ring (all asics).
 * Helper function for radeon_fence_driver_init().
 */
static void radeon_fence_driver_init_ring(struct radeon_device *rdev, int ring)
{
	int i;

	rdev->fence_drv[ring].scratch_reg = -1;
	rdev->fence_drv[ring].cpu_addr = NULL;
	rdev->fence_drv[ring].gpu_addr = 0;
	for (i = 0; i < RADEON_NUM_RINGS; ++i)
		rdev->fence_drv[ring].sync_seq[i] = 0;
	atomic64_set(&rdev->fence_drv[ring].last_seq, 0);
	rdev->fence_drv[ring].initialized = false;
	INIT_DELAYED_WORK(&rdev->fence_drv[ring].lockup_work,
			  radeon_fence_check_lockup);
	rdev->fence_drv[ring].rdev = rdev;
}

/**
 * radeon_fence_driver_init - init the fence driver
 * for all possible rings.
 *
 * @rdev: radeon device pointer
 *
 * Init the fence driver for all possible rings (all asics).
 * Not all asics have all rings, so each asic will only
 * start the fence driver on the rings it has using
 * radeon_fence_driver_start_ring().
 * Returns 0 for success.
 */
int radeon_fence_driver_init(struct radeon_device *rdev)
{
	int ring;

	init_waitqueue_head(&rdev->fence_queue);
	for (ring = 0; ring < RADEON_NUM_RINGS; ring++) {
		radeon_fence_driver_init_ring(rdev, ring);
	}
	if (radeon_debugfs_fence_init(rdev)) {
		dev_err(rdev->dev, "fence debugfs file creation failed\n");
	}
	return 0;
}

/**
 * radeon_fence_driver_fini - tear down the fence driver
 * for all possible rings.
 *
 * @rdev: radeon device pointer
 *
 * Tear down the fence driver for all possible rings (all asics).
 */
void radeon_fence_driver_fini(struct radeon_device *rdev)
{
	int ring, r;

	/* 卸驱动前必须撤掉写断点，否则断点会指向已释放的模块代码 */
	radeon_spy_bp_remove();

	mutex_lock(&rdev->ring_lock);
	for (ring = 0; ring < RADEON_NUM_RINGS; ring++) {
		if (!rdev->fence_drv[ring].initialized)
			continue;
		r = radeon_fence_wait_empty(rdev, ring);
		if (r) {
			/* no need to trigger GPU reset as we are unloading */
			radeon_fence_driver_force_completion(rdev, ring);
		}
		cancel_delayed_work_sync(&rdev->fence_drv[ring].lockup_work);
		wake_up_all(&rdev->fence_queue);
		radeon_scratch_free(rdev, rdev->fence_drv[ring].scratch_reg);
		rdev->fence_drv[ring].initialized = false;
	}
	mutex_unlock(&rdev->ring_lock);
}

/**
 * radeon_fence_driver_force_completion - force all fence waiter to complete
 *
 * @rdev: radeon device pointer
 * @ring: the ring to complete
 *
 * In case of GPU reset failure make sure no process keep waiting on fence
 * that will never complete.
 */
void radeon_fence_driver_force_completion(struct radeon_device *rdev, int ring)
{
	if (rdev->fence_drv[ring].initialized) {
		radeon_fence_write(rdev, rdev->fence_drv[ring].sync_seq[ring], ring);
		cancel_delayed_work_sync(&rdev->fence_drv[ring].lockup_work);
	}
}


/*
 * Fence debugfs
 */
#if defined(CONFIG_DEBUG_FS)
static int radeon_debugfs_fence_info(struct seq_file *m, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *dev = node->minor->dev;
	struct radeon_device *rdev = dev->dev_private;
	int i, j;

	for (i = 0; i < RADEON_NUM_RINGS; ++i) {
		if (!rdev->fence_drv[i].initialized)
			continue;

		radeon_fence_process(rdev, i);

		seq_printf(m, "--- ring %d ---\n", i);
		seq_printf(m, "Last signaled fence 0x%016llx\n",
			   (unsigned long long)atomic64_read(&rdev->fence_drv[i].last_seq));
		seq_printf(m, "Last emitted        0x%016llx\n",
			   rdev->fence_drv[i].sync_seq[i]);

		for (j = 0; j < RADEON_NUM_RINGS; ++j) {
			if (i != j && rdev->fence_drv[j].initialized)
				seq_printf(m, "Last sync to ring %d 0x%016llx\n",
					   j, rdev->fence_drv[i].sync_seq[j]);
		}
	}
	return 0;
}

/**
 * radeon_debugfs_gpu_reset - manually trigger a gpu reset
 *
 * Manually trigger a gpu reset at the next fence wait.
 */
static int radeon_debugfs_gpu_reset(struct seq_file *m, void *data)
{
	struct drm_info_node *node = (struct drm_info_node *) m->private;
	struct drm_device *dev = node->minor->dev;
	struct radeon_device *rdev = dev->dev_private;

	down_read(&rdev->exclusive_lock);
	seq_printf(m, "%d\n", rdev->needs_reset);
	rdev->needs_reset = true;
	wake_up_all(&rdev->fence_queue);
	up_read(&rdev->exclusive_lock);

	return 0;
}

static struct drm_info_list radeon_debugfs_fence_list[] = {
	{"radeon_fence_info", &radeon_debugfs_fence_info, 0, NULL},
	{"radeon_gpu_reset", &radeon_debugfs_gpu_reset, 0, NULL}
};
#endif

int radeon_debugfs_fence_init(struct radeon_device *rdev)
{
#if defined(CONFIG_DEBUG_FS)
	return radeon_debugfs_add_files(rdev, radeon_debugfs_fence_list, 2);
#else
	return 0;
#endif
}

static const char *radeon_fence_get_driver_name(struct dma_fence *fence)
{
	return "radeon";
}

static const char *radeon_fence_get_timeline_name(struct dma_fence *f)
{
	struct radeon_fence *fence = to_radeon_fence(f);
	switch (fence->ring) {
	case RADEON_RING_TYPE_GFX_INDEX: return "radeon.gfx";
	case CAYMAN_RING_TYPE_CP1_INDEX: return "radeon.cp1";
	case CAYMAN_RING_TYPE_CP2_INDEX: return "radeon.cp2";
	case R600_RING_TYPE_DMA_INDEX: return "radeon.dma";
	case CAYMAN_RING_TYPE_DMA1_INDEX: return "radeon.dma1";
	case R600_RING_TYPE_UVD_INDEX: return "radeon.uvd";
	case TN_RING_TYPE_VCE1_INDEX: return "radeon.vce1";
	case TN_RING_TYPE_VCE2_INDEX: return "radeon.vce2";
	default: WARN_ON_ONCE(1); return "radeon.unk";
	}
}

static inline bool radeon_test_signaled(struct radeon_fence *fence)
{
	return test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->base.flags);
}

struct radeon_wait_cb {
	struct dma_fence_cb base;
	struct task_struct *task;
};

static void
radeon_fence_wait_cb(struct dma_fence *fence, struct dma_fence_cb *cb)
{
	struct radeon_wait_cb *wait =
		container_of(cb, struct radeon_wait_cb, base);

	wake_up_process(wait->task);
}

static signed long radeon_fence_default_wait(struct dma_fence *f, bool intr,
					     signed long t)
{
	struct radeon_fence *fence = to_radeon_fence(f);
	struct radeon_device *rdev = fence->rdev;
	struct radeon_wait_cb cb;

	cb.task = current;

	if (dma_fence_add_callback(f, &cb.base, radeon_fence_wait_cb))
		return t;

	while (t > 0) {
		long slice, left;

		if (intr)
			set_current_state(TASK_INTERRUPTIBLE);
		else
			set_current_state(TASK_UNINTERRUPTIBLE);

		/*
		 * radeon_test_signaled must be called after
		 * set_current_state to prevent a race with wake_up_process
		 */
		if (radeon_test_signaled(fence))
			break;

		if (rdev->needs_reset) {
			t = -EDEADLK;
			break;
		}

		/*
		 * Task#209：本环没有 fence 完成中断时（TN/ARUBA 的 VCE），
		 * 只有 lockup_work 的 HZ/2 定时检查会 signal fence。主动推进
		 * 一次 fence 处理（读硬件 fence 值），再按短分片睡眠，避免
		 * 每个 fence 固定等满 500ms。
		 */
		radeon_fence_process(rdev, fence->ring);
		if (radeon_test_signaled(fence))
			break;

		slice = min_t(long, t, RADEON_FENCE_WAIT_SLICE_JIFFIES);
		left = schedule_timeout(slice);

		/* 只扣除实际睡掉的 jiffies，被回调唤醒时不丢剩余时间 */
		if (t != MAX_SCHEDULE_TIMEOUT)
			t -= slice - left;

		if (t > 0 && intr && signal_pending(current))
			t = -ERESTARTSYS;
	}

	__set_current_state(TASK_RUNNING);
	dma_fence_remove_callback(f, &cb.base);

	return t;
}

const struct dma_fence_ops radeon_fence_ops = {
	.get_driver_name = radeon_fence_get_driver_name,
	.get_timeline_name = radeon_fence_get_timeline_name,
	.enable_signaling = radeon_fence_enable_signaling,
	.signaled = radeon_fence_is_signaled,
	.wait = radeon_fence_default_wait,
	.release = NULL,
};

// SPDX-License-Identifier: MIT
/*
 * DSM 4.4.302 无 fbdev 模拟的 radeon 桩实现。
 *
 * 4.4.302 官方内核 CONFIG_DRM_FBDEV_EMULATION=n（实测 update/drm_kms_helper.ko
 * 里没有任何 drm_fb_helper_* 符号），而 radeon_fb.c 无条件调用 drm_fb_helper_*，
 * 编进来会在加载时报未解析符号。这里按 amdgpu 现有的 headless 做法，用空实现
 * 顶替 fbdev 初始化 / 挂起 / 连接器热插拔钩子：驱动照常完成 KMS 初始化，
 * 只是不创建 framebuffer console。
 *
 * radeon_align_pitch() 不依赖 fb helper，且 radeon_gem.c 的 dumb buffer 需要它，
 * 因此保留真实实现（与 radeon_fb.c 逐字一致）。
 */

#include "radeon.h"

int radeon_align_pitch(struct radeon_device *rdev, int width, int cpp, bool tiled)
{
	int aligned = width;
	int align_large = (ASIC_IS_AVIVO(rdev)) || tiled;
	int pitch_mask = 0;

	switch (cpp) {
	case 1:
		pitch_mask = align_large ? 255 : 127;
		break;
	case 2:
		pitch_mask = align_large ? 127 : 31;
		break;
	case 3:
	case 4:
		pitch_mask = align_large ? 63 : 15;
		break;
	}

	aligned += pitch_mask;
	aligned &= ~pitch_mask;
	return aligned * cpp;
}

int radeon_fbdev_init(struct radeon_device *rdev)
{
	return 0;
}

void radeon_fbdev_fini(struct radeon_device *rdev)
{
}

void radeon_fbdev_set_suspend(struct radeon_device *rdev, int state)
{
}

bool radeon_fbdev_robj_is_fb(struct radeon_device *rdev, struct radeon_bo *robj)
{
	return false;
}

void radeon_fb_add_connector(struct radeon_device *rdev,
			     struct drm_connector *connector)
{
}

void radeon_fb_remove_connector(struct radeon_device *rdev,
				struct drm_connector *connector)
{
}

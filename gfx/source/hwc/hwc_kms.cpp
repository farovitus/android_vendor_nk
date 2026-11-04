/*
 * Minimal HWC 1.4 module for the Galaxy S6 on mainline: SurfaceFlinger
 * composites every layer with GLES (Mesa panfrost); this module shows the
 * client target buffer (a minigbm dma-buf) on the exynos DRM CRTC and
 * drives vsync from a timer. Android's HWC2On1Adapter wraps it for
 * android.hardware.graphics.composer@2.1-service.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>
#include <hardware/hwcomposer.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "cros_gralloc_handle.h"

#define LOG_TAG "hwc_kms"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define FB_CACHE 8

struct fb_entry {
	ino_t ino;
	uint32_t fb_id;
	uint32_t gem;
};

struct kms_ctx {
	hwc_composer_device_1_t dev;
	const hwc_procs_t *procs;

	int fd;
	uint32_t crtc_id;
	uint32_t conn_id;
	drmModeModeInfo mode;
	bool mode_set;
	uint32_t width_mm, height_mm;

	struct fb_entry fbs[FB_CACHE];
	unsigned int fb_next;

	pthread_t vsync_thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool vsync_on;
	bool stop;
	int64_t period_ns;
};

static int64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int open_exynos_card(void)
{
	char path[32];

	for (int i = 0; i < 4; i++) {
		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		drmVersionPtr v = drmGetVersion(fd);
		bool ok = v && !strcmp(v->name, "exynos");
		drmFreeVersion(v);
		if (ok) {
			ALOGI("using %s", path);
			return fd;
		}
		close(fd);
	}
	return -1;
}

static int kms_init(struct kms_ctx *ctx)
{
	ctx->fd = open_exynos_card();
	if (ctx->fd < 0) {
		ALOGE("no exynos DRM card");
		return -ENODEV;
	}

	drmModeResPtr res = drmModeGetResources(ctx->fd);
	if (!res) {
		ALOGE("drmModeGetResources: %s", strerror(errno));
		return -errno;
	}

	drmModeConnectorPtr conn = NULL;
	for (int i = 0; i < res->count_connectors && !conn; i++) {
		drmModeConnectorPtr c = drmModeGetConnector(ctx->fd, res->connectors[i]);
		if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
			conn = c;
		else
			drmModeFreeConnector(c);
	}
	if (!conn) {
		ALOGE("no connected connector");
		drmModeFreeResources(res);
		return -ENODEV;
	}

	ctx->conn_id = conn->connector_id;
	ctx->mode = conn->modes[0];
	for (int i = 0; i < conn->count_modes; i++)
		if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED)
			ctx->mode = conn->modes[i];
	ctx->width_mm = conn->mmWidth;
	ctx->height_mm = conn->mmHeight;

	drmModeEncoderPtr enc = drmModeGetEncoder(ctx->fd, conn->encoder_id);
	if (enc && enc->crtc_id) {
		ctx->crtc_id = enc->crtc_id;
	} else {
		uint32_t possible = enc ? enc->possible_crtcs : 1;
		for (int i = 0; i < res->count_crtcs; i++)
			if (possible & (1u << i)) {
				ctx->crtc_id = res->crtcs[i];
				break;
			}
	}
	drmModeFreeEncoder(enc);
	drmModeFreeConnector(conn);
	drmModeFreeResources(res);

	if (!ctx->crtc_id) {
		ALOGE("no CRTC");
		return -ENODEV;
	}

	int hz = ctx->mode.vrefresh ? ctx->mode.vrefresh : 60;
	ctx->period_ns = 1000000000LL / hz;
	ALOGI("connector %u crtc %u mode %ux%u@%d", ctx->conn_id, ctx->crtc_id,
	      ctx->mode.hdisplay, ctx->mode.vdisplay, hz);
	return 0;
}

static uint32_t opaque_format(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_ABGR8888:
		return DRM_FORMAT_XBGR8888;
	case DRM_FORMAT_ARGB8888:
		return DRM_FORMAT_XRGB8888;
	default:
		return format;
	}
}

static void fb_release(struct kms_ctx *ctx, struct fb_entry *e)
{
	if (e->fb_id)
		drmModeRmFB(ctx->fd, e->fb_id);
	if (e->gem) {
		struct drm_gem_close gc = { .handle = e->gem };
		drmIoctl(ctx->fd, DRM_IOCTL_GEM_CLOSE, &gc);
	}
	memset(e, 0, sizeof(*e));
}

/* Framebuffer for a gralloc buffer, cached by dma-buf inode. */
static uint32_t fb_for(struct kms_ctx *ctx, buffer_handle_t handle)
{
	cros_gralloc_handle_t h = (cros_gralloc_handle_t)handle;
	struct stat st;

	if (!h || h->numFds < 1 || fstat(h->fds[0], &st))
		return 0;

	for (int i = 0; i < FB_CACHE; i++)
		if (ctx->fbs[i].fb_id && ctx->fbs[i].ino == st.st_ino)
			return ctx->fbs[i].fb_id;

	struct fb_entry *e = &ctx->fbs[ctx->fb_next];
	ctx->fb_next = (ctx->fb_next + 1) % FB_CACHE;
	fb_release(ctx, e);

	if (drmPrimeFDToHandle(ctx->fd, h->fds[0], &e->gem)) {
		ALOGE("PrimeFDToHandle: %s", strerror(errno));
		return 0;
	}

	uint32_t handles[4] = { e->gem };
	uint32_t pitches[4] = { h->strides[0] };
	uint32_t offsets[4] = { h->offsets[0] };
	uint32_t format = h->format;

	if (drmModeAddFB2(ctx->fd, h->width, h->height, format, handles, pitches,
			  offsets, &e->fb_id, 0)) {
		format = opaque_format(format);
		if (drmModeAddFB2(ctx->fd, h->width, h->height, format, handles,
				  pitches, offsets, &e->fb_id, 0)) {
			ALOGE("AddFB2 %ux%u %.4s: %s", h->width, h->height,
			      (const char *)&h->format, strerror(errno));
			fb_release(ctx, e);
			return 0;
		}
	}
	e->ino = st.st_ino;
	return e->fb_id;
}

static void flip_done(int fd, unsigned int seq, unsigned int sec,
		      unsigned int usec, void *data)
{
	(void)fd; (void)seq; (void)sec; (void)usec;
	*(bool *)data = true;
}

static int show(struct kms_ctx *ctx, uint32_t fb_id)
{
	if (!ctx->mode_set) {
		if (drmModeSetCrtc(ctx->fd, ctx->crtc_id, fb_id, 0, 0,
				   &ctx->conn_id, 1, &ctx->mode)) {
			ALOGE("SetCrtc: %s", strerror(errno));
			return -errno;
		}
		ctx->mode_set = true;
		return 0;
	}

	bool done = false;
	if (drmModePageFlip(ctx->fd, ctx->crtc_id, fb_id,
			    DRM_MODE_PAGE_FLIP_EVENT, &done)) {
		/* No flip support or still busy: set the buffer directly. */
		ctx->mode_set = false;
		return show(ctx, fb_id);
	}

	/* Wait for the flip so the previous buffer is free when we return. */
	drmEventContext ev = {};
	ev.version = 2;
	ev.page_flip_handler = flip_done;
	struct pollfd pfd = { .fd = ctx->fd, .events = POLLIN };
	while (!done) {
		if (poll(&pfd, 1, 100) <= 0) {
			ALOGE("page flip timed out");
			break;
		}
		drmHandleEvent(ctx->fd, &ev);
	}
	return 0;
}

static void wait_fence(int fd)
{
	if (fd < 0)
		return;
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	poll(&pfd, 1, 3000);
	close(fd);
}

static int hwc_prepare(hwc_composer_device_1_t *dev, size_t num,
		       hwc_display_contents_1_t **displays)
{
	(void)dev;
	if (num < 1 || !displays[0])
		return 0;
	hwc_display_contents_1_t *c = displays[0];
	for (size_t i = 0; i < c->numHwLayers; i++)
		if (c->hwLayers[i].compositionType != HWC_FRAMEBUFFER_TARGET)
			c->hwLayers[i].compositionType = HWC_FRAMEBUFFER;
	return 0;
}

static int hwc_set(hwc_composer_device_1_t *dev, size_t num,
		   hwc_display_contents_1_t **displays)
{
	struct kms_ctx *ctx = (struct kms_ctx *)dev;

	for (size_t d = 0; d < num; d++) {
		hwc_display_contents_1_t *c = displays[d];
		if (!c)
			continue;
		buffer_handle_t target = NULL;
		for (size_t i = 0; i < c->numHwLayers; i++) {
			hwc_layer_1_t *l = &c->hwLayers[i];
			if (d == 0 && l->compositionType == HWC_FRAMEBUFFER_TARGET) {
				target = l->handle;
				wait_fence(l->acquireFenceFd);
			} else if (l->acquireFenceFd >= 0) {
				close(l->acquireFenceFd);
			}
			l->acquireFenceFd = -1;
			l->releaseFenceFd = -1;
		}
		c->retireFenceFd = -1;
		if (d == 0 && target) {
			uint32_t fb = fb_for(ctx, target);
			if (fb)
				show(ctx, fb);
		}
	}
	return 0;
}

static void *vsync_loop(void *arg)
{
	struct kms_ctx *ctx = (struct kms_ctx *)arg;
	int64_t next = now_ns();

	pthread_mutex_lock(&ctx->lock);
	while (!ctx->stop) {
		if (!ctx->vsync_on) {
			pthread_cond_wait(&ctx->cond, &ctx->lock);
			next = now_ns();
			continue;
		}
		pthread_mutex_unlock(&ctx->lock);

		next += ctx->period_ns;
		struct timespec ts = { (time_t)(next / 1000000000LL),
				       (long)(next % 1000000000LL) };
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
		if (now_ns() - next > ctx->period_ns)
			next = now_ns();

		pthread_mutex_lock(&ctx->lock);
		const hwc_procs_t *procs = ctx->vsync_on ? ctx->procs : NULL;
		if (procs && procs->vsync) {
			/* The callback may call eventControl; do not hold the lock. */
			pthread_mutex_unlock(&ctx->lock);
			procs->vsync(procs, 0, next);
			pthread_mutex_lock(&ctx->lock);
		}
	}
	pthread_mutex_unlock(&ctx->lock);
	return NULL;
}

static int hwc_event_control(hwc_composer_device_1_t *dev, int disp, int event,
			     int enabled)
{
	struct kms_ctx *ctx = (struct kms_ctx *)dev;

	if (disp != 0 || event != HWC_EVENT_VSYNC)
		return -EINVAL;
	pthread_mutex_lock(&ctx->lock);
	ctx->vsync_on = enabled;
	pthread_cond_signal(&ctx->cond);
	pthread_mutex_unlock(&ctx->lock);
	return 0;
}

static int hwc_set_power_mode(hwc_composer_device_1_t *dev, int disp, int mode)
{
	(void)dev; (void)mode;
	return disp == 0 ? 0 : -EINVAL;
}

static int hwc_query(hwc_composer_device_1_t *dev, int what, int *value)
{
	struct kms_ctx *ctx = (struct kms_ctx *)dev;

	switch (what) {
	case HWC_BACKGROUND_LAYER_SUPPORTED:
		*value = 0;
		return 0;
	case HWC_VSYNC_PERIOD:
		*value = (int)ctx->period_ns;
		return 0;
	case HWC_DISPLAY_TYPES_SUPPORTED:
		*value = HWC_DISPLAY_PRIMARY_BIT;
		return 0;
	default:
		return -EINVAL;
	}
}

static void hwc_register_procs(hwc_composer_device_1_t *dev,
			       const hwc_procs_t *procs)
{
	struct kms_ctx *ctx = (struct kms_ctx *)dev;

	pthread_mutex_lock(&ctx->lock);
	ctx->procs = procs;
	pthread_mutex_unlock(&ctx->lock);
}

static int hwc_get_display_configs(hwc_composer_device_1_t *dev, int disp,
				   uint32_t *configs, size_t *num)
{
	(void)dev;
	if (disp != 0)
		return -EINVAL;
	if (*num > 0)
		configs[0] = 0;
	*num = 1;
	return 0;
}

static int32_t dpi(uint32_t px, uint32_t mm)
{
	/* HWC 1 reports dots per thousand inches. S6: 577 ppi. */
	return mm ? (int32_t)(px * 25400000ULL / mm) : 577000;
}

static int hwc_get_display_attributes(hwc_composer_device_1_t *dev, int disp,
				      uint32_t config, const uint32_t *attrs,
				      int32_t *values)
{
	struct kms_ctx *ctx = (struct kms_ctx *)dev;

	if (disp != 0 || config != 0)
		return -EINVAL;
	for (int i = 0; attrs[i] != HWC_DISPLAY_NO_ATTRIBUTE; i++) {
		switch (attrs[i]) {
		case HWC_DISPLAY_VSYNC_PERIOD:
			values[i] = (int32_t)ctx->period_ns;
			break;
		case HWC_DISPLAY_WIDTH:
			values[i] = ctx->mode.hdisplay;
			break;
		case HWC_DISPLAY_HEIGHT:
			values[i] = ctx->mode.vdisplay;
			break;
		case HWC_DISPLAY_DPI_X:
			values[i] = dpi(ctx->mode.hdisplay, ctx->width_mm);
			break;
		case HWC_DISPLAY_DPI_Y:
			values[i] = dpi(ctx->mode.vdisplay, ctx->height_mm);
			break;
		default:
			values[i] = 0;
			break;
		}
	}
	return 0;
}

static int hwc_get_active_config(hwc_composer_device_1_t *dev, int disp)
{
	(void)dev;
	return disp == 0 ? 0 : -1;
}

static int hwc_set_active_config(hwc_composer_device_1_t *dev, int disp,
				 int index)
{
	(void)dev;
	return disp == 0 && index == 0 ? 0 : -EINVAL;
}

static void hwc_dump(hwc_composer_device_1_t *dev, char *buff, int len)
{
	struct kms_ctx *ctx = (struct kms_ctx *)dev;

	snprintf(buff, len, "hwc_kms: crtc %u connector %u %ux%u\n",
		 ctx->crtc_id, ctx->conn_id, ctx->mode.hdisplay,
		 ctx->mode.vdisplay);
}

static int hwc_close(hw_device_t *dev)
{
	struct kms_ctx *ctx = (struct kms_ctx *)dev;

	pthread_mutex_lock(&ctx->lock);
	ctx->stop = true;
	pthread_cond_signal(&ctx->cond);
	pthread_mutex_unlock(&ctx->lock);
	pthread_join(ctx->vsync_thread, NULL);
	for (int i = 0; i < FB_CACHE; i++)
		fb_release(ctx, &ctx->fbs[i]);
	close(ctx->fd);
	free(ctx);
	return 0;
}

static int hwc_open(const hw_module_t *module, const char *name,
		    hw_device_t **device)
{
	if (strcmp(name, HWC_HARDWARE_COMPOSER))
		return -EINVAL;

	struct kms_ctx *ctx = (struct kms_ctx *)calloc(1, sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;

	int ret = kms_init(ctx);
	if (ret) {
		if (ctx->fd >= 0)
			close(ctx->fd);
		free(ctx);
		return ret;
	}

	pthread_mutex_init(&ctx->lock, NULL);
	pthread_cond_init(&ctx->cond, NULL);
	pthread_create(&ctx->vsync_thread, NULL, vsync_loop, ctx);

	ctx->dev.common.tag = HARDWARE_DEVICE_TAG;
	ctx->dev.common.version = HWC_DEVICE_API_VERSION_1_4;
	ctx->dev.common.module = (hw_module_t *)module;
	ctx->dev.common.close = hwc_close;
	ctx->dev.prepare = hwc_prepare;
	ctx->dev.set = hwc_set;
	ctx->dev.eventControl = hwc_event_control;
	ctx->dev.setPowerMode = hwc_set_power_mode;
	ctx->dev.query = hwc_query;
	ctx->dev.registerProcs = hwc_register_procs;
	ctx->dev.dump = hwc_dump;
	ctx->dev.getDisplayConfigs = hwc_get_display_configs;
	ctx->dev.getDisplayAttributes = hwc_get_display_attributes;
	ctx->dev.getActiveConfig = hwc_get_active_config;
	ctx->dev.setActiveConfig = hwc_set_active_config;

	*device = &ctx->dev.common;
	return 0;
}

static struct hw_module_methods_t hwc_methods = {
	.open = hwc_open,
};

extern "C" __attribute__((visibility("default"))) hwc_module_t HAL_MODULE_INFO_SYM = {
	.common = {
		.tag = HARDWARE_MODULE_TAG,
		.module_api_version = HWC_MODULE_API_VERSION_0_1,
		.hal_api_version = HARDWARE_HAL_API_VERSION,
		.id = HWC_HARDWARE_MODULE_ID,
		.name = "S6 exynos KMS hwcomposer",
		.author = "FAROVITUS",
		.methods = &hwc_methods,
	},
};

/*
 * Minimal HWC 1.4 module for the Galaxy S6 on mainline: SurfaceFlinger
 * composites every layer with GLES (Mesa panfrost); this module shows the
 * client target buffer (a minigbm dma-buf) on the exynos DRM CRTC.
 * Android's HWC2On1Adapter wraps it for
 * android.hardware.graphics.composer@2.1-service.
 *
 * Frames go out as non-blocking atomic commits: the GPU's acquire fence is
 * handed to the kernel as the plane's IN_FENCE_FD, and the CRTC's
 * OUT_FENCE_PTR comes back as the retire (present) fence, which signals
 * when the frame reaches the panel. set() returns without waiting on the
 * GPU or the flip. A test-only commit after the first mode set checks that
 * the kernel accepts atomic flips. If it does not, or a later commit is
 * rejected with EINVAL or EOPNOTSUPP, the module falls back to the legacy
 * path (wait for the GPU, page flip, wait for the flip). Other failures
 * send only that frame the legacy way. Setting vendor.hwc.s6kms.legacy=1
 * forces the legacy path.
 *
 * The panel runs in command (i80) mode, so the kernel raises vblank only
 * when a frame is sent. Vsync therefore comes from a free-running timer
 * at the mode's frame period.
 *
 * Power modes: OFF and DOZE_SUSPEND stop the vsync callbacks; NORMAL and
 * DOZE start them again on a new timer phase. Turning the CRTC off as
 * well (ACTIVE=0, or DPMS off on the legacy path) is opt-in through
 * vendor.hwc.s6kms.crtc_off=1: the panel is left running by S-Boot and
 * there is no panel driver to bring it back if DECON cannot.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
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

/*
 * Framebuffers for the client target ring (normally 3 buffers). An entry
 * keeps its dma-buf alive through the GEM handle, so one that has not
 * been shown for FB_IDLE_FRAMES frames is dropped: SurfaceFlinger has
 * reallocated its ring and the old buffer's memory would stay pinned.
 */
#define FB_CACHE 8
#define FB_IDLE_FRAMES 120

struct fb_entry {
	ino_t ino;
	uint32_t fb_id;
	uint32_t gem;
	uint64_t used;		/* frame that last showed it */
};

struct kms_ctx {
	hwc_composer_device_1_t dev;
	const hwc_procs_t *procs;

	int fd;
	uint32_t crtc_id;
	int crtc_index;
	uint32_t conn_id;
	drmModeModeInfo mode;
	bool mode_set;
	uint32_t width_mm, height_mm;

	struct fb_entry fbs[FB_CACHE];
	uint64_t frame;
	uint32_t cur_fb;	/* last committed */
	uint32_t prev_fb;	/* on screen until cur_fb's flip completes */

	/* Atomic path. */
	bool atomic;
	bool atomic_probed;
	uint32_t plane_id;
	uint32_t prop_plane_fb, prop_plane_crtc, prop_plane_in_fence;
	uint32_t prop_crtc_out_fence;
	uint32_t prop_crtc_active;
	int last_out;		/* our copy of the newest retire fence */

	/* Power. */
	uint32_t prop_conn_dpms;
	bool crtc_off;		/* CRTC turned off by setPowerMode */

	pthread_t vsync_thread;
	pthread_t event_thread;
	int stop_pipe[2];
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_cond_t flip_cond;
	unsigned int flips;	/* completed flips, under lock */
	bool vsync_on;
	bool powered;		/* display on (NORMAL or DOZE), under lock */
	bool stop;
	int64_t period_ns;
	int64_t epoch;		/* vsync phase, under lock */
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
	ctx->crtc_index = -1;
	for (int i = 0; i < res->count_crtcs; i++)
		if (res->crtcs[i] == ctx->crtc_id)
			ctx->crtc_index = i;
	drmModeFreeEncoder(enc);
	drmModeFreeConnector(conn);
	drmModeFreeResources(res);

	if (!ctx->crtc_id) {
		ALOGE("no CRTC");
		return -ENODEV;
	}

	/*
	 * The frame period follows from the mode's timing, not from its
	 * rounded vrefresh: the zeroflte mode (222371 kHz, 1443 x 2577 total)
	 * takes 16.72 ms per frame, 59.80 Hz, not 60.
	 */
	const drmModeModeInfo *m = &ctx->mode;
	if (m->clock && m->htotal && m->vtotal) {
		int64_t px = (int64_t)m->htotal * m->vtotal;
		ctx->period_ns = (px * 1000000LL + m->clock / 2) / m->clock;
	} else {
		ctx->period_ns = 1000000000LL / (m->vrefresh ? m->vrefresh : 60);
	}
	ALOGI("connector %u crtc %u mode %ux%u period %lld ns", ctx->conn_id,
	      ctx->crtc_id, m->hdisplay, m->vdisplay, (long long)ctx->period_ns);
	return 0;
}

static uint32_t prop_id(int fd, uint32_t obj, uint32_t type, const char *name,
			uint64_t *value)
{
	drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj, type);
	uint32_t id = 0;

	if (!props)
		return 0;
	for (uint32_t i = 0; i < props->count_props && !id; i++) {
		drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
		if (p && !strcmp(p->name, name)) {
			id = p->prop_id;
			if (value)
				*value = props->prop_values[i];
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return id;
}

static bool prop_true(const char *name)
{
	char v[PROP_VALUE_MAX] = "";

	__system_property_get(name, v);
	return v[0] == '1' || !strcmp(v, "true");
}

static bool force_legacy(void)
{
	return prop_true("vendor.hwc.s6kms.legacy");
}

/* Find the CRTC's primary plane and the properties a page flip needs. */
static void atomic_init(struct kms_ctx *ctx)
{
	if (force_legacy()) {
		ALOGI("legacy page flips forced by vendor.hwc.s6kms.legacy");
		return;
	}
	if (ctx->crtc_index < 0 ||
	    drmSetClientCap(ctx->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
	    drmSetClientCap(ctx->fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
		ALOGI("no atomic modesetting, using legacy page flips");
		return;
	}

	drmModePlaneResPtr pres = drmModeGetPlaneResources(ctx->fd);
	for (uint32_t i = 0; pres && i < pres->count_planes && !ctx->plane_id; i++) {
		drmModePlanePtr pl = drmModeGetPlane(ctx->fd, pres->planes[i]);
		uint64_t type = 0;
		if (pl && (pl->possible_crtcs & (1u << ctx->crtc_index)) &&
		    prop_id(ctx->fd, pl->plane_id, DRM_MODE_OBJECT_PLANE, "type",
			    &type) && type == DRM_PLANE_TYPE_PRIMARY)
			ctx->plane_id = pl->plane_id;
		drmModeFreePlane(pl);
	}
	drmModeFreePlaneResources(pres);

	if (ctx->plane_id) {
		ctx->prop_plane_fb = prop_id(ctx->fd, ctx->plane_id,
					     DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
		ctx->prop_plane_crtc = prop_id(ctx->fd, ctx->plane_id,
					       DRM_MODE_OBJECT_PLANE, "CRTC_ID", NULL);
		ctx->prop_plane_in_fence = prop_id(ctx->fd, ctx->plane_id,
						   DRM_MODE_OBJECT_PLANE,
						   "IN_FENCE_FD", NULL);
	}
	ctx->prop_crtc_out_fence = prop_id(ctx->fd, ctx->crtc_id,
					   DRM_MODE_OBJECT_CRTC,
					   "OUT_FENCE_PTR", NULL);
	ctx->prop_crtc_active = prop_id(ctx->fd, ctx->crtc_id,
					DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);

	if (!ctx->plane_id || !ctx->prop_plane_fb || !ctx->prop_plane_crtc ||
	    !ctx->prop_plane_in_fence || !ctx->prop_crtc_out_fence) {
		ALOGE("atomic: missing plane %u or fence properties, using legacy page flips",
		      ctx->plane_id);
		return;
	}
	ctx->atomic = true;
	ALOGI("atomic page flips on plane %u with in/out fences", ctx->plane_id);
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

	uint64_t frame = ++ctx->frame;
	struct fb_entry *hit = NULL, *e = NULL;

	for (int i = 0; i < FB_CACHE; i++) {
		struct fb_entry *f = &ctx->fbs[i];
		if (f->fb_id && f->ino == st.st_ino) {
			f->used = frame;
			hit = f;
			continue;
		}
		/* Never remove a framebuffer that is on screen or about to be. */
		if (f->fb_id && (f->fb_id == ctx->cur_fb || f->fb_id == ctx->prev_fb))
			continue;
		if (f->fb_id && frame - f->used > FB_IDLE_FRAMES)
			fb_release(ctx, f);
		/* A free slot, else the least recently shown entry. */
		if (!e || (e->fb_id && (!f->fb_id || f->used < e->used)))
			e = f;
	}
	if (hit)
		return hit->fb_id;
	fb_release(ctx, e);
	e->used = frame;

	if (drmPrimeFDToHandle(ctx->fd, h->fds[0], &e->gem)) {
		ALOGE("PrimeFDToHandle: %s", strerror(errno));
		return 0;
	}

	uint32_t handles[4] = { e->gem };
	uint32_t pitches[4] = { h->strides[0] };
	uint32_t offsets[4] = { h->offsets[0] };
	/*
	 * The client target fills the primary plane and is opaque. With an
	 * alpha format DECON turns on per-pixel blending (WINCONx_BLD_PIX)
	 * for the window, so scan it out as XRGB/XBGR and fall back to the
	 * buffer's own format only if the kernel refuses that.
	 */
	uint32_t format = opaque_format(h->format);

	if (drmModeAddFB2(ctx->fd, h->width, h->height, format, handles, pitches,
			  offsets, &e->fb_id, 0)) {
		format = h->format;
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
	struct kms_ctx *ctx = (struct kms_ctx *)data;
	(void)fd; (void)seq;

	(void)sec; (void)usec;
	pthread_mutex_lock(&ctx->lock);
	ctx->flips++;
	pthread_cond_broadcast(&ctx->flip_cond);
	pthread_mutex_unlock(&ctx->lock);
}

/* Reads DRM events: flip completions from both paths. */
static void *event_loop(void *arg)
{
	struct kms_ctx *ctx = (struct kms_ctx *)arg;
	drmEventContext ev = {};
	struct pollfd pfd[2] = {
		{ .fd = ctx->fd, .events = POLLIN },
		{ .fd = ctx->stop_pipe[0], .events = POLLIN },
	};

	ev.version = 2;
	ev.page_flip_handler = flip_done;
	for (;;) {
		if (poll(pfd, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pfd[1].revents)
			break;
		if (pfd[0].revents & POLLIN)
			drmHandleEvent(ctx->fd, &ev);
	}
	return NULL;
}

static void wait_fence(int fd, int timeout_ms)
{
	if (fd < 0)
		return;
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	poll(&pfd, 1, timeout_ms);
}

static void committed(struct kms_ctx *ctx, uint32_t fb_id)
{
	if (fb_id != ctx->cur_fb) {
		ctx->prev_fb = ctx->cur_fb;
		ctx->cur_fb = fb_id;
	}
}

/* Legacy path: the caller has already waited for the GPU. */
static int show_legacy(struct kms_ctx *ctx, uint32_t fb_id)
{
	if (!ctx->mode_set) {
		if (drmModeSetCrtc(ctx->fd, ctx->crtc_id, fb_id, 0, 0,
				   &ctx->conn_id, 1, &ctx->mode)) {
			ALOGE("SetCrtc: %s", strerror(errno));
			return -errno;
		}
		ctx->mode_set = true;
		committed(ctx, fb_id);
		return 0;
	}

	pthread_mutex_lock(&ctx->lock);
	unsigned int start = ctx->flips;
	pthread_mutex_unlock(&ctx->lock);

	if (drmModePageFlip(ctx->fd, ctx->crtc_id, fb_id,
			    DRM_MODE_PAGE_FLIP_EVENT, ctx)) {
		/* No flip support or still busy: set the buffer directly. */
		ctx->mode_set = false;
		return show_legacy(ctx, fb_id);
	}
	committed(ctx, fb_id);

	/* Wait for the flip so the previous buffer is free when we return. */
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += 100000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&ctx->lock);
	while (ctx->flips == start)
		if (pthread_cond_timedwait(&ctx->flip_cond, &ctx->lock, &ts)) {
			ALOGE("page flip timed out");
			break;
		}
	pthread_mutex_unlock(&ctx->lock);
	return 0;
}

/*
 * Once the mode is set, ask the kernel whether it accepts a plain atomic
 * page flip before relying on it.
 */
static bool atomic_probe(struct kms_ctx *ctx, uint32_t fb_id)
{
	drmModeAtomicReqPtr req = drmModeAtomicAlloc();
	if (!req)
		return false;
	drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_plane_fb, fb_id);
	drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_plane_crtc,
				 ctx->crtc_id);
	int ret = drmModeAtomicCommit(ctx->fd, req, DRM_MODE_ATOMIC_TEST_ONLY, NULL);
	drmModeAtomicFree(req);
	if (ret)
		ALOGE("atomic test commit: %s, using legacy page flips",
		      strerror(errno));
	return !ret;
}

static int atomic_commit(struct kms_ctx *ctx, uint32_t fb_id, int in_fence,
			 int32_t *out_fence)
{
	drmModeAtomicReqPtr req = drmModeAtomicAlloc();
	if (!req)
		return -ENOMEM;

	*out_fence = -1;
	drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_plane_fb, fb_id);
	drmModeAtomicAddProperty(req, ctx->plane_id, ctx->prop_plane_crtc,
				 ctx->crtc_id);
	if (in_fence >= 0)
		drmModeAtomicAddProperty(req, ctx->plane_id,
					 ctx->prop_plane_in_fence, in_fence);
	drmModeAtomicAddProperty(req, ctx->crtc_id, ctx->prop_crtc_out_fence,
				 (uint64_t)(uintptr_t)out_fence);

	uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
	int ret = drmModeAtomicCommit(ctx->fd, req, flags, ctx);
	/*
	 * EBUSY: the previous frame has not completed yet, because its GPU
	 * work is slow or its 16.7 ms transfer is still running. Its retire
	 * fence signals when the kernel completes that flip; wait for it and
	 * try again. A slow frame (app launch, shader compile) must not end
	 * the atomic path.
	 */
	for (int tries = 0; ret && errno == EBUSY && tries < 3; tries++) {
		if (ctx->last_out >= 0) {
			wait_fence(ctx->last_out, 1000);
		} else {
			struct timespec ts = { 0, (long)ctx->period_ns };
			nanosleep(&ts, NULL);
		}
		ret = drmModeAtomicCommit(ctx->fd, req, flags, ctx);
	}
	if (ret)
		ret = -errno;
	drmModeAtomicFree(req);
	return ret;
}

/*
 * Show fb_id once in_fence signals. Takes ownership of in_fence. Returns
 * the retire fence, or -1 when the frame is already on screen.
 */
static int show(struct kms_ctx *ctx, uint32_t fb_id, int in_fence)
{
	if (ctx->crtc_off) {
		/* Nothing scans out while the CRTC is off: drop the frame. */
		if (in_fence >= 0)
			close(in_fence);
		return -1;
	}
	if (ctx->atomic && ctx->mode_set && !ctx->atomic_probed) {
		ctx->atomic_probed = true;
		ctx->atomic = atomic_probe(ctx, fb_id);
	}
	if (ctx->atomic && ctx->mode_set) {
		int32_t out = -1;
		int ret = atomic_commit(ctx, fb_id, in_fence, &out);
		if (!ret) {
			if (in_fence >= 0)
				close(in_fence);
			committed(ctx, fb_id);
			if (ctx->last_out >= 0)
				close(ctx->last_out);
			ctx->last_out = out >= 0 ? dup(out) : -1;
			return out;
		}
		if (out >= 0)
			close(out);
		if (ret == -EINVAL || ret == -EOPNOTSUPP) {
			/* The kernel rejects this commit itself: stop using it. */
			ALOGE("atomic commit: %s, switching to legacy page flips",
			      strerror(-ret));
			ctx->atomic = false;
		} else {
			/* Transient: show this frame the legacy way, stay atomic. */
			ALOGE("atomic commit: %s, legacy flip for this frame",
			      strerror(-ret));
		}
	}

	/* The first frame sets the mode through the legacy call. */
	wait_fence(in_fence, 3000);
	if (in_fence >= 0)
		close(in_fence);
	if (ctx->last_out >= 0) {
		wait_fence(ctx->last_out, 100);
		close(ctx->last_out);
		ctx->last_out = -1;
	}
	show_legacy(ctx, fb_id);
	return -1;
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
		int acquire = -1;
		for (size_t i = 0; i < c->numHwLayers; i++) {
			hwc_layer_1_t *l = &c->hwLayers[i];
			if (d == 0 && l->compositionType == HWC_FRAMEBUFFER_TARGET) {
				target = l->handle;
				acquire = l->acquireFenceFd;
			} else if (l->acquireFenceFd >= 0) {
				close(l->acquireFenceFd);
			}
			l->acquireFenceFd = -1;
			/*
			 * SurfaceFlinger releases the previous client target
			 * on the retire fence, so no per-layer fence is needed.
			 */
			l->releaseFenceFd = -1;
		}
		c->retireFenceFd = -1;
		uint32_t fb = target ? fb_for(ctx, target) : 0;
		if (fb)
			c->retireFenceFd = show(ctx, fb, acquire);
		else if (acquire >= 0)
			close(acquire);
	}
	return 0;
}

/*
 * Free-running vsync: ticks fall on epoch + k * period, also across
 * vsync off and on, so SurfaceFlinger sees one steady phase. Flip
 * completions are not used as a phase reference: in command mode they
 * land at vsync plus SurfaceFlinger's offset, GPU time and the transfer,
 * and re-anchoring on them moved the phase with every frame.
 */
static void *vsync_loop(void *arg)
{
	struct kms_ctx *ctx = (struct kms_ctx *)arg;

	pthread_mutex_lock(&ctx->lock);
	int64_t last = ctx->epoch;
	while (!ctx->stop) {
		if (!ctx->vsync_on || !ctx->powered) {
			pthread_cond_wait(&ctx->cond, &ctx->lock);
			continue;
		}
		const int64_t epoch = ctx->epoch;
		pthread_mutex_unlock(&ctx->lock);

		/* The next tick after now, never one already sent. */
		int64_t from = now_ns();
		if (from < last)
			from = last;
		int64_t next = epoch +
			((from - epoch) / ctx->period_ns + 1) * ctx->period_ns;
		struct timespec ts = { (time_t)(next / 1000000000LL),
				       (long)(next % 1000000000LL) };
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
			;
		last = next;

		pthread_mutex_lock(&ctx->lock);
		/* A power cycle during the sleep moved the phase: drop this tick. */
		const hwc_procs_t *procs = ctx->vsync_on && ctx->powered &&
			ctx->epoch == epoch ? ctx->procs : NULL;
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

/* Turn the CRTC on or off: atomic ACTIVE, or connector DPMS on the legacy path. */
static int crtc_power(struct kms_ctx *ctx, bool on)
{
	int ret;

	if (ctx->atomic && ctx->prop_crtc_active) {
		drmModeAtomicReqPtr req = drmModeAtomicAlloc();
		if (!req)
			return -ENOMEM;
		drmModeAtomicAddProperty(req, ctx->crtc_id, ctx->prop_crtc_active, on);
		/* Blocking: waits for an in-flight flip before the CRTC changes. */
		ret = drmModeAtomicCommit(ctx->fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET,
					  NULL);
		drmModeAtomicFree(req);
	} else {
		if (!ctx->prop_conn_dpms)
			ctx->prop_conn_dpms = prop_id(ctx->fd, ctx->conn_id,
						      DRM_MODE_OBJECT_CONNECTOR,
						      "DPMS", NULL);
		if (!ctx->prop_conn_dpms)
			return -ENOTSUP;
		ret = drmModeConnectorSetProperty(ctx->fd, ctx->conn_id,
						  ctx->prop_conn_dpms,
						  on ? DRM_MODE_DPMS_ON :
						       DRM_MODE_DPMS_OFF);
	}
	return ret ? -errno : 0;
}

static int hwc_set_power_mode(hwc_composer_device_1_t *dev, int disp, int mode)
{
	struct kms_ctx *ctx = (struct kms_ctx *)dev;
	bool on;

	if (disp != 0)
		return -EINVAL;
	switch (mode) {
	case HWC_POWER_MODE_OFF:
	case HWC_POWER_MODE_DOZE_SUSPEND:
		on = false;
		break;
	case HWC_POWER_MODE_NORMAL:
	case HWC_POWER_MODE_DOZE:
		on = true;
		break;
	default:
		return -EINVAL;
	}

	if (on && ctx->crtc_off) {
		int ret = crtc_power(ctx, true);
		if (ret) {
			/* The next frame sets the mode again through SetCrtc. */
			ALOGE("CRTC on: %s, mode set on the next frame",
			      strerror(-ret));
			ctx->mode_set = false;
		}
		ctx->crtc_off = false;
	} else if (!on && !ctx->crtc_off && ctx->mode_set &&
		   prop_true("vendor.hwc.s6kms.crtc_off")) {
		int ret = crtc_power(ctx, false);
		if (ret)
			ALOGE("CRTC off: %s, leaving it on", strerror(-ret));
		else
			ctx->crtc_off = true;
	}

	pthread_mutex_lock(&ctx->lock);
	if (on && !ctx->powered)
		ctx->epoch = now_ns();
	ctx->powered = on;
	pthread_cond_signal(&ctx->cond);
	pthread_mutex_unlock(&ctx->lock);
	ALOGI("power mode %d: vsync %s, CRTC %s", mode, on ? "on" : "off",
	      ctx->crtc_off ? "off" : "on");
	return 0;
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
	/*
	 * HWC 1 reports dots per thousand inches: px / (mm / 25.4) * 1000.
	 * S6: 577 ppi.
	 */
	return mm ? (int32_t)(px * 25400ULL / mm) : 577000;
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

	snprintf(buff, len, "hwc_kms: crtc %u connector %u %ux%u %s plane %u%s\n",
		 ctx->crtc_id, ctx->conn_id, ctx->mode.hdisplay,
		 ctx->mode.vdisplay, ctx->atomic ? "atomic" : "legacy",
		 ctx->plane_id, ctx->crtc_off ? " crtc off" : "");
}

static int hwc_close(hw_device_t *dev)
{
	struct kms_ctx *ctx = (struct kms_ctx *)dev;

	pthread_mutex_lock(&ctx->lock);
	ctx->stop = true;
	pthread_cond_signal(&ctx->cond);
	pthread_mutex_unlock(&ctx->lock);
	pthread_join(ctx->vsync_thread, NULL);
	if (write(ctx->stop_pipe[1], "x", 1) < 0)
		ALOGE("stop event thread: %s", strerror(errno));
	pthread_join(ctx->event_thread, NULL);
	close(ctx->stop_pipe[0]);
	close(ctx->stop_pipe[1]);
	if (ctx->last_out >= 0)
		close(ctx->last_out);
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

	ctx->last_out = -1;
	int ret = kms_init(ctx);
	if (!ret && pipe2(ctx->stop_pipe, O_CLOEXEC))
		ret = -errno;
	if (ret) {
		if (ctx->fd >= 0)
			close(ctx->fd);
		free(ctx);
		return ret;
	}
	atomic_init(ctx);

	pthread_mutex_init(&ctx->lock, NULL);
	pthread_cond_init(&ctx->cond, NULL);
	pthread_cond_init(&ctx->flip_cond, NULL);
	ctx->powered = true;
	ctx->epoch = now_ns();
	pthread_create(&ctx->vsync_thread, NULL, vsync_loop, ctx);
	pthread_create(&ctx->event_thread, NULL, event_loop, ctx);

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

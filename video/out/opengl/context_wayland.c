/*
 * This file is part of mpv video player.
 * Copyright © 2013 Alexander Preisinger <alexander.preisinger@gmail.com>
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "video/out/present_sync.h"
#include "video/out/wayland_common.h"
#include "context.h"
#include "egl_helpers.h"
#include "utils.h"

#define EGL_PLATFORM_WAYLAND_EXT 0x31D8

struct priv {
    GL gl;
    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    EGLConfig  egl_config;
    EGLConfig  window_config; // the window's surface, if not egl_config
    struct wl_egl_window *egl_window;
    // The video layer under the window's surface, if there is one
    EGLSurface video_surface;
    struct wl_egl_window *video_window;
    int video_w, video_h;
};

static void resize(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct vo_wayland_state *wl = ctx->vo->wl;

    MP_VERBOSE(wl, "Handling resize on the egl side\n");

    const int32_t width = mp_rect_w(wl->geometry);
    const int32_t height = mp_rect_h(wl->geometry);

    vo_wayland_handle_scale(wl);

    vo_wayland_set_opaque_region(wl, ctx->opts.want_alpha);
    if (p->egl_window)
        wl_egl_window_resize(p->egl_window, width, height, 0, 0);

    wl->vo->dwidth  = width;
    wl->vo->dheight = height;
}

static bool wayland_egl_check_visible(struct ra_ctx *ctx)
{
    return vo_wayland_check_visible(ctx->vo);
}

static pl_color_space_t wayland_egl_preferred_csp(struct ra_ctx *ctx)
{
    return vo_wayland_preferred_csp(ctx->vo);
}

static bool wayland_egl_set_color(struct ra_ctx *ctx, struct mp_image_params *params)
{
    vo_wayland_handle_color(ctx->vo->wl, params);
    return true;
}

static void wayland_egl_make_current(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    eglMakeCurrent(p->egl_display, p->egl_surface, p->egl_surface, p->egl_context);
}

static void wayland_egl_make_current_video(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    eglMakeCurrent(p->egl_display, p->video_surface, p->video_surface, p->egl_context);
}

static void wayland_egl_swap_video_buffers(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    wayland_egl_make_current_video(ctx);
    eglSwapBuffers(p->egl_display, p->video_surface);
}

static bool wayland_egl_resize_video(struct ra_ctx *ctx, int width, int height)
{
    struct priv *p = ctx->priv;
    if (width != p->video_w || height != p->video_h) {
        p->video_w = width;
        p->video_h = height;
        wl_egl_window_resize(p->video_window, width, height, 0, 0);
    }
    ra_gl_ctx_resize(ctx->video_swapchain, width, height, 0);
    return true;
}

static void wayland_egl_swap_buffers(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct vo_wayland_state *wl = ctx->vo->wl;

    if (p->video_surface)
        wayland_egl_make_current(ctx);
    eglSwapBuffers(p->egl_display, p->egl_surface);

    if (wl->opts->wl_internal_vsync)
        vo_wayland_wait_frame(wl);

    if (wl->use_present)
        present_sync_swap(wl->present);
}

static void wayland_egl_get_vsync(struct ra_ctx *ctx, struct vo_vsync_info *info)
{
    struct vo_wayland_state *wl = ctx->vo->wl;
    if (wl->use_present)
        present_sync_get_info(wl->present, info);
}

// The window's surface holds the controls over the video layer, with alpha.
// The context has no config then, so that the video layer keeps its own.
static bool choose_alpha_config(struct ra_ctx *ctx, EGLConfig *config)
{
    struct priv *p = ctx->priv;
    const char *exts = eglQueryString(p->egl_display, EGL_EXTENSIONS);
    EGLint num_configs = 0;
    EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    return gl_check_extension(exts, "EGL_KHR_no_config_context") &&
           eglChooseConfig(p->egl_display, attributes, config, 1, &num_configs) &&
           num_configs;
}

static bool egl_create_context(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct vo_wayland_state *wl = ctx->vo->wl;

    if (!(p->egl_display = mpegl_get_display(EGL_PLATFORM_WAYLAND_EXT,
                                             "EGL_EXT_platform_wayland",
                                             wl->display)))
        return false;

    if (eglInitialize(p->egl_display, NULL, NULL) != EGL_TRUE)
        return false;

    struct mpegl_cb cb = {0};
    if (ctx->opts.video_layer && vo_wayland_enable_video_layer(wl)) {
        if (choose_alpha_config(ctx, &p->window_config))
            cb.no_config = true;
        else
            ctx->opts.want_alpha = true;
    }

    if (!mpegl_create_context_cb(ctx, p->egl_display, cb, &p->egl_context,
                                 &p->egl_config))
        return false;
    if (!cb.no_config)
        p->window_config = p->egl_config;

    eglMakeCurrent(p->egl_display, NULL, NULL, p->egl_context);

    mpegl_load_functions(&p->gl, wl->log);

    struct ra_ctx_params params = {
        .make_current       = wl->video_layer ? wayland_egl_make_current : NULL,
        .check_visible      = wayland_egl_check_visible,
        .preferred_csp      = wayland_egl_preferred_csp,
        .set_color          = wayland_egl_set_color,
        .swap_buffers       = wayland_egl_swap_buffers,
        .get_vsync          = wayland_egl_get_vsync,
    };

    if (!ra_gl_ctx_init(ctx, &p->gl, params))
        return false;

    if (wl->video_layer) {
        struct ra_ctx_params video_params = {
            .make_current       = wayland_egl_make_current_video,
            .swap_buffers       = wayland_egl_swap_video_buffers,
        };
        ra_gl_ctx_init_video(ctx, &p->gl, video_params);
    }

    ra_add_native_resource(ctx->ra, "wl", wl->display);

    return true;
}

static void egl_create_window(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;
    struct vo_wayland_state *wl = ctx->vo->wl;

    p->egl_window = wl_egl_window_create(wl->surface,
                                         mp_rect_w(wl->geometry),
                                         mp_rect_h(wl->geometry));

    p->egl_surface = mpegl_create_window_surface(
        p->egl_display, p->window_config, p->egl_window);
    if (p->egl_surface == EGL_NO_SURFACE) {
        p->egl_surface = eglCreateWindowSurface(
            p->egl_display, p->window_config, p->egl_window, NULL);
    }

    eglMakeCurrent(p->egl_display, p->egl_surface, p->egl_surface, p->egl_context);
    // eglMakeCurrent may not configure the draw or read buffers if the context
    // has been made current previously. On nvidia GL_NONE is bound because EGL_NO_SURFACE
    // is used initially and we must bind the read and draw buffers here.
    if(!p->gl.es) {
        p->gl.ReadBuffer(GL_BACK);
        p->gl.DrawBuffer(GL_BACK);
    }

    eglSwapInterval(p->egl_display, 0);

    if (wl->video_layer) {
        p->video_w = mp_rect_w(wl->geometry);
        p->video_h = mp_rect_h(wl->geometry);
        p->video_window = wl_egl_window_create(wl->video_surface, p->video_w,
                                               p->video_h);
        p->video_surface = mpegl_create_window_surface(
            p->egl_display, p->egl_config, p->video_window);
        if (p->video_surface == EGL_NO_SURFACE) {
            p->video_surface = eglCreateWindowSurface(
                p->egl_display, p->egl_config, p->video_window, NULL);
        }
        wayland_egl_make_current_video(ctx);
        eglSwapInterval(p->egl_display, 0);
        wayland_egl_make_current(ctx);
    }
}

static bool wayland_egl_reconfig(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;

    if (!vo_wayland_reconfig(ctx->vo))
        return false;

    if (!p->egl_window)
        egl_create_window(ctx);

    return true;
}

static void wayland_egl_uninit(struct ra_ctx *ctx)
{
    struct priv *p = ctx->priv;

    ra_gl_ctx_uninit(ctx);

    if (p->egl_context) {
        eglReleaseThread();
        if (p->egl_window)
            wl_egl_window_destroy(p->egl_window);
        if (p->video_window)
            wl_egl_window_destroy(p->video_window);
        if (p->video_surface)
            eglDestroySurface(p->egl_display, p->video_surface);
        eglDestroySurface(p->egl_display, p->egl_surface);
        eglMakeCurrent(p->egl_display, NULL, NULL, EGL_NO_CONTEXT);
        eglDestroyContext(p->egl_display, p->egl_context);
        p->egl_context = NULL;
    }
    eglTerminate(p->egl_display);

    vo_wayland_uninit(ctx->vo);
}

static int wayland_egl_control(struct ra_ctx *ctx, int *events, int request,
                             void *data)
{
    struct vo_wayland_state *wl = ctx->vo->wl;
    int r = vo_wayland_control(ctx->vo, events, request, data);

    if (*events & VO_EVENT_RESIZE) {
        resize(ctx);
        ra_gl_ctx_resize(ctx->swapchain, wl->vo->dwidth, wl->vo->dheight, 0);
    }

    return r;
}

static void wayland_egl_wakeup(struct ra_ctx *ctx)
{
    vo_wayland_wakeup(ctx->vo);
}

static void wayland_egl_wait_events(struct ra_ctx *ctx, int64_t until_time_ns)
{
    vo_wayland_wait_events(ctx->vo, until_time_ns);
}

static void wayland_egl_update_render_opts(struct ra_ctx *ctx)
{
    struct vo_wayland_state *wl = ctx->vo->wl;
    vo_wayland_set_opaque_region(wl, ctx->opts.want_alpha);
    wl_surface_commit(wl->surface);
}

static bool wayland_egl_init(struct ra_ctx *ctx)
{
    ctx->priv = talloc_zero(ctx, struct priv);
    if (vo_wayland_init(ctx->vo) && egl_create_context(ctx))
        return true;
    wayland_egl_uninit(ctx);
    return false;
}

const struct ra_ctx_fns ra_ctx_wayland_egl = {
    .type               = "opengl",
    .name               = "wayland",
    .description        = "Wayland/EGL",
    .reconfig           = wayland_egl_reconfig,
    .control            = wayland_egl_control,
    .wakeup             = wayland_egl_wakeup,
    .wait_events        = wayland_egl_wait_events,
    .update_render_opts = wayland_egl_update_render_opts,
    .resize_video       = wayland_egl_resize_video,
    .init               = wayland_egl_init,
    .uninit             = wayland_egl_uninit,
};

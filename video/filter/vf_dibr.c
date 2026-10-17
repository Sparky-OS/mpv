/*
 * Depth-image-based rendering: a picture with its depth map becomes full side by side, both views at the picture's
 * size, the left eye first (libstereo-dibr, the core Sparky Stereo OS shares between its players, FFmpeg and the
 * desktop). The picture and its depth map come packed in one frame, in one of the depth layouts of the stereo depth
 * marks (stereo-layer, docs/depth-marks.md), or the depth map comes with the picture as an image of its own (a depth
 * auxiliary layer, mp_image.depth). What a depth sample means comes from the depth map's side data (the depth
 * representation information of H.264 and H.265, the supplemental information of an ISO/IEC 23002-3 auxiliary video
 * stream), else from the container's Stereo3DDepth mark, else larger samples are nearer.
 *
 * This file is part of mpv.
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

#include <float.h>
#include <math.h>
#include <string.h>

#include "config.h"

#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#if HAVE_AV_DEPTH
#include <libavutil/multiview.h>
#endif

#include <stereo-dibr.h>

#include "common/common.h"
#include "common/msg.h"
#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "options/m_option.h"
#include "video/csputils.h"
#include "video/img_format.h"
#include "video/mp_image.h"
#include "vf_dibr.h"

struct f_opts {
    int packing;
    int view, warp, fill, smooth;
    double sigma_h, sigma_v;
    int radius;
    double range, zero, shift;
    int meaning;
    double near, far;
    char *mark;
};

#define OPT_BASE_STRUCT struct f_opts
static const struct m_option f_opts_list[] = {
    {"packing", OPT_CHOICE(packing,
        {"auto", -1},
        {"depth-sbs", MP_STEREO3D_DEPTH_SBS},
        {"depth-sbs-depth-first", MP_STEREO3D_DEPTH_SBS_DF},
        {"depth-ab", MP_STEREO3D_DEPTH_AB},
        {"depth-ab-depth-first", MP_STEREO3D_DEPTH_AB_DF},
        {"depth-graphics", MP_STEREO3D_DEPTH_GRAPHICS},
        {"depth-background", MP_STEREO3D_DEPTH_BACKGROUND},
        {"depth-separate", MP_STEREO3D_DEPTH_SEPARATE})},
    {"view", OPT_CHOICE_C(view, mp_depth_view_names)},
    {"warp", OPT_CHOICE_C(warp, mp_depth_warp_names)},
    {"fill", OPT_CHOICE_C(fill, mp_depth_fill_names)},
    {"smooth", OPT_CHOICE_C(smooth, mp_depth_smooth_names)},
    {"sigma-h", OPT_DOUBLE(sigma_h), M_RANGE(0, 1000)},
    {"sigma-v", OPT_DOUBLE(sigma_v), M_RANGE(0, 1000)},
    {"radius", OPT_INT(radius), M_RANGE(1, 100)},
    {"range", OPT_DOUBLE(range)},
    {"zero", OPT_DOUBLE(zero), M_RANGE(0, 1)},
    {"shift", OPT_DOUBLE(shift)},
    {"meaning", OPT_CHOICE_C(meaning, mp_depth_meaning_names)},
    {"near", OPT_DOUBLE(near), M_RANGE(0, DBL_MAX)},
    {"far", OPT_DOUBLE(far), M_RANGE(0, DBL_MAX)},
    {"mark", OPT_STRING(mark)},
    {0}
};

static const struct f_opts f_opts_def = {
    .packing = -1,
    .sigma_h = 10,
    .sigma_v = 30,
    .radius = 5,
    .range = 58,
    .zero = 0.5,
    .meaning = -1,
};

struct priv {
    struct f_opts *opts;
    stereo_dibr *dibr;
    float *table, *layer_table;
    uint16_t *lut;
    int table_size;
    bool warned;
};

bool mp_dibr_accepts(int imgfmt)
{
    struct mp_regular_imgfmt d;
    if (!mp_get_regular_imgfmt(&d, imgfmt) || d.component_type != MP_COMPONENT_TYPE_UINT ||
        (d.component_size != 1 && d.component_size != 2) || d.component_pad > 0 || d.num_planes < 1)
        return false;
    for (int n = 0; n < d.num_planes; n++) {
        if (d.planes[n].num_components != 1)
            return false;
    }
    return true;
}

// The planes of an image (or of a part of one, x0 samples in) for libstereo-dibr. A hole of the none fill is black:
// limited range black and neutral chroma for YUV, zero for RGB and full range gray, opaque alpha.
static void fill_picture(struct stereo_dibr_picture *pic, struct mp_image *img, int x0, int w, int h)
{
    struct mp_regular_imgfmt d;
    mp_get_regular_imgfmt(&d, img->imgfmt);
    const int bits = d.component_size * 8 + MPMIN(0, d.component_pad);
    const bool rgb = mp_imgfmt_get_forced_csp(img->imgfmt) == PL_COLOR_SYSTEM_RGB;
    const bool limited = img->params.repr.levels == PL_COLOR_LEVELS_LIMITED;
    memset(pic, 0, sizeof(*pic));
    pic->width = w;
    pic->height = h;
    pic->nb_planes = d.num_planes;
    for (int n = 0; n < d.num_planes; n++) {
        const int c = d.planes[n].components[0]; // 1 to 4: Y, U, V, alpha (or R, G, B, alpha)
        const bool chroma = !rgb && (c == 2 || c == 3);
        struct stereo_dibr_plane *pl = &pic->plane[n];
        pl->log2_w = chroma ? d.chroma_xs : 0;
        pl->log2_h = chroma ? d.chroma_ys : 0;
        pl->bytes = d.component_size;
        pl->stride = img->stride[n];
        pl->data = img->planes[n] + (x0 >> pl->log2_w) * pl->bytes;
        if (c == 4)
            pl->hole = (1u << bits) - 1;
        else if (chroma)
            pl->hole = 1u << (bits - 1);
        else
            pl->hole = !rgb && limited ? 16u << (bits - 8) : 0;
    }
}

static int image_bits(struct mp_image *img)
{
    struct mp_regular_imgfmt d;
    if (!mp_get_regular_imgfmt(&d, img->imgfmt))
        return 0;
    return d.component_size * 8 + MPMIN(0, d.component_pad);
}

static AVBufferRef *side_data(struct mp_image *img, int type)
{
    for (int n = 0; img && n < img->num_ff_side_data; n++) {
        if (img->ff_side_data[n].type == type)
            return img->ff_side_data[n].buf;
    }
    return NULL;
}

// A Stereo3DDepth mark read over c: 1 read, 0 malformed (docs/depth-marks.md: the file is then one picture), -1 error
static int read_mark(struct mp_filter *f, const char *text, int bits, struct stereo_depth_convention *c)
{
    struct priv *p = f->priv;
    uint16_t model[65];
    int nb_model = 0;
    if (stereo_depth_read_mark(text, c, model, &nb_model) < 0)
        return 0;
    c->bits = bits;
    if (nb_model) {
        MP_TARRAY_GROW(p, p->lut, (1 << bits) - 1);
        if (stereo_depth_nonlinear_lut(model, nb_model, bits, p->lut) < 0)
            return -1;
        c->lut = p->lut;
    }
    return 1;
}

// What the depth samples mean: the user's mark, else the user's choice of meaning, else the depth map's side data (or
// the picture's), else the container's mark, else nearness; over the range, the screen and the shift the user chose.
// 1: c filled, 0: a malformed mark (one picture), -1: error.
static int convention(struct mp_filter *f, struct mp_image *picture, struct mp_image *depth, int bits, int view_width,
                      struct stereo_depth_convention *c)
{
    struct priv *p = f->priv;
    struct f_opts *o = p->opts;
    memset(c, 0, sizeof(*c));
    c->bits = bits;
    c->zero = o->zero;
    c->range = o->range;
    c->shift = o->shift;
    c->kind = o->meaning < 0 ? STEREO_DEPTH_NEARNESS : o->meaning;
    if (o->near > 0 && o->far > 0) {
        c->z_present = 1;
        c->z_near = o->near;
        c->z_far = o->far;
    }
    if (o->mark && o->mark[0])
        return read_mark(f, o->mark, bits, c);
    if (o->meaning >= 0)
        return 1;

#if HAVE_AV_DEPTH
    // H.264 H.13.2.3, H.265 G.14.3.4: the first view's parameters
    AVBufferRef *sd = side_data(depth, AV_FRAME_DATA_DEPTH_REPRESENTATION_INFO);
    if (!sd)
        sd = side_data(picture, AV_FRAME_DATA_DEPTH_REPRESENTATION_INFO);
    if (sd) {
        const AVDepthRepresentationInfo *info = (const AVDepthRepresentationInfo *)sd->data;
        const AVDepthRepresentationView *v = info->nb_views ? av_depth_representation_info_get_view(info, 0) : NULL;
        switch (info->type) {
        case AV_DEPTH_REPRESENTATION_INVERSE_Z: c->kind = STEREO_DEPTH_INVERSE_Z; break;
        case AV_DEPTH_REPRESENTATION_DISPARITY: c->kind = STEREO_DEPTH_DISPARITY; break;
        case AV_DEPTH_REPRESENTATION_Z: c->kind = STEREO_DEPTH_Z; break;
        case AV_DEPTH_REPRESENTATION_NONLINEAR_DISPARITY:
            c->kind = STEREO_DEPTH_NONLINEAR_DISPARITY;
            MP_TARRAY_GROW(p, p->lut, (1 << bits) - 1);
            if (av_depth_representation_nonlinear_lut(info, bits, p->lut) < 0)
                return -1;
            c->lut = p->lut;
            break;
        default:
            return 1;
        }
        if (v && info->z_near_present && info->z_far_present && !c->z_present) {
            c->z_present = 1;
            c->z_near = av_multiview_real_to_double(&v->z_near);
            c->z_far = av_multiview_real_to_double(&v->z_far);
        }
        if (v && info->d_min_present && info->d_max_present) {
            // in luma samples of the view (H.265 G.14.3.4)
            c->d_present = 1;
            c->d_min = av_multiview_real_to_double(&v->d_min);
            c->d_max = av_multiview_real_to_double(&v->d_max);
            c->d_width = view_width;
        }
        return 1;
    }

    // ISO/IEC 23002-3, which is not freely available: as open implementations read it
    sd = side_data(depth, AV_FRAME_DATA_AUXILIARY_VIDEO_INFO);
    if (sd) {
        const AVAuxiliaryVideoInfo *info = (const AVAuxiliaryVideoInfo *)sd->data;
        if (info->type == AV_AUXILIARY_VIDEO_PARALLAX) {
            c->kind = STEREO_DEPTH_PARALLAX;
            c->parallax_zero = info->parallax_zero;
            c->parallax_scale = info->parallax_scale;
            c->wref = info->wref;
        } else if (info->nkfar + info->nknear > 0) {
            c->zero = (double)info->nkfar / (info->nkfar + info->nknear);
        }
        return 1;
    }
#endif

    // the container's Stereo3DDepth mark
    AVBufferRef *mark = picture->depth_mark;
    if (mark && mark->size) {
        char *text = talloc_strndup(NULL, (const char *)mark->data, mark->size);
        int r = read_mark(f, text, bits, c);
        talloc_free(text);
        return r;
    }
    return 1;
}

bool mp_dibr_malformed(struct mp_image *img, const char *user_mark, int user_meaning)
{
    struct stereo_depth_convention c = {0};
    if (user_mark && user_mark[0])
        return stereo_depth_read_mark(user_mark, &c, NULL, NULL) < 0;
    if (user_meaning >= 0)
        return false;
#if HAVE_AV_DEPTH
    struct mp_image *depth = img->depth ? img->depth : img;
    if (side_data(depth, AV_FRAME_DATA_DEPTH_REPRESENTATION_INFO) ||
        side_data(img, AV_FRAME_DATA_DEPTH_REPRESENTATION_INFO) || side_data(depth, AV_FRAME_DATA_AUXILIARY_VIDEO_INFO))
        return false;
#endif
    AVBufferRef *mark = img->depth_mark;
    if (!mark || !mark->size)
        return false;
    char *text = talloc_strndup(NULL, (const char *)mark->data, mark->size);
    bool malformed = stereo_depth_read_mark(text, &c, NULL, NULL) < 0;
    talloc_free(text);
    return malformed;
}

static int packing_of(struct mp_filter *f, struct mp_image *img)
{
    struct priv *p = f->priv;
    if (p->opts->packing >= 0)
        return p->opts->packing;
    if (img->depth)
        return MP_STEREO3D_DEPTH_SEPARATE;
    return mp_stereo3d_is_depth(img->params.stereo3d) ? img->params.stereo3d : -1;
}

static struct mp_image *render(struct mp_filter *f, struct mp_image *img)
{
    struct priv *p = f->priv;
    int packing = packing_of(f, img);
    if (packing < 0 || !mp_dibr_accepts(img->imgfmt))
        return NULL;

    struct stereo_dibr_picture frame;
    fill_picture(&frame, img, 0, img->w, img->h);
    struct stereo_dibr_parts parts = {0};
    struct mp_image *depth_img = img;
    int bits = image_bits(img);
    if (packing == MP_STEREO3D_DEPTH_SEPARATE) {
        depth_img = img->depth;
        if (!depth_img || !mp_dibr_accepts(depth_img->imgfmt))
            return NULL;
        bits = image_bits(depth_img);
        parts.picture = frame;
        parts.depth = (struct stereo_dibr_depth){
            .data = depth_img->planes[0], .stride = depth_img->stride[0],
            .bytes = (bits + 7) / 8, .width = depth_img->w, .height = depth_img->h,
        };
    } else {
        int layout = packing - MP_STEREO3D_DEPTH_SBS;
        int packed = layout < 2 ? STEREO_DIBR_PACKING_SIDE_BY_SIDE : layout < 4 ? STEREO_DIBR_PACKING_TOP_BOTTOM :
                     layout == 4 ? STEREO_DIBR_PACKING_GRAPHICS : STEREO_DIBR_PACKING_BACKGROUND;
        bool depth_first = layout < 4 && (layout & 1);
        if (stereo_dibr_unpack(&frame, packed, depth_first, 0, &parts) < 0)
            return NULL;
    }
    const int vw = parts.picture.width, vh = parts.picture.height;
    if (bits < 1 || bits > 16 || vw < 2 || vh < 1)
        return NULL;

    struct stereo_depth_convention c;
    int known = convention(f, img, packing == MP_STEREO3D_DEPTH_SEPARATE ? depth_img : img, bits, vw, &c);
    if (known < 0)
        return NULL;
    if (!known) {
        // a malformed Stereo3DDepth mark: one picture (docs/depth-marks.md, rule 1), the picture part of the frame
        if (!p->warned)
            MP_WARN(f, "Malformed Stereo3DDepth mark: the picture is shown alone.\n");
        p->warned = true;
        struct mp_image *out = mp_image_new_ref(img);
        if (!out)
            return NULL;
        if (packing != MP_STEREO3D_DEPTH_SEPARATE) {
            ptrdiff_t offset = (uint8_t *)parts.picture.plane[0].data - img->planes[0];
            int x = (offset % img->stride[0]) / parts.picture.plane[0].bytes, y = offset / img->stride[0];
            out->params.crop = (struct mp_rect){x, y, x + vw, y + vh};
        }
        talloc_free(out->depth);
        out->depth = NULL;
        av_buffer_unref(&out->depth_mark);
        out->params.stereo3d = MP_STEREO3D_MONO;
        out->params.stereo3d_marked = true;
        out->params.stereo3d_plane_w = out->params.stereo3d_plane_h = 0;
        return out;
    }
    if (p->table_size != (1 << bits)) {
        p->table_size = 1 << bits;
        p->table = talloc_realloc(p, p->table, float, p->table_size);
        p->layer_table = talloc_realloc(p, p->layer_table, float, p->table_size);
    }
    if (stereo_depth_parallax_table(&c, vw, p->table) < 0)
        return NULL;

    struct mp_image *out = mp_image_alloc(img->imgfmt, 2 * vw, vh);
    if (!out)
        return NULL;
    mp_image_copy_attributes(out, img);
    talloc_free(out->depth);
    out->depth = NULL;
    av_buffer_unref(&out->depth_mark);
    out->params.stereo3d = MP_STEREO3D_SBSL;
    out->params.stereo3d_marked = true;
    out->params.stereo3d_plane_w = out->params.stereo3d_plane_h = 0;
    out->params.crop = (struct mp_rect){0, 0, 2 * vw, vh};

    struct stereo_dibr_picture left, right;
    fill_picture(&left, out, 0, vw, vh);
    fill_picture(&right, out, vw, vw, vh);
    struct stereo_dibr_params prm;
    stereo_dibr_params_default(&prm);
    prm.view = p->opts->view;
    prm.warp = p->opts->warp;
    prm.fill = p->opts->fill;
    prm.smooth = p->opts->smooth;
    prm.sigma_h = p->opts->sigma_h;
    prm.sigma_v = p->opts->sigma_v;
    prm.radius = p->opts->radius;
    int r;
    if (parts.layer.width) {
        // a layer's 0 is nothing there (docs/depth-marks.md)
        memcpy(p->layer_table, p->table, p->table_size * sizeof(float));
        p->layer_table[0] = NAN;
        r = stereo_dibr_render_layer(p->dibr, &prm, p->table, p->table_size, &parts.picture, &parts.depth, parts.layer_role,
                                     &parts.layer, p->layer_table, p->table_size, &parts.layer_depth, &left, &right);
    } else {
        r = stereo_dibr_render(p->dibr, &prm, p->table, p->table_size, &parts.picture, &parts.depth, &left, &right);
    }
    if (r < 0) {
        talloc_free(out);
        return NULL;
    }
    return out;
}

static void f_process(struct mp_filter *f)
{
    if (!mp_pin_can_transfer_data(f->ppins[1], f->ppins[0]))
        return;

    struct mp_frame frame = mp_pin_out_read(f->ppins[0]);
    if (mp_frame_is_signaling(frame) || frame.type != MP_FRAME_VIDEO) {
        mp_pin_in_write(f->ppins[1], frame);
        return;
    }

    struct mp_image *img = frame.data;
    struct mp_image *out = render(f, img);
    if (out) {
        talloc_free(img);
        frame.data = out;
    } else if (packing_of(f, img) >= 0) {
        struct priv *p = f->priv;
        if (!p->warned)
            MP_ERR(f, "Cannot draw the views of format %s.\n", mp_imgfmt_to_name(img->imgfmt));
        p->warned = true;
    }
    mp_pin_in_write(f->ppins[1], frame);
}

static void f_destroy(struct mp_filter *f)
{
    struct priv *p = f->priv;
    stereo_dibr_free(p->dibr);
}

static const struct mp_filter_info filter = {
    .name = "dibr",
    .process = f_process,
    .destroy = f_destroy,
    .priv_size = sizeof(struct priv),
};

static struct mp_filter *f_create(struct mp_filter *parent, void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }
    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");
    struct priv *p = f->priv;
    p->opts = talloc_steal(p, options);
    p->dibr = stereo_dibr_alloc();
    MP_HANDLE_OOM(p->dibr);
    return f;
}

const struct mp_user_filter_entry vf_dibr = {
    .desc = {
        .description = "views drawn from a picture with its depth map",
        .name = "dibr",
        .priv_size = sizeof(OPT_BASE_STRUCT),
        .priv_defaults = &f_opts_def,
        .options = f_opts_list,
    },
    .create = f_create,
};

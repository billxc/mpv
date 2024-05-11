/*
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

#include <assert.h>
#include <windows.h>
#include <d3d11.h>

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>

#include "common/common.h"
#include "osdep/timer.h"
#include "osdep/windows_utils.h"
#include "filters/f_autoconvert.h"
#include "filters/filter.h"
#include "filters/filter_internal.h"
#include "filters/user_filters.h"
#include "refqueue.h"
#include "video/hwdec.h"
#include "video/img_format.h"
#include "video/mp_image.h"
#include "video/mp_image_pool.h"

// missing in MinGW
#define D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BLEND 0x1
#define D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_BOB 0x2
#define D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_ADAPTIVE 0x4
#define D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_DEINTERLACE_MOTION_COMPENSATION 0x8
#define D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_INVERSE_TELECINE 0x10
#define D3D11_VIDEO_PROCESSOR_PROCESSOR_CAPS_FRAME_RATE_CONVERSION 0x20

#define SUPER_RESOLUTION_OFF 0
#define SUPER_RESOLUTION_NVIDIA 1
#define SUPER_RESOLUTION_INTEL 2

#define SUPER_RESOLUTION_AUTO 0
#define SUPER_RESOLUTION_720P 1
#define SUPER_RESOLUTION_1080P 2
#define SUPER_RESOLUTION_1440P 3
#define SUPER_RESOLUTION_2160P 4
#define SUPER_RESOLUTION_2X 5
#define SUPER_RESOLUTION_3X 6


struct opts {
    int mode;
    int scale;
};

struct priv {
    struct opts *opts;

    ID3D11Device *vo_dev;

    ID3D11DeviceContext *device_ctx;
    ID3D11VideoDevice *video_dev;
    ID3D11VideoContext *video_ctx;

    ID3D11VideoProcessor *video_proc;
    ID3D11VideoProcessorEnumerator *vp_enum;

    DXGI_FORMAT out_format;

    struct mp_image_params params, out_params;
    int c_w, c_h;

    struct mp_image_pool *pool;

    struct mp_refqueue *queue;
    struct mp_autoconvert *conv;
};

static void release_tex(void *arg)
{
    ID3D11Texture2D *texture = arg;

    ID3D11Texture2D_Release(texture);
}

static struct mp_image *alloc_pool(void *pctx, int fmt, int w, int h)
{
    struct mp_filter *vf = pctx;
    struct priv *p = vf->priv;
    HRESULT hr;

    ID3D11Texture2D *texture = NULL;
    D3D11_TEXTURE2D_DESC texdesc = {
        .Width = w,
        .Height = h,
        .Format = p->out_format,
        .MipLevels = 1,
        .ArraySize = 1,
        .SampleDesc = { .Count = 1 },
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
    };
    hr = ID3D11Device_CreateTexture2D(p->vo_dev, &texdesc, NULL, &texture);
    if (FAILED(hr))
        return NULL;

    struct mp_image *mpi = mp_image_new_custom_ref(NULL, texture, release_tex);
    MP_HANDLE_OOM(mpi);

    mp_image_setfmt(mpi, IMGFMT_D3D11);
    mp_image_set_size(mpi, w, h);
    mpi->params.hw_subfmt = p->out_params.hw_subfmt;

    mpi->planes[0] = (void *)texture;
    mpi->planes[1] = (void *)(intptr_t)0;

    return mpi;
}

static void flush_frames(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    mp_refqueue_flush(p->queue);
}

static void destroy_video_proc(struct mp_filter *vf)
{
    struct priv *p = vf->priv;

    if (p->video_proc)
        ID3D11VideoProcessor_Release(p->video_proc);
    p->video_proc = NULL;

    if (p->vp_enum)
        ID3D11VideoProcessorEnumerator_Release(p->vp_enum);
    p->vp_enum = NULL;
}

// cacluate the render output, given the video size and the window size.
static void get_render_size(int input_w, int input_h,
                                int window_w, int window_h,
                                int *out_w, int *out_h)
{
    // if input larger than window, then keep it as it is.
    if (input_w > window_w || input_h > window_h) {
        *out_w = input_w;
        *out_h = input_h;
    } else {
        // else scale to window_w,window_h as much as possible
        float aspect_ratio = (float)input_w / input_h;
        *out_w = window_w;
        *out_h = (int)(window_w / aspect_ratio);

        // if the height is still larger than the window height after scaling,
        // adjust the width based on the window height
        if (*out_h > window_h) {
            *out_h = window_h;
            *out_w = (int)(window_h * aspect_ratio);
        }
    }
}

static void SetSuperResNvidia(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
     GUID kNvidiaPPEInterfaceGUID = {
        0xd43ce1b3,
        0x1f4b,
        0x48ac,
        {0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7}};
    unsigned int kStreamExtensionVersionV1 = 0x1;
    unsigned int kStreamExtensionMethodSuperResolution = 0x2;

    struct {
        UINT version;
        UINT method;
        UINT enable; // 1 to enable, 0 to disable
    } stream_extension_info = {kStreamExtensionVersionV1,
                                kStreamExtensionMethodSuperResolution,
                                1u};


    HRESULT hr = ID3D11VideoContext_VideoProcessorSetStreamExtension(p->video_ctx,p->video_proc,0,&kNvidiaPPEInterfaceGUID,
        sizeof(stream_extension_info), &stream_extension_info);

    if (FAILED(hr)) {
        MP_ERR(vf, "Failed to enable Nvidia RTX Super RES. Error code: %lx\n", hr);
    }
}

static void SetSuperResIntel(struct mp_filter *vf)
{
    struct priv *p = vf->priv;

    GUID GUID_INTEL_VPE_INTERFACE = {
		  0xedd1d4b9,
		  0x8659,
		  0x4cbc,
		  {0xa4, 0xd6, 0x98, 0x31, 0xa2, 0x16, 0x3a, 0xc3} };

    enum : UINT {
        kIntelVpeFnVersion = 0x01,
        kIntelVpeFnMode = 0x20,
        kIntelVpeFnScaling = 0x37,
    };

    enum : UINT {
        kIntelVpeVersion3 = 0x0003,
    };

    enum : UINT {
        kIntelVpeModeNone = 0x0,
        kIntelVpeModePreproc = 0x01,
    };

    enum : UINT {
        kIntelVpeScalingDefault = 0x0,
        kIntelVpeScalingSuperResolution = 0x2,
    };

    struct IntelVpeExt {
        UINT function;
        void* param;
    };

    struct IntelVpeExt ext = {};
    UINT param = 0;
    ext.param = &param;

    ext.function = kIntelVpeFnVersion;
    param = kIntelVpeVersion3;
    HRESULT hr;
    
    hr = ID3D11VideoContext_VideoProcessorSetOutputExtension(
        p->video_ctx,p->video_proc,
        &GUID_INTEL_VPE_INTERFACE, sizeof(ext), &ext
    );
    
    if (FAILED(hr)) {
        MP_ERR(vf, "Failed to enable Intel RES. Error code: %lx\n", hr);
        return;
    }

    ext.function = kIntelVpeFnMode;
    param = kIntelVpeModePreproc;
    hr = ID3D11VideoContext_VideoProcessorSetOutputExtension(
        p->video_ctx,p->video_proc,
        &GUID_INTEL_VPE_INTERFACE, sizeof(ext), &ext
    );
    
    if (FAILED(hr)) {
        MP_ERR(vf, "Failed to enable Intel RES. Error code: %lx\n", hr);
        return;
    }

    ext.function = kIntelVpeFnScaling;
    param = kIntelVpeScalingSuperResolution;

    hr = ID3D11VideoContext_VideoProcessorSetStreamExtension(
        p->video_ctx,p->video_proc,0,&GUID_INTEL_VPE_INTERFACE,
        sizeof(ext), &ext
    );
    
    if (FAILED(hr)) {
        MP_ERR(vf, "Failed to enable Intel RES. Error code: %lx\n", hr);
    }
}

static int recreate_video_proc(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    HRESULT hr;

    destroy_video_proc(vf);

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC vpdesc = {
        .InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE,
        .InputWidth = p->c_w,
        .InputHeight = p->c_h,
        .OutputWidth = p->out_params.w,
        .OutputHeight = p->out_params.h,
    };
    hr = ID3D11VideoDevice_CreateVideoProcessorEnumerator(p->video_dev, &vpdesc,
                                                          &p->vp_enum);
    if (FAILED(hr))
        goto fail;
    
    D3D11_VIDEO_PROCESSOR_CAPS caps;
    hr = ID3D11VideoProcessorEnumerator_GetVideoProcessorCaps(p->vp_enum, &caps);
    if (FAILED(hr))
        goto fail;

    hr = ID3D11VideoDevice_CreateVideoProcessor(p->video_dev, p->vp_enum, 0,
                                                &p->video_proc);
    if (FAILED(hr)) {
        MP_ERR(vf, "Failed to create D3D11 video processor.\n");
        goto fail;
    }

    // Note: libavcodec does not support cropping left/top with hwaccel.
    RECT src_rc = {
        .right = p->params.w,
        .bottom = p->params.h,
    };


    ID3D11VideoContext_VideoProcessorSetStreamSourceRect(p->video_ctx,
                                                         p->video_proc,
                                                         0, TRUE, &src_rc);

    // This is supposed to stop drivers from fucking up the video quality.
    ID3D11VideoContext_VideoProcessorSetStreamAutoProcessingMode(p->video_ctx,
                                                                 p->video_proc,
                                                                 0, FALSE);

    ID3D11VideoContext_VideoProcessorSetStreamOutputRate(p->video_ctx,
                                                         p->video_proc,
                                                         0,
                                                         D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL,
                                                         FALSE, 0);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE csp = {
        .YCbCr_Matrix = p->params.repr.sys != PL_COLOR_SYSTEM_BT_601,
        .Nominal_Range = p->params.repr.levels == PL_COLOR_LEVELS_LIMITED ? 1 : 2,
    };
    ID3D11VideoContext_VideoProcessorSetStreamColorSpace(p->video_ctx,
                                                         p->video_proc,
                                                         0, &csp);
    ID3D11VideoContext_VideoProcessorSetOutputColorSpace(p->video_ctx,
                                                         p->video_proc,
                                                         &csp);

  
    return 0;
fail:
    destroy_video_proc(vf);
    return -1;
}

static struct mp_image *render(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    int res = -1;
    HRESULT hr;
    ID3D11VideoProcessorInputView *in_view = NULL;
    ID3D11VideoProcessorOutputView *out_view = NULL;
    struct mp_image *in = NULL, *out = NULL;
    out = mp_image_pool_get(p->pool, IMGFMT_D3D11, p->out_params.w, p->out_params.h);
    if (!out) {
        MP_WARN(vf, "failed to allocate frame\n");
        goto cleanup;
    }

    ID3D11Texture2D *d3d_out_tex = (void *)out->planes[0];

    in = mp_refqueue_get(p->queue, 0);
    if (!in)
        goto cleanup;

    ID3D11Texture2D *d3d_tex;
    int d3d_subindex = 0;
    D3D11_VIDEO_FRAME_FORMAT d3d_frame_format = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
   
    if(in->imgfmt == IMGFMT_420P){
        int width = in->w;
        int height = in->h;
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = in->w;
        desc.Height = in->h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_DECODER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ID3D11Device_CreateTexture2D(p->vo_dev,&desc, NULL, &d3d_tex);
        
        // Map the D3D texture to system memory
        D3D11_MAPPED_SUBRESOURCE mapped;

        ID3D11DeviceContext_Map(p->device_ctx,(ID3D11Resource*) d3d_tex,0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            uint8_t * dptr = (uint8_t *)(mapped.pData);

        for (int i = 0; i < height; ++i)
        {
            memcpy(dptr + mapped.RowPitch * i, in->planes[0] + width * i, width);
        }

        // UV values are interleaved and follow the Y values in NV12 format
        // Assuming planes[1] contains U and planes[2] contains V values
        dptr += mapped.RowPitch * height; // Move pointer to the beginning of UV data
        for (int i = 0; i < height / 2; ++i)
        {
            for (int j = 0; j < width / 2; ++j)
            {
                // U value
                *dptr++ = in->planes[1][(i * width / 2) + j];
                // V value
                *dptr++ = in->planes[2][(i * width / 2) + j];
            }
            dptr += mapped.RowPitch - width; // Skip over padding bytes if any
        }

        ID3D11DeviceContext_Unmap(p->device_ctx, (ID3D11Resource*) d3d_tex,0);
    } else {
        d3d_tex = (void *)in->planes[0];
        d3d_subindex = (intptr_t)in->planes[1];
    }


    struct mp_rect bakup_crop = out->params.crop;
    mp_image_copy_attributes(out, in);

    // mp_image_copy_attributes overwrites the height and width
    // set it the size back here
    if (p->opts->mode) {
        mp_image_set_size(out, p->out_params.w, p->out_params.h);
        out->params.crop = bakup_crop;
    }

    D3D11_TEXTURE2D_DESC texdesc;
    ID3D11Texture2D_GetDesc(d3d_tex, &texdesc);
    if (!p->video_proc || p->c_w != texdesc.Width || p->c_h != texdesc.Height)
    {
        p->c_w = texdesc.Width;
        p->c_h = texdesc.Height;
        if (recreate_video_proc(vf) < 0)
            goto cleanup;
    }

    ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(p->video_ctx,
                                                          p->video_proc,
                                                          0, d3d_frame_format);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC indesc = {
        .ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D,
        .Texture2D = {
            .ArraySlice = d3d_subindex,
        },
    };
    hr = ID3D11VideoDevice_CreateVideoProcessorInputView(p->video_dev,
                                                         (ID3D11Resource *)d3d_tex,
                                                         p->vp_enum, &indesc,
                                                         &in_view);
    if (FAILED(hr)) {
        MP_ERR(vf, "Could not create ID3D11VideoProcessorInputView\n");
        goto cleanup;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outdesc = {
        .ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D,
    };
    hr = ID3D11VideoDevice_CreateVideoProcessorOutputView(p->video_dev,
                                                          (ID3D11Resource *)d3d_out_tex,
                                                          p->vp_enum, &outdesc,
                                                          &out_view);
    if (FAILED(hr)) {
        MP_ERR(vf, "Could not create ID3D11VideoProcessorOutputView\n");
        goto cleanup;
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream = {
        .Enable = TRUE,
        .pInputSurface = in_view,
    };
    int frame = mp_refqueue_is_second_field(p->queue);
    switch (p->opts->mode){
        case SUPER_RESOLUTION_NVIDIA:
            SetSuperResNvidia(vf);
            break;
        case SUPER_RESOLUTION_INTEL:
            SetSuperResIntel(vf);
            break;
    }
    hr = ID3D11VideoContext_VideoProcessorBlt(p->video_ctx, p->video_proc,
                                              out_view, frame, 1, &stream);
    if (FAILED(hr)) {
        MP_ERR(vf, "VideoProcessorBlt failed.\n");
        goto cleanup;
    }

    res = 0;
cleanup:
    if (in_view)
        ID3D11VideoProcessorInputView_Release(in_view);
    if (out_view)
        ID3D11VideoProcessorOutputView_Release(out_view);
    if (res < 0)
        TA_FREEP(&out);
    return out;
}

static void vf_d3d11sr_process(struct mp_filter *vf)
{
    struct priv *p = vf->priv;
    struct mp_image *in_fmt = mp_refqueue_execute_reinit(p->queue);
    if (in_fmt) {
        mp_image_pool_clear(p->pool);
        destroy_video_proc(vf);

        p->params = in_fmt->params;
        p->out_params = p->params;
        if (p->opts->mode) {
            int window_w, window_h;
            switch (p->opts->scale) {
                case SUPER_RESOLUTION_720P:
                    window_w = 1280;
                    window_h = 720;
                    break;
                case SUPER_RESOLUTION_AUTO:
                case SUPER_RESOLUTION_1080P:
                    window_w = 1920;
                    window_h = 1080;
                    break;
                case SUPER_RESOLUTION_1440P:
                    window_w = 2560;
                    window_h = 1440;
                    break;
                case SUPER_RESOLUTION_2160P:
                    window_w = 3840;
                    window_h = 2160;
                    break;
                case SUPER_RESOLUTION_2X:
                    window_w = 2 * in_fmt->w;
                    window_h = 2 * in_fmt->h;
                    break;
                case SUPER_RESOLUTION_3X:
                    window_w = 3 * in_fmt->w;
                    window_h = 3 * in_fmt->h;
                    break;
          }
          get_render_size(p->params.w, p->params.h, window_w, window_h,
                          &(p->out_params.w), &(p->out_params.h));
          p->out_params.hw_subfmt = IMGFMT_NV12;
          p->out_format = DXGI_FORMAT_NV12;
        }
    }

    if (!mp_refqueue_can_output(p->queue))
        return;

    if (p->params.w % 2|| p->params.h % 2){
        MP_ERR(vf,"Cannot process video when width or height is uneven value\n");
        mp_filter_internal_mark_failed(vf);
        return;
    }

    if (p->opts->mode == SUPER_RESOLUTION_OFF) {
        struct mp_image *in = mp_image_new_ref(mp_refqueue_get(p->queue, 0));
        if (!in) {
            mp_filter_internal_mark_failed(vf);
            return;
        }
        mp_refqueue_write_out_pin(p->queue, in);
    } else {
        mp_refqueue_write_out_pin(p->queue, render(vf));
    }
}

static void uninit(struct mp_filter *vf)
{
    struct priv *p = vf->priv;

    destroy_video_proc(vf);

    flush_frames(vf);
    talloc_free(p->queue);
    talloc_free(p->pool);

    if (p->video_ctx)
        ID3D11VideoContext_Release(p->video_ctx);

    if (p->video_dev)
        ID3D11VideoDevice_Release(p->video_dev);

    if (p->device_ctx)
        ID3D11DeviceContext_Release(p->device_ctx);

    if (p->vo_dev)
        ID3D11Device_Release(p->vo_dev);
}

static const struct mp_filter_info vf_d3d11sr_filter = {
    .name = "d3d11sr",
    .process = vf_d3d11sr_process,
    .reset = flush_frames,
    .destroy = uninit,
    .priv_size = sizeof(struct priv),
};

static struct mp_filter *vf_d3d11sr_create(struct mp_filter *parent,
                                            void *options)
{
    struct mp_filter *f = mp_filter_create(parent, &vf_d3d11sr_filter);
    if (!f) {
        talloc_free(options);
        return NULL;
    }

    mp_filter_add_pin(f, MP_PIN_IN, "in");
    mp_filter_add_pin(f, MP_PIN_OUT, "out");

    struct priv *p = f->priv;
    p->opts = talloc_steal(p, options);

    p->queue = mp_refqueue_alloc(f);
 
    struct mp_stream_info *info = mp_filter_find_stream_info(f);
    if (!info || !info->hwdec_devs)
        goto fail;

    struct hwdec_imgfmt_request params = {
        .imgfmt = IMGFMT_D3D11,
        .probing = false,
    };
    hwdec_devices_request_for_img_fmt(info->hwdec_devs, &params);

    struct mp_hwdec_ctx *hwctx =
        hwdec_devices_get_by_imgfmt(info->hwdec_devs, IMGFMT_D3D11);
    if (!hwctx || !hwctx->av_device_ref)
        goto fail;
    AVHWDeviceContext *avhwctx = (void *)hwctx->av_device_ref->data;
    AVD3D11VADeviceContext *d3dctx = avhwctx->hwctx;

    p->vo_dev = d3dctx->device;
    ID3D11Device_AddRef(p->vo_dev);

    HRESULT hr;

    hr = ID3D11Device_QueryInterface(p->vo_dev, &IID_ID3D11VideoDevice,
                                     (void **)&p->video_dev);
    if (FAILED(hr))
        goto fail;

    ID3D11Device_GetImmediateContext(p->vo_dev, &p->device_ctx);
    if (!p->device_ctx)
        goto fail;
    hr = ID3D11DeviceContext_QueryInterface(p->device_ctx, &IID_ID3D11VideoContext,
                                            (void **)&p->video_ctx);
    if (FAILED(hr))
        goto fail;

    p->pool = mp_image_pool_new(f);
    mp_image_pool_set_allocator(p->pool, alloc_pool, f);
    mp_image_pool_set_lru(p->pool);

    mp_refqueue_add_in_format(p->queue, IMGFMT_420P, 0);
    mp_refqueue_add_in_format(p->queue, IMGFMT_D3D11, 0);

    mp_refqueue_set_refs(p->queue, 0, 0);
    
    return f;

fail:
    talloc_free(f);
    return NULL;
}

#define OPT_BASE_STRUCT struct opts
static const m_option_t vf_opts_fields[] = {
    {"mode", OPT_CHOICE(mode,
        {"intel", SUPER_RESOLUTION_INTEL},
        {"nvidia", SUPER_RESOLUTION_NVIDIA},
        {"none", SUPER_RESOLUTION_OFF})},
    {"scale", OPT_CHOICE(scale,
        {"2X", SUPER_RESOLUTION_2X},
        {"3X", SUPER_RESOLUTION_3X},
        {"720p", SUPER_RESOLUTION_720P},
        {"1080p", SUPER_RESOLUTION_1080P},
        {"1440p", SUPER_RESOLUTION_1440P},
        {"2160p", SUPER_RESOLUTION_2160P},
        {"auto", SUPER_RESOLUTION_AUTO})},
    {0}
};

const struct mp_user_filter_entry vf_d3d11sr = {
    .desc = {
        .description = "D3D11 Video Post-Process Filter",
        .name = "d3d11sr",
        .priv_size = sizeof(OPT_BASE_STRUCT),
        .priv_defaults = &(const OPT_BASE_STRUCT) {
            .mode = SUPER_RESOLUTION_OFF,
            .scale = SUPER_RESOLUTION_AUTO,
        },
        .options = vf_opts_fields,
    },
    .create = vf_d3d11sr_create,
};

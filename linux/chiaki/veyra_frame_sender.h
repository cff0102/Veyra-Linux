// SPDX-License-Identifier: GPL-3.0-or-later
// Included by the optional AGPLv3 chiaki-ng build; see docs/LINUX_DIRECT_FRAMES.md.
#pragma once
#include "../player/SharedFrames.h"
#include <chrono>
#include <cstdlib>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace veyra { namespace bridge {
class Sender {
public:
    ~Sender(){sws_freeContext(scale_);}
    void send(const AVFrame* f,int64_t pts,int64_t receipt) {
        const char* path=std::getenv("VEYRA_DIRECT_FRAMES");
        if(!path || !*path || !f || f->hw_frames_ctx) return;
        if(receipt-lastLinkCheck_>1000000) {
            lastLinkCheck_=receipt;
            if(mapping_.valid() && !mapping_.stillLinked()) mapping_.close();
        }
        // Explicit SDR-only boundary; do not reinterpret HDR as SDR.
        if(f->color_trc==AVCOL_TRC_SMPTE2084 || f->color_trc==AVCOL_TRC_ARIB_STD_B67 ||
           f->color_primaries==AVCOL_PRI_BT2020 || f->width<16 || f->height<16 ||
           f->width>3840 || f->height>2160) return;
        if(!mapping_.valid() && !mapping_.open(path,false)) return;
        Lock lock(mapping_);
        if(!lock || !mapping_.compatible()) return;
        const auto fmt=static_cast<AVPixelFormat>(f->format);
        scale_=sws_getCachedContext(scale_,f->width,f->height,fmt,f->width,f->height,
            AV_PIX_FMT_RGBA,SWS_POINT|SWS_ACCURATE_RND|SWS_FULL_CHR_H_INT,nullptr,nullptr,nullptr);
        if(!scale_) return;
        int matrix=SWS_CS_ITU709;
        if(f->colorspace==AVCOL_SPC_BT470BG || f->colorspace==AVCOL_SPC_SMPTE170M) matrix=SWS_CS_ITU601;
        else if(f->colorspace!=AVCOL_SPC_BT709 && f->colorspace!=AVCOL_SPC_UNSPECIFIED) return;
        const auto* coefficients=sws_getCoefficients(matrix);
        if(sws_setColorspaceDetails(scale_,coefficients,f->color_range==AVCOL_RANGE_JPEG,
            coefficients,1,0,1<<16,1<<16)<0) return;
        uint8_t* output[]={mapping_.pixels(),nullptr,nullptr,nullptr};
        int strides[]={f->width*4,0,0,0};
        if(sws_scale(scale_,f->data,f->linesize,0,f->height,output,strides)!=f->height) return;
        auto& h=mapping_.header();
        h.width=f->width;h.height=f->height;h.stride=f->width*4;h.bytes=h.stride*h.height;
        h.ptsUs=pts;h.receivedUs=receipt;++h.sequence;
    }
private:
    Mapping mapping_;
    SwsContext* scale_=nullptr;
    int64_t lastLinkCheck_=0;
};
inline void sendFrame(const AVFrame* f,int64_t pts,int64_t receipt) {
    // Called only on Chiaki's frame-delivery thread.
    static Sender sender;
    sender.send(f,pts,receipt);
}
}}

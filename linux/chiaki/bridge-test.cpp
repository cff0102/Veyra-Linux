// SPDX-License-Identifier: GPL-3.0-or-later
#include "veyra_frame_sender.h"
#include <cassert>
#include <cstdio>
#include <sys/wait.h>

int main() {
    char path[]="/tmp/veyra-bridge-test-XXXXXX";
    int fd=mkstemp(path);assert(fd>=0);close(fd);unlink(path);
    veyra::bridge::Mapping receiver;
    assert(receiver.open(path,true));
    veyra::bridge::Mapping second;
    assert(!second.open(path,true)); // never overwrite an existing bridge
    setenv("VEYRA_DIRECT_FRAMES",path,1);
    AVFrame* f=av_frame_alloc();assert(f);
    f->format=AV_PIX_FMT_NV12;f->width=64;f->height=32;
    f->colorspace=AVCOL_SPC_BT709;f->color_range=AVCOL_RANGE_MPEG;f->color_trc=AVCOL_TRC_BT709;
    assert(av_frame_get_buffer(f,32)==0);
    for(int y=0;y<32;++y) memset(f->data[0]+y*f->linesize[0],y<16?16:235,64);
    for(int y=0;y<16;++y) memset(f->data[1]+y*f->linesize[1],128,64);
    veyra::bridge::Sender sender;
    sender.send(f,1000,1000000);
    {
        veyra::bridge::Lock lock(receiver);assert(lock);
        assert(receiver.header().sequence==1 && receiver.header().bytes==64*32*4);
        const auto* pixels=receiver.pixels();
        for(int c=0;c<3;++c) {std::fprintf(stderr,"channel %d black %u white %u\n",c,pixels[c],pixels[64*16*4+c]);assert(pixels[c]<=1);assert(pixels[64*16*4+c]>=254);}
        assert(pixels[3]==255);
        // A busy consumer makes the producer drop, never wait or tear pixels.
        sender.send(f,2000,1016667);
        assert(receiver.header().sequence==1);
    }
    sender.send(f,2000,1016667);assert(receiver.header().sequence==2);
    f->color_trc=AVCOL_TRC_SMPTE2084;sender.send(f,3000,1033333);
    assert(receiver.header().sequence==2); // reject HDR
    f->color_trc=AVCOL_TRC_BT709;
    receiver.close();assert(access(path,F_OK)!=0);
    assert(receiver.open(path,true));
    sender.send(f,4000,3000000);assert(receiver.header().sequence==1); // reconnect
    const auto child=fork();assert(child>=0);
    if(child==0) {
        veyra::bridge::Mapping other;
        if(!other.open(path,false) || !other.lock()) _exit(1);
        other.header().ptsUs=424242;other.unlock();_exit(0);
    }
    int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    assert(receiver.header().ptsUs==424242);
    // Closing an old owner must not unlink a replacement at the same path.
    assert(unlink(path)==0);
    veyra::bridge::Mapping replacement;
    assert(replacement.open(path,true));
    receiver.close();assert(access(path,F_OK)==0);
    av_frame_free(&f);
    std::puts("PASS: process sharing, bounded nonblocking writer, SDR range, HDR rejection, receiver restart, exclusive creation");
}

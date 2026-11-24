// SPDX-License-Identifier: GPL-3.0-or-later
// Local Linux frame transport. No credentials or network protocol data.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <stdexcept>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace veyra { namespace bridge {
constexpr uint64_t Magic=0x564559524146524dULL;
constexpr size_t HeaderBytes=4096, PixelBytes=3840u*2160u*4u, MappingBytes=HeaderBytes+PixelBytes;
struct Header {
    uint64_t magic;
    uint32_t version, width, height, stride, bytes, reserved;
    uint64_t sequence;
    int64_t ptsUs, receivedUs;
};
static_assert(sizeof(Header)<=HeaderBytes,"Header too large");

class Mapping {
public:
    Mapping()=default;
    Mapping(const Mapping&)=delete;
    Mapping& operator=(const Mapping&)=delete;
    ~Mapping() { close(); }
    void close() {
        const bool remove=owner_ && stillLinked();
        if(data_) munmap(data_,MappingBytes);
        if(fd_>=0) ::close(fd_);
        data_=nullptr;fd_=-1;
        if(remove) unlink(path_.c_str());
        owner_=false;path_.clear();
    }
    bool open(const std::string& path,bool create) {
        close();
        fd_=::open(path.c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW|(create?(O_CREAT|O_EXCL):0),0600);
        if(fd_<0) return false;
        owner_=create;path_=path;
        struct stat info{};
        if(fstat(fd_,&info)!=0 || !S_ISREG(info.st_mode) || info.st_uid!=geteuid() || (info.st_mode&077)!=0) {close();return false;}
        if(create && ftruncate(fd_,MappingBytes)!=0) {close();return false;}
        if(!create && info.st_size!=static_cast<off_t>(MappingBytes)) {close();return false;}
        auto* p=mmap(nullptr,MappingBytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd_,0);
        if(p==MAP_FAILED) {close();return false;}
        data_=static_cast<unsigned char*>(p);
        if(create) {
            if(!lock()) {close();return false;}
            std::memset(data_,0,HeaderBytes);
            header().magic=Magic;header().version=1;
            unlock();
        }
        return true;
    }
    bool valid() const {return data_!=nullptr;}
    bool stillLinked() const {
        struct stat current{}, linked{};
        return fd_>=0 && fstat(fd_,&current)==0 && lstat(path_.c_str(),&linked)==0 &&
            current.st_dev==linked.st_dev && current.st_ino==linked.st_ino;
    }
    bool lock() {return fd_>=0 && flock(fd_,LOCK_EX|LOCK_NB)==0;}
    void unlock() {flock(fd_,LOCK_UN);}
    Header& header() {return *reinterpret_cast<Header*>(data_);}
    unsigned char* pixels() {return data_+HeaderBytes;}
    bool compatible() {return header().magic==Magic && header().version==1;}
private:
    int fd_=-1;
    unsigned char* data_=nullptr;
    bool owner_=false;
    std::string path_;
};
class Lock {
public:
    explicit Lock(Mapping& m):m_(m),held_(m.lock()) {}
    ~Lock(){if(held_)m_.unlock();}
    explicit operator bool() const {return held_;}
private:
    Mapping& m_;bool held_;
};
}}

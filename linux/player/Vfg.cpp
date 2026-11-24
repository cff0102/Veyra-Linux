#include "Vfg.h"
#include <cuda.h>
#include <cudaGL.h>
#include <dlfcn.h>
#include <array>
#include <filesystem>
#include <cstring>
#include <stdexcept>

namespace veyra::linuxplayer {
namespace {
void check(CUresult result, const char* operation) {
    if (result == CUDA_SUCCESS) return;
    const char* message = nullptr;
    cuGetErrorString(result, &message);
    throw std::runtime_error(std::string(operation) + ": " + (message ? message : "CUDA error"));
}
struct Current {
    explicit Current(CUcontext context) { check(cuCtxPushCurrent(context), "CUDA context"); }
    ~Current() { CUcontext previous; cuCtxPopCurrent(&previous); }
};
template<class T> T symbol(void* library, const char* name) {
    auto result = reinterpret_cast<T>(dlsym(library, name));
    if (!result) throw std::runtime_error(std::string("Missing runtime symbol: ") + name);
    return result;
}
}

struct Vfg::Impl {
    QOpenGLFunctions_3_3_Core* gl = nullptr;
    std::vector<void*> libraries;
    CUcontext context = nullptr;
    CUdevice device = 0;
    CUstream stream = nullptr;
    void* effect = nullptr;
    int width = 0, height = 0, multiplier = 0;
    std::array<CUdeviceptr, 3> pixels{};
    std::array<void*, 3> images{};
    std::array<void*, 2> staging{};
    std::vector<GLuint> textures;
    std::vector<CUgraphicsResource> resources;

    int (*create)(const char*, void**) = nullptr;
    void (*destroy)(void*) = nullptr;
    int (*setU32)(void*, const char*, unsigned) = nullptr;
    int (*setImage)(void*, const char*, void*) = nullptr;
    int (*setStream)(void*, const char*, CUstream) = nullptr;
    int (*load)(void*) = nullptr;
    int (*run)(void*, int) = nullptr;
    int (*imageCreate)(unsigned,unsigned,int,int,unsigned,unsigned,unsigned,void**) = nullptr;
    void (*imageDealloc)(void*) = nullptr;
    void (*imageDestroy)(void*) = nullptr;
    int (*imageInit)(void*,unsigned,unsigned,int,void*,int,int,unsigned,unsigned) = nullptr;
    const char* (*errorString)(int) = nullptr;

    void cv(int result, const char* operation) {
        if (result == 0) return;
        const char* detail = errorString ? errorString(result) : nullptr;
        throw std::runtime_error(std::string(operation) + ": " +
            (detail ? detail : std::to_string(result)));
    }
    void* library(const std::filesystem::path& path) {
        auto* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!handle) throw std::runtime_error(path.string() + ": " + dlerror());
        libraries.push_back(handle);
        return handle;
    }
    ~Impl() {
        if (context) {
            CUcontext old = nullptr;
            if (cuCtxPushCurrent(context) == CUDA_SUCCESS) {
                // Only lifecycle teardown waits on CPU; processing uses CUDA/GL
                // interop ordering, with no GPU->CPU pixel readback.
                if (stream) cuStreamSynchronize(stream);
                if (effect && destroy) destroy(effect);
                for (auto resource : resources) if (resource) cuGraphicsUnregisterResource(resource);
                if (gl && !textures.empty()) gl->glDeleteTextures(int(textures.size()), textures.data());
                for (auto image : images) if (image && imageDestroy) imageDestroy(image);
                for (auto pointer : pixels) if (pointer) cuMemFree(pointer);
                for (auto pointer : staging) if (pointer) cuMemFreeHost(pointer);
                if (stream) cuStreamDestroy(stream);
                cuCtxPopCurrent(&old);
            }
            cuDevicePrimaryCtxRelease(device);
        }
        for (auto it = libraries.rbegin(); it != libraries.rend(); ++it) dlclose(*it);
    }
};

Vfg::Vfg() = default;
Vfg::~Vfg() = default;
void Vfg::reset() { impl_.reset(); }
bool Vfg::ready() const {
    if (!impl_) return true;
    Current current(impl_->context);
    auto result=cuStreamQuery(impl_->stream);
    if (result==CUDA_ERROR_NOT_READY) return false;
    check(result,"Check frame-generation completion");
    return true;
}

void Vfg::initialize(const std::string& runtime, int width, int height,
                     int multiplier, int quality, QOpenGLFunctions_3_3_Core* gl) {
    reset();
    if (!std::filesystem::path(runtime).is_absolute())
        throw std::runtime_error("Choose an absolute NVIDIA VFX runtime directory.");
    if (width < 16 || height < 16 || width > 7680 || height > 4320 ||
        multiplier < 2 || multiplier > 4 || quality < 0 || quality > 2)
        throw std::runtime_error("Unsupported frame size, multiplier or quality.");
    auto candidate = std::make_unique<Impl>();
    auto& p = *candidate;
    p.gl = gl; p.width = width; p.height = height; p.multiplier = multiplier;
    check(cuInit(0), "CUDA initialization");
    unsigned count = 0;
    check(cuGLGetDevices(&count, &p.device, 1, CU_GL_DEVICE_LIST_CURRENT_FRAME), "Find display GPU");
    if (count != 1) throw std::runtime_error("Preview must run on one NVIDIA display GPU.");
    check(cuDevicePrimaryCtxRetain(&p.context, p.device), "Retain CUDA context");
    Current current(p.context);
    check(cuStreamCreate(&p.stream, CU_STREAM_NON_BLOCKING), "Create stream");
    for (auto& pointer : p.staging)
        check(cuMemHostAlloc(&pointer,size_t(width)*height*4,CU_MEMHOSTALLOC_PORTABLE),"Allocate upload staging");
    const std::filesystem::path root(runtime);
    for (const char* dependency : {"libnppc.so.12", "libnppial.so.12", "libnppicc.so.12",
            "libnppidei.so.12", "libnppig.so.12"}) p.library(root / dependency);
    auto imageLibrary = p.library(root / "libNVCVImage.so");
    p.library(root / "libVideoFXLocal.so");
    auto core = p.library(root / "libVideoFX.so");
    p.library(root / "libnvVFXVideoFrameGeneration.so");
#define BIND(field, lib, name) p.field = symbol<decltype(p.field)>(lib, name)
    BIND(create, core, "NvVFX_CreateEffect"); BIND(destroy, core, "NvVFX_DestroyEffect");
    BIND(setU32, core, "NvVFX_SetU32"); BIND(setImage, core, "NvVFX_SetImage");
    BIND(setStream, core, "NvVFX_SetCudaStream"); BIND(load, core, "NvVFX_Load");
    BIND(run, core, "NvVFX_Run");
    BIND(imageCreate, imageLibrary, "NvCVImage_Create");
    BIND(imageDealloc, imageLibrary, "NvCVImage_Dealloc");
    BIND(imageDestroy, imageLibrary, "NvCVImage_Destroy");
    BIND(imageInit, imageLibrary, "NvCVImage_Init");
    BIND(errorString, imageLibrary, "NvCV_GetErrorStringFromCode");
#undef BIND
    // Public NvCV API enum values: RGBA=6, U8=1, interleaved=0, GPU=1.
    for (size_t i = 0; i < p.pixels.size(); ++i) {
        check(cuMemAlloc(&p.pixels[i], size_t(width)*height*4), "Allocate RGBA surface");
        p.cv(p.imageCreate(1,1,6,1,0,0,1,&p.images[i]), "Create image descriptor");
        p.imageDealloc(p.images[i]);
        p.cv(p.imageInit(p.images[i],width,height,width*4,
             reinterpret_cast<void*>(p.pixels[i]),6,1,0,1), "Bind CUDA pixels");
    }
    p.cv(p.create("VideoFrameGeneration", &p.effect), "Create VFG");
    p.cv(p.setStream(p.effect,"CudaStream",p.stream), "Set stream");
    p.cv(p.setU32(p.effect,"InputWidth",width), "Set width");
    p.cv(p.setU32(p.effect,"InputHeight",height), "Set height");
    p.cv(p.setU32(p.effect,"Mode",quality), "Set quality");
    p.cv(p.setU32(p.effect,"AutomaticShotChangeDetectionEnabled",1), "Enable cut detection");
    p.cv(p.setImage(p.effect,"SrcImage0",p.images[0]), "Set previous image");
    p.cv(p.setImage(p.effect,"SrcImage1",p.images[1]), "Set current image");
    p.cv(p.load(p.effect), "Load VFG model");
    p.cv(p.setImage(p.effect,"DstImage0",p.images[2]), "Set output image");
    p.cv(p.setU32(p.effect,"FrameMultiplier",multiplier), "Set multiplier");
    p.textures.resize(multiplier+1);
    p.resources.resize(multiplier+1);
    gl->glGenTextures(int(p.textures.size()),p.textures.data());
    for (size_t i=0; i<p.textures.size(); ++i) {
        gl->glBindTexture(GL_TEXTURE_2D,p.textures[i]);
        gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        gl->glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        check(cuGraphicsGLRegisterImage(&p.resources[i],p.textures[i],GL_TEXTURE_2D,
              CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD), "Register display texture");
    }
    gl->glBindTexture(GL_TEXTURE_2D,0);
    impl_ = std::move(candidate);
}

const std::vector<GLuint>& Vfg::process(const QImage& a, const QImage& b, bool cut) {
    if (!impl_) throw std::runtime_error("Frame generation is not initialized.");
    if (!ready()) throw std::runtime_error("Previous GPU batch is still in use.");
    auto& p=*impl_;
    for (const auto* image : {&a, &b})
        if (image->size()!=QSize(p.width,p.height) || image->format()!=QImage::Format_RGBA8888)
            throw std::runtime_error("VFG input must be matching RGBA8 frames.");
    Current current(p.context);
    const QImage* inputs[]={&a,&b};
    for (int i=0;i<2;++i) {
        CUDA_MEMCPY2D copy{};
        for (int row=0;row<p.height;++row)
            std::memcpy(static_cast<char*>(p.staging[i])+size_t(row)*p.width*4,
                        inputs[i]->constScanLine(row),size_t(p.width)*4);
        copy.srcMemoryType=CU_MEMORYTYPE_HOST; copy.srcHost=p.staging[i];
        copy.srcPitch=p.width*4; copy.dstMemoryType=CU_MEMORYTYPE_DEVICE;
        copy.dstDevice=p.pixels[i]; copy.dstPitch=p.width*4;
        copy.WidthInBytes=p.width*4; copy.Height=p.height;
        // Owned pinned staging survives asynchronous upload. ready() prevents
        // reuse while the previous batch is still consuming these buffers.
        check(cuMemcpy2DAsync(&copy,p.stream), "Upload source");
    }
    check(cuGraphicsMapResources(unsigned(p.resources.size()),p.resources.data(),p.stream), "Map display textures");
    try {
        p.cv(p.setImage(p.effect,"SrcImage0",p.images[0]), "Bind previous frame");
        p.cv(p.setImage(p.effect,"SrcImage1",p.images[1]), "Bind current frame");
        p.cv(p.setU32(p.effect,"ShotChange",cut ? 1 : 0), "Set discontinuity");
        for (int i=0;i<=p.multiplier;++i) {
            CUdeviceptr source=p.pixels[i==0 ? 0 : 1];
            if (i>0 && i<p.multiplier) {
                p.cv(p.setU32(p.effect,"FrameIndex",i), "Select generated frame");
                p.cv(p.run(p.effect,1), "Generate frame");
                source=p.pixels[2];
            }
            CUarray target=nullptr;
            check(cuGraphicsSubResourceGetMappedArray(&target,p.resources[i],0,0), "Map texture array");
            CUDA_MEMCPY2D copy{};
            copy.srcMemoryType=CU_MEMORYTYPE_DEVICE; copy.srcDevice=source; copy.srcPitch=p.width*4;
            copy.dstMemoryType=CU_MEMORYTYPE_ARRAY; copy.dstArray=target;
            copy.WidthInBytes=p.width*4; copy.Height=p.height;
            check(cuMemcpy2DAsync(&copy,p.stream), "Copy frame to display texture");
        }
    } catch (...) {
        cuGraphicsUnmapResources(unsigned(p.resources.size()),p.resources.data(),p.stream);
        throw;
    }
    check(cuGraphicsUnmapResources(unsigned(p.resources.size()),p.resources.data(),p.stream), "Publish textures");
    return p.textures;
}
}

#pragma once

#include <QImage>
#include <QOpenGLFunctions_3_3_Core>
#include <memory>
#include <string>
#include <vector>

namespace veyra::linuxplayer {

// Native Linux counterpart of pipeline/VfgBackend.cpp. The public SDK owns
// opaque image descriptors; all pixels remain CUDA/GL-owned. No SDK ABI structs
// or proprietary headers/models are copied into the source tree.
class Vfg {
public:
    Vfg();
    ~Vfg();
    Vfg(const Vfg&) = delete;
    Vfg& operator=(const Vfg&) = delete;
    void initialize(const std::string& runtime, int width, int height,
                    int multiplier, int quality, QOpenGLFunctions_3_3_Core* gl);
    // Current GL context must be the same context used by initialize().
    // Returns A, interpolated frames, B; the textures live until next process().
    const std::vector<GLuint>& process(const QImage& a, const QImage& b, bool cut = false);
    void reset();
    bool ready() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

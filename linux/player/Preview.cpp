#include "Preview.h"
#include <QDir>
#include <QOffscreenSurface>
#include <QPainter>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <numeric>

namespace veyra::linuxplayer {
Preview::Preview(QWidget* parent):QOpenGLWidget(parent) {
    setMinimumSize(480,270);
    clock_.start();
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(2);
    connect(&timer_,&QTimer::timeout,this,[this]{
        const bool nextFrameDue=!batch_.empty() && displayed_<multiplier_ &&
            clock_.nsecsElapsed()/1000>=epoch_+duration_*(displayed_+1)/multiplier_;
        if (!pending_.isNull() || nextFrameDue) update();
    });
    timer_.start();
    connect(this,&QOpenGLWidget::frameSwapped,this,[this] {
        if (!timingEnabled_ || !awaitingSwap_) return;
        awaitingSwap_=false;
        const auto now=clock_.nsecsElapsed()/1000;
        // Application/compositor submission timing, not physical scanout or
        // controller-to-screen latency. Interpolated content time is estimated.
        if (contentAges_.size()<4096) {
            contentAges_.push_back(std::max<qint64>(0,now-presentedContentArrival_));
            latestAges_.push_back(std::max<qint64>(0,now-presentedLatestArrival_));
        }
    });
}
Preview::~Preview() { cleanup(); }
void Preview::cleanup() {
    if (!initialized_) return;
    makeCurrent();
    vfg_.reset();
    shader_.removeAllShaders();
    glDeleteTextures(1,&plain_);
    glDeleteVertexArrays(1,&vao_);
    doneCurrent();
    initialized_=false;
}
void Preview::clearSource() {
    pending_=QImage(); previous_=QImage();
    pendingPts_=previousPts_=-1;
    awaitingSwap_=false;
    batch_.clear(); textureSize_=QSize(); dirty_=true; displayed_=-1;
    update();
}
void Preview::configure(QString runtime,int multiplier,int quality) {
    runtime_=std::move(runtime); multiplier_=multiplier; quality_=quality;
    clearSource();
}
void Preview::submit(QImage image,qint64 pts,qint64 deliveryAgeUs) {
    if (image.isNull()) return;
    ++received;
    if (!pending_.isNull()) ++dropped;
    pending_=std::move(image);
    pendingPts_=pts;
    pendingArrival_=clock_.nsecsElapsed()/1000-std::max<qint64>(0,deliveryAgeUs);
    update();
}
QString Preview::takeTimingSummary() {
    if (contentAges_.empty()) return {};
    auto summarize=[](std::vector<qint64>& samples) {
        std::sort(samples.begin(),samples.end());
        const double average=std::accumulate(samples.begin(),samples.end(),0.0)/samples.size()/1000.0;
        const double p95=samples[(samples.size()-1)*95/100]/1000.0;
        return QString("avg=%1ms p95=%2ms").arg(average,0,'f',2).arg(p95,0,'f',2);
    };
    const auto result=QString("Presentation timing: samples=%1 content-age %2 latest-receipt-to-swap %3")
        .arg(contentAges_.size()).arg(summarize(contentAges_)).arg(summarize(latestAges_));
    contentAges_.clear(); latestAges_.clear();
    return result;
}
void Preview::initializeGL() {
    if (!initializeOpenGLFunctions()) {
        if (report) report("Desktop OpenGL 3.3 is required for this preview.");
        return;
    }
    glGenVertexArrays(1,&vao_);
    glGenTextures(1,&plain_);
    glBindTexture(GL_TEXTURE_2D,plain_);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    bool ok=shader_.addShaderFromSourceCode(QOpenGLShader::Vertex,R"(
        #version 330 core
        out vec2 uv;
        void main() {
            vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);
            uv=vec2(p.x,1.0-p.y);
            gl_Position=vec4(p*2.0-1.0,0.0,1.0);
        })");
    ok=ok && shader_.addShaderFromSourceCode(QOpenGLShader::Fragment,R"(
        #version 330 core
        uniform sampler2D picture;
        uniform vec2 texel;
        uniform float sharpen;
        in vec2 uv;
        out vec4 color;
        void main() {
            vec3 c=texture(picture,uv).rgb;
            vec3 average=(texture(picture,uv+vec2(texel.x,0)).rgb+
                          texture(picture,uv-vec2(texel.x,0)).rgb+
                          texture(picture,uv+vec2(0,texel.y)).rgb+
                          texture(picture,uv-vec2(0,texel.y)).rgb)*0.25;
            color=vec4(clamp(c+(c-average)*sharpen,0.0,1.0),1.0);
        })");
    if (!ok || !shader_.link()) {
        if (report) report("OpenGL shader error: "+shader_.log());
        return;
    }
    initialized_=true;
    connect(context(),&QOpenGLContext::aboutToBeDestroyed,this,[this]{cleanup();},Qt::DirectConnection);
    if (report) report(QString("Native OpenGL: %1").arg(reinterpret_cast<const char*>(glGetString(GL_RENDERER))));
}
void Preview::paintGL() {
    if (!initialized_) return;
    glClearColor(0.025f,0.03f,0.045f,1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (dirty_) { vfg_.reset(); modelSize_=QSize(); batch_.clear(); dirty_=false; }
    auto now=clock_.nsecsElapsed()/1000;
    bool gpuReady=true;
    try { gpuReady=vfg_.ready(); }
    catch (const std::exception& e) {
        multiplier_=1; vfg_.reset(); batch_.clear(); textureSize_=QSize();
        if (report) report(QString::fromUtf8(e.what()));
        if (failed) failed();
    }
    // Accept the next source as soon as it arrives. Waiting for the previous
    // interval to expire here creates a feedback loop: a missed repaint drops a
    // source, lengthens the next interval, and causes more source drops.
    if (!pending_.isNull() && gpuReady) {
        QImage next=std::move(pending_);
        const auto pts=pendingPts_;
        const auto arrival=pendingArrival_;
        pending_=QImage();
        if (deduplicate_ && next==previous_ && pts>previousPts_ && pts-previousPts_<100000) {
            ++duplicates;
        } else {
            const auto delta=pts-previousPts_;
            const bool discontinuity=previous_.isNull() || next.size()!=previous_.size() || delta<=0 || delta>100000;
            try {
                if (multiplier_>1 && !discontinuity) {
                    if (modelSize_!=next.size()) {
                        if (report) report("Loading NVIDIA frame-generation model…");
                        vfg_.initialize(runtime_.toStdString(),next.width(),next.height(),multiplier_,quality_,this);
                        modelSize_=next.size();
                        if (report) report("NVIDIA video frame generation active • SDR • CUDA → OpenGL");
                    }
                    batch_=vfg_.process(previous_,next);
                    generated+=multiplier_-1;
                    duration_=std::clamp<qint64>(delta,8000,100000);
                    if(latencyPriority_ && interpolationBudgetUs_>0)
                        duration_=std::min(duration_,interpolationBudgetUs_*multiplier_/(multiplier_-1));
                    // Re-anchor to the live source arrival. Extrapolating the
                    // media PTS indefinitely drifts when capture delivery and
                    // the compositor clock differ, hiding all intermediate frames.
                    // Latency priority skips the already-visible A endpoint.
                    // Anchor to receipt before conversion/inference so slow
                    // work consumes the budget rather than extending it.
                    epoch_=latencyPriority_ ? arrival-duration_/multiplier_ : clock_.nsecsElapsed()/1000;
                    batchFirstArrival_=previousArrival_;
                    batchLastArrival_=arrival;
                    displayed_=-1;
                } else {
                    // A pause or dropped capture interval invalidates the pair,
                    // but does not require reloading the model. This also avoids
                    // a reload loop after first-use model warmup stalls the GUI.
                    if (discontinuity) batch_.clear();
                    glBindTexture(GL_TEXTURE_2D,plain_);
                    glPixelStorei(GL_UNPACK_ALIGNMENT,4);
                    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,next.width(),next.height(),0,GL_RGBA,GL_UNSIGNED_BYTE,next.constBits());
                    ++submitted;
                    presentedContentArrival_=presentedLatestArrival_=arrival;
                    awaitingSwap_=true;
                }
                textureSize_=next.size();
                previous_=std::move(next); previousPts_=pts; previousArrival_=arrival;
            } catch (const std::exception& error) {
                multiplier_=1; vfg_.reset(); modelSize_=QSize(); batch_.clear();
                previous_=QImage(); textureSize_=QSize();
                if (report) report("Frame generation stopped: "+QString::fromUtf8(error.what()));
                if (failed) failed();
            }
        }
    }
    if (textureSize_.isEmpty()) return;
    GLuint texture=plain_;
    if (!batch_.empty()) {
        now=clock_.nsecsElapsed()/1000;
        const int index=std::clamp<int>(int((now-epoch_)*multiplier_/duration_),0,multiplier_);
        texture=batch_[index];
        if (index!=displayed_) {
            ++submitted; displayed_=index;
            presentedContentArrival_=batchFirstArrival_+(batchLastArrival_-batchFirstArrival_)*index/multiplier_;
            presentedLatestArrival_=batchLastArrival_;
            awaitingSwap_=true;
        }
    }
    const int w=int(width()*devicePixelRatioF()),h=int(height()*devicePixelRatioF());
    QSize view=textureSize_; view.scale(w,h,Qt::KeepAspectRatio);
    glViewport((w-view.width())/2,(h-view.height())/2,view.width(),view.height());
    shader_.bind();
    shader_.setUniformValue("picture",0);
    shader_.setUniformValue("texel",QVector2D(1.f/textureSize_.width(),1.f/textureSize_.height()));
    shader_.setUniformValue("sharpen",sharpen_);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,texture);
    glBindVertexArray(vao_); glDrawArrays(GL_TRIANGLES,0,3);
    glBindVertexArray(0); shader_.release();
    glViewport(0,0,w,h);
}

int gpuSelfTest(const QString& runtime,const QString& outputDirectory) {
    QOffscreenSurface surface; surface.setFormat(QSurfaceFormat::defaultFormat()); surface.create();
    QOpenGLContext context; context.setFormat(surface.format());
    if (!context.create() || !context.makeCurrent(&surface)) { qCritical()<<"Cannot create GL test context"; return 1; }
    QOpenGLFunctions_3_3_Core gl;
    if (!gl.initializeOpenGLFunctions()) {qCritical()<<"OpenGL 3.3 functions unavailable:"<<context.format();return 1;}
    qInfo()<<"GPU:"<<reinterpret_cast<const char*>(gl.glGetString(GL_RENDERER));
    QDir().mkpath(outputDirectory);
    const QSize size(640,360);
    auto frame=[&](int x) {
        QImage image(size,QImage::Format_RGBA8888); image.fill(QColor(25,35,55));
        QPainter painter(&image);
        painter.fillRect(x,100,100,100,QColor(230,100,40));
        painter.fillRect(20,20,30,30,Qt::red);
        painter.fillRect(60,20,30,30,Qt::green);
        painter.fillRect(100,20,30,30,Qt::blue);
        return image;
    };
    auto a=frame(180),b=frame(220);
    auto read=[&](GLuint texture) {
        QImage image(size,QImage::Format_RGBA8888);
        gl.glBindTexture(GL_TEXTURE_2D,texture);
        gl.glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,image.bits());
        return image;
    };
    try {
        for (int multiplier : {2,4}) {
            Vfg engine;
            engine.initialize(runtime.toStdString(),size.width(),size.height(),multiplier,0,&gl);
            QElapsedTimer time; time.start();
            const auto textures=engine.process(a,b);
            auto first=read(textures.front()),last=read(textures.back());
            if (first!=a || last!=b) throw std::runtime_error("Source upload/color identity failed");
            for (int i=1;i<multiplier;++i) {
                auto generated=read(textures[i]);
                generated.save(outputDirectory+QString("/generated-%1x-%2.png").arg(multiplier).arg(i));
                if (generated==a || generated==b) throw std::runtime_error("Model repeated an endpoint instead of interpolating");
                double mass=0,weighted=0;
                for (int y=90;y<210;++y) for(int x=130;x<350;++x) {
                    const auto c=generated.pixelColor(x,y);
                    if (c.red()>150 && c.green()<150) { mass++; weighted+=x; }
                }
                const double expected=229.5+40.0*i/multiplier;
                if (!mass || std::abs(weighted/mass-expected)>8)
                    throw std::runtime_error("Moving-object interpolation position failed");
                qInfo()<<multiplier<<"x index"<<i<<"object centroid"<<weighted/mass<<"expected"<<expected;
            }
            qInfo()<<"First pair including warmup, ms:"<<time.elapsed();
            // Reset/shot-change must show B without blending scenes.
            const auto cut=engine.process(a,b,true);
            if (read(cut[1])!=b) throw std::runtime_error("Shot-change bypass failed");
            for (int i=0;i<5;++i) { engine.process(a,b); gl.glFinish(); }
            engine.reset();
        }
        a.save(outputDirectory+"/source-a.png"); b.save(outputDirectory+"/source-b.png");
        {
            const auto a1080=a.scaled(1920,1080), b1080=b.scaled(1920,1080);
            Vfg engine;
            engine.initialize(runtime.toStdString(),1920,1080,4,0,&gl);
            engine.process(a1080,b1080); gl.glFinish(); // warm up outside timing
            std::vector<double> milliseconds;
            for(int i=0;i<30;++i) {
                QElapsedTimer time; time.start();
                engine.process(a1080,b1080); gl.glFinish();
                milliseconds.push_back(time.nsecsElapsed()/1e6);
            }
            qInfo()<<"1080p 4x low model: 30 warmed pairs; upload + inference + texture copies mean ms"
                   <<std::accumulate(milliseconds.begin(),milliseconds.end(),0.0)/milliseconds.size()
                   <<"max ms"<<*std::max_element(milliseconds.begin(),milliseconds.end());
            qInfo()<<"Synthetic processing measurement only; not capture-to-display latency or physical FPS.";
        }
        qInfo()<<"PASS: native NVIDIA VFG 2x/4x, source colors, motion positions, cuts, repeat calls, teardown.";
        return 0;
    } catch(const std::exception& e) { qCritical()<<e.what(); return 1; }
}
}

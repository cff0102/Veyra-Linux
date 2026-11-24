#pragma once
#include "Vfg.h"
#include <QElapsedTimer>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QTimer>
#include <functional>

namespace veyra::linuxplayer {
class Preview final : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
public:
    explicit Preview(QWidget* parent=nullptr);
    ~Preview() override;
    void submit(QImage image, qint64 pts, qint64 deliveryAgeUs=0);
    void clearSource();
    void configure(QString runtime, int multiplier, int quality);
    void setSharpen(float amount) { sharpen_=amount; update(); }
    void setDeduplicate(bool enabled) { deduplicate_=enabled; }
    void setLatencyPriority(bool enabled) { latencyPriority_=enabled; clearSource(); }
    void setTimingEnabled(bool enabled) { timingEnabled_=enabled; }
    void setInterpolationBudget(qint64 microseconds) { interpolationBudgetUs_=microseconds; }
    QString takeTimingSummary();
    std::function<void(QString)> report;
    std::function<void()> failed;
    quint64 received=0, duplicates=0, generated=0, dropped=0, submitted=0;
protected:
    void initializeGL() override;
    void paintGL() override;
private:
    void cleanup();
    QTimer timer_;
    QElapsedTimer clock_;
    QOpenGLShaderProgram shader_;
    GLuint vao_=0, plain_=0;
    Vfg vfg_;
    QString runtime_;
    int multiplier_=1, quality_=0;
    bool dirty_=true, initialized_=false, deduplicate_=true;
    bool latencyPriority_=false, timingEnabled_=false, awaitingSwap_=false;
    float sharpen_=0;
    QImage previous_, pending_;
    qint64 previousPts_=-1, pendingPts_=-1, epoch_=0, duration_=33333;
    qint64 previousArrival_=0, pendingArrival_=0, batchFirstArrival_=0, batchLastArrival_=0;
    qint64 presentedContentArrival_=0, presentedLatestArrival_=0;
    qint64 interpolationBudgetUs_=0;
    std::vector<qint64> contentAges_, latestAges_;
    std::vector<GLuint> batch_;
    int displayed_=-1;
    QSize textureSize_;
    QSize modelSize_;
};
int gpuSelfTest(const QString& runtime, const QString& outputDirectory);
}

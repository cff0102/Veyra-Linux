#include "Preview.h"
#include "SharedFrames.h"
#include <QApplication>
#include <QAudioInput>
#include <QAudioDevice>
#include <QAudioOutput>
#include <QCamera>
#include <QCameraDevice>
#include <QCheckBox>
#include <QComboBox>
#include <QCommandLineParser>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScreenCapture>
#include <QWindowCapture>
#include <QShortcut>
#include <QSlider>
#include <QVideoFrame>
#include <QVideoSink>
#include <QVBoxLayout>
#include <QDebug>
#include <mutex>
#include <optional>
#include <cstdio>
#include <chrono>

using veyra::linuxplayer::Preview;

int main(int argc,char** argv) {
    qInstallMessageHandler([](QtMsgType type,const QMessageLogContext&,const QString& text) {
        if(type==QtDebugMsg) return;
        const auto bytes=text.toUtf8(); std::fprintf(stderr,"%s\n",bytes.constData());
    });
    QLoggingCategory::setFilterRules("*.debug=false\n*.info=false\ndefault.info=true\n*.warning=true\n*.critical=true");
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(3,3); format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSwapInterval(1); QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc,argv);
    app.setApplicationName("Veyra Linux Preview");
    app.setDesktopFileName("veyra-linux");
    QCommandLineParser args;
    args.setApplicationDescription("Experimental native Linux SDR capture/player with NVIDIA VFG.");
    args.addHelpOption();
    args.addOption({"runtime","Absolute directory containing Linux NVIDIA VFX .so libraries.","directory",qEnvironmentVariable("VEYRA_VFG_RUNTIME")});
    args.addOption({"gpu-self-test","Test real interpolation, color, cuts and CUDA/GL interop; no console connection."});
    args.addOption({"test-output","Self-test artifact directory.","directory"});
    args.addOption({"demo","Show a synthetic 30 fps motion source."});
    args.addOption({"multiplier","Frame-generation multiplier: 1 (off), 2 or 4.","number","1"});
    args.addOption({"capture-window","Capture the single window with this exact title (X11/XWayland).","title"});
    args.addOption({"direct-frames","Create a private local RGBA bridge for the patched Chiaki decoder.","path"});
    args.addOption({"capture-fps","Window capture rate limit (Qt 6.12+). Higher rates reduce sampling wait but cost more CPU.","fps","60"});
    args.addOption({"fullscreen","Start with picture-only fullscreen."});
    args.addOption({"latency-priority","Present newer frames sooner, skipping late intermediate frames."});
    args.addOption({"interpolation-budget-ms","Cap interpolation waiting in latency-priority mode; reduces smoothing (0 disables cap).","ms","0"});
    args.addOption({"timing-log","Log receipt-to-swap timing; excludes network, capture sampling and physical scanout."});
    args.addOption({"quit-after","Exit after this many seconds (for smoke tests).","seconds"});
    args.addOption({"screenshot","Save this application window before timed exit.","file"});
    args.addPositionalArgument("video","Optional local video file.","[video]");
    args.process(app);
    bool budgetValid=false;
    const int interpolationBudget=args.value("interpolation-budget-ms").toInt(&budgetValid);
    if(!budgetValid || interpolationBudget<0 || interpolationBudget>100 || (interpolationBudget && !args.isSet("latency-priority"))) {
        qCritical()<<"--interpolation-budget-ms must be 0..100 and requires --latency-priority";return 2;
    }
    if(args.isSet("latency-priority")) {
        // The desktop compositor still controls scanout; avoid adding a
        // separate application swap wait to the live capture path.
        format.setSwapInterval(0); QSurfaceFormat::setDefaultFormat(format);
    }
    if(args.isSet("gpu-self-test")) {
        if(args.value("test-output").isEmpty()) { qCritical()<<"--test-output is required"; return 2; }
        return veyra::linuxplayer::gpuSelfTest(args.value("runtime"),args.value("test-output"));
    }
    const int initialMultiplier=args.value("multiplier").toInt();
    if(initialMultiplier!=1 && initialMultiplier!=2 && initialMultiplier!=4) { qCritical()<<"Invalid multiplier"; return 2; }
    bool captureFpsValid=false;
    const int captureFps=args.value("capture-fps").toInt(&captureFpsValid);
    if(!captureFpsValid || captureFps<1 || captureFps>240) { qCritical()<<"--capture-fps must be 1..240"; return 2; }
#if QT_VERSION < QT_VERSION_CHECK(6, 12, 0)
    if(args.isSet("capture-fps")) { qCritical()<<"--capture-fps requires Qt 6.12 or newer"; return 2; }
#endif
    int quitSeconds=0;
    if(args.isSet("quit-after")) {
        bool valid=false; quitSeconds=args.value("quit-after").toInt(&valid);
        if(!valid || quitSeconds<1 || quitSeconds>300) {qCritical()<<"quit-after must be 1..300";return 2;}
    }
    QMainWindow window;
    window.setWindowTitle("Veyra • Linux Preview"); window.resize(1180,800);
    auto* content=new QWidget;
    auto* layout=new QVBoxLayout(content);
    auto* sourceRow=new QHBoxLayout;
    auto* cameraBox=new QComboBox;
    auto* formatBox=new QComboBox;
    auto* refresh=new QPushButton("Refresh devices");
    auto* capture=new QPushButton("Open capture card");
    auto* captureWindow=new QPushButton("Capture window");
    auto* screen=new QPushButton("Share screen");
    auto* file=new QPushButton("Open video");
    auto* stop=new QPushButton("Stop");
    sourceRow->addWidget(cameraBox,1); sourceRow->addWidget(formatBox,1);
    for(auto* button:{refresh,capture,captureWindow,screen,file,stop}) sourceRow->addWidget(button);
    layout->addLayout(sourceRow);
    auto* settings=new QHBoxLayout;
    auto* fg=new QComboBox; fg->addItem("Frame generation off",1); fg->addItem("NVIDIA VFG 2×",2); fg->addItem("NVIDIA VFG 4×",4);
    fg->setCurrentIndex(fg->findData(initialMultiplier));
    auto* quality=new QComboBox; quality->addItems({"Low model","Medium model","High model"});
    auto* duplicate=new QCheckBox("Skip identical frames"); duplicate->setChecked(true);
    auto* lowLatency=new QCheckBox("Latency priority"); lowLatency->setChecked(args.isSet("latency-priority"));
    lowLatency->setToolTip("Present newer frames sooner; late intermediate frames may be skipped. Does not remove interpolation latency.");
    auto* sharpen=new QSlider(Qt::Horizontal); sharpen->setRange(0,100); sharpen->setMaximumWidth(120);
    auto* audioBox=new QComboBox;
    auto* help=new QPushButton("PS5 setup");
    settings->addWidget(fg); settings->addWidget(quality); settings->addWidget(duplicate); settings->addWidget(lowLatency);
    settings->addWidget(new QLabel("Sharpen")); settings->addWidget(sharpen);
    settings->addWidget(new QLabel("Capture audio")); settings->addWidget(audioBox,1); settings->addWidget(help);
    layout->addLayout(settings);
    auto* runtime=new QLineEdit(args.value("runtime"));
    runtime->setPlaceholderText("NVIDIA Linux VFX runtime directory (only needed for frame generation)");
    layout->addWidget(runtime);
    auto* preview=new Preview;
    preview->setLatencyPriority(lowLatency->isChecked());
    preview->setTimingEnabled(args.isSet("timing-log"));
    preview->setInterpolationBudget(interpolationBudget*1000LL);
    layout->addWidget(preview,1);
    auto* message=new QLabel("Choose a capture card, share a Remote Play window, or open a video. SDR input only.");
    message->setWordWrap(true); layout->addWidget(message);
    auto* counters=new QLabel; layout->addWidget(counters);
    window.setCentralWidget(content);
    window.setStyleSheet("QMainWindow,QWidget {background:#111522;color:#e5eaf5;} QPushButton,QComboBox,QLineEdit {background:#252d43;padding:7px;border:1px solid #44516e;border-radius:4px;} QPushButton:hover {background:#35466b;} QLabel {padding:3px;}");
    QMediaDevices devices;
    QList<QCameraDevice> cameras;
    QList<QCameraFormat> formats;
    QList<QAudioDevice> audioDevices;
    QMediaCaptureSession session;
    QVideoSink sink;
    QMediaPlayer player;
    QAudioOutput playbackAudio;
    QAudioOutput captureAudio;
    QScreenCapture screenCapture;
    QWindowCapture windowCapture;
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
    windowCapture.setMaximumFrameRate(captureFps);
#endif
    std::unique_ptr<QCamera> camera;
    std::unique_ptr<QAudioInput> audioInput;
    QTimer demo;
    demo.setTimerType(Qt::PreciseTimer); demo.setInterval(33);
    QElapsedTimer sourceClock; sourceClock.start();
    veyra::bridge::Mapping directFrames;
    QTimer directTimer;
    directTimer.setTimerType(Qt::PreciseTimer);directTimer.setInterval(1);
    uint64_t directSequence=0;
    // Direct producer callbacks store exactly one frame and queue one GUI
    // drain; slow inference cannot accumulate per-frame Qt events.
    std::mutex mailboxMutex;
    std::optional<QVideoFrame> mailbox;
    qint64 mailboxArrival=0;
    auto monotonicUs=[] {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    quint64 mailboxDrops=0;
    bool ingestScheduled=false;
    auto ingestFrame=[&] {
        std::optional<QVideoFrame> frame;
        qint64 arrival;
        {
            std::lock_guard lock(mailboxMutex);
            frame=std::move(mailbox); mailbox.reset(); arrival=mailboxArrival;
            ingestScheduled=false;
        }
        if(!frame || !frame->isValid()) return;
        const auto transfer=frame->surfaceFormat().colorTransfer();
        if(transfer==QVideoFrameFormat::ColorTransfer_ST2084 || transfer==QVideoFrameFormat::ColorTransfer_STD_B67) {
            message->setText("HDR input is unsupported in this preview. Set PS5 / Remote Play to SDR."); return;
        }
        auto image=frame->toImage().convertToFormat(QImage::Format_RGBA8888);
        if(image.isNull()) { message->setText("Could not map the captured frame to RGBA."); return; }
        preview->submit(std::move(image),frame->startTime()>=0 ? frame->startTime() : sourceClock.nsecsElapsed()/1000,monotonicUs()-arrival);
    };
    auto connection=QObject::connect(&sink,&QVideoSink::videoFrameChanged,&sink,[&](const QVideoFrame& frame) {
        std::lock_guard lock(mailboxMutex);
        if(mailbox) ++mailboxDrops;
        mailbox=frame;
        mailboxArrival=monotonicUs();
        // Wake the GUI immediately, with at most one queued drain. New frames
        // replace the mailbox rather than adding per-frame GUI events.
        if(!ingestScheduled) {
            ingestScheduled=true;
            QMetaObject::invokeMethod(&sink,ingestFrame,Qt::QueuedConnection);
        }
    },Qt::DirectConnection);
    auto stopSource=[&] {
        directTimer.stop();directFrames.close();directSequence=0;
        demo.stop(); player.stop(); player.setVideoSink(nullptr);
        screenCapture.stop(); session.setScreenCapture(nullptr);
        windowCapture.stop(); session.setWindowCapture(nullptr);
        if(camera) camera->stop();
        session.setCamera(nullptr); camera.reset();
        session.setAudioInput(nullptr); audioInput.reset();
        session.setAudioOutput(nullptr); session.setVideoSink(nullptr);
        { std::lock_guard lock(mailboxMutex); mailbox.reset(); }
        preview->clearSource();
    };
    auto refreshFormats=[&] {
        formatBox->clear(); formats.clear();
        const int index=cameraBox->currentIndex();
        if(index<0 || index>=cameras.size()) return;
        formats=cameras[index].videoFormats();
        int selected=0;
        for(int i=0;i<formats.size();++i) {
            const auto& f=formats[i];
            formatBox->addItem(QString("%1×%2 • %3 fps • %4").arg(f.resolution().width()).arg(f.resolution().height())
              .arg(f.maxFrameRate(),0,'f',1).arg(QVideoFrameFormat::pixelFormatToString(f.pixelFormat())));
            if(f.resolution()==QSize(1920,1080) && f.maxFrameRate()>=59) selected=i;
        }
        formatBox->setCurrentIndex(selected);
    };
    auto refreshDevices=[&] {
        cameras=devices.videoInputs(); cameraBox->clear();
        for(const auto& c:cameras) cameraBox->addItem(c.description());
        capture->setEnabled(!cameras.isEmpty());
        if(cameras.isEmpty()) cameraBox->setPlaceholderText("No capture device detected");
        audioDevices=devices.audioInputs(); audioBox->clear(); audioBox->addItem("Off");
        for(const auto& a:audioDevices) audioBox->addItem(a.description());
        refreshFormats();
    };
    QObject::connect(cameraBox,&QComboBox::currentIndexChanged,[&]{refreshFormats();});
    QObject::connect(refresh,&QPushButton::clicked,[&]{refreshDevices();});
    refreshDevices();
    auto apply=[&] { preview->configure(runtime->text(),fg->currentData().toInt(),quality->currentIndex()); };
    QObject::connect(fg,&QComboBox::currentIndexChanged,[&]{apply();});
    QObject::connect(quality,&QComboBox::currentIndexChanged,[&]{apply();});
    QObject::connect(runtime,&QLineEdit::editingFinished,[&]{apply();});
    QObject::connect(duplicate,&QCheckBox::toggled,[&](bool value){preview->setDeduplicate(value);});
    QObject::connect(lowLatency,&QCheckBox::toggled,[&](bool value){preview->setLatencyPriority(value);});
    QObject::connect(sharpen,&QSlider::valueChanged,[&](int value){preview->setSharpen(value/100.f);});
    preview->report=[&](QString value){message->setText(value); qInfo().noquote()<<value;};
    preview->failed=[&]{QSignalBlocker block(fg);fg->setCurrentIndex(0);};
    apply();
    auto applyAudio=[&] {
        session.setAudioInput(nullptr); audioInput.reset(); session.setAudioOutput(nullptr);
        const int selected=audioBox->currentIndex()-1;
        if(camera && selected>=0 && selected<audioDevices.size()) {
            audioInput=std::make_unique<QAudioInput>(audioDevices[selected]);
            session.setAudioInput(audioInput.get()); session.setAudioOutput(&captureAudio);
        }
    };
    QObject::connect(audioBox,&QComboBox::currentIndexChanged,[&]{applyAudio();});
    QObject::connect(capture,&QPushButton::clicked,[&] {
        const int index=cameraBox->currentIndex(),mode=formatBox->currentIndex();
        if(index<0 || index>=cameras.size() || mode<0 || mode>=formats.size()) return;
        stopSource();
        camera=std::make_unique<QCamera>(cameras[index]); camera->setCameraFormat(formats[mode]);
        QObject::connect(camera.get(),&QCamera::errorOccurred,[&](QCamera::Error,const QString& text){message->setText(text);});
        session.setCamera(camera.get()); session.setVideoSink(&sink); applyAudio(); camera->start();
        message->setText("Capture started. Use the PS5 controller connected to the console.");
    });
    auto openWindow=[&](const QString& title) {
        if(QGuiApplication::platformName()!=QStringLiteral("xcb")) {
            message->setText("Direct window capture requires X11/XWayland. Launch Veyra with QT_QPA_PLATFORM=xcb; the source must also use X11/XWayland.");
            return;
        }
        QList<QCapturableWindow> windows;
        QStringList descriptions;
        for(const auto& candidate:QWindowCapture::capturableWindows()) {
            if(!candidate.isValid() || candidate.description()==window.windowTitle()) continue;
            if(!title.isEmpty() && candidate.description()!=title) continue;
            windows.append(candidate); descriptions.append(candidate.description());
        }
        if(windows.isEmpty() || (!title.isEmpty() && windows.size()!=1)) {
            message->setText("No unique matching window found. Keep the Remote Play window open on this desktop.");
            return;
        }
        int index=0;
        if(title.isEmpty()) {
            bool accepted=false;
            const auto selected=QInputDialog::getItem(&window,"Capture window","Window",descriptions,0,false,&accepted);
            if(!accepted) return;
            index=descriptions.indexOf(selected);
            if(index<0) return;
        }
        stopSource();
        session.setWindowCapture(&windowCapture); session.setVideoSink(&sink);
        windowCapture.setWindow(windows[index]);
        message->setText("Capturing "+descriptions[index]+". Keep audio and controller input in the Remote Play client.");
        windowCapture.start();
    };
    QObject::connect(captureWindow,&QPushButton::clicked,[&] { openWindow({}); });
    QObject::connect(&windowCapture,&QWindowCapture::errorOccurred,[&](QWindowCapture::Error,const QString& text){message->setText(text);qWarning()<<"Window capture:"<<text;});
    QObject::connect(screen,&QPushButton::clicked,[&] {
        stopSource(); session.setScreenCapture(&screenCapture); session.setVideoSink(&sink);
        screenCapture.start();
        message->setText("Select a screen in the sharing dialog. Do not display Veyra on the captured screen: that creates a feedback loop. Use Capture window for Remote Play.");
    });
    QObject::connect(&screenCapture,&QScreenCapture::errorOccurred,[&](QScreenCapture::Error,const QString& text){message->setText(text);});
    auto openFile=[&](const QString& path) {
        if(path.isEmpty()) return;
        stopSource(); player.setVideoSink(&sink); player.setAudioOutput(&playbackAudio);
        player.setSource(QUrl::fromLocalFile(path)); player.play(); message->setText("Playing "+path);
    };
    QObject::connect(file,&QPushButton::clicked,[&]{openFile(QFileDialog::getOpenFileName(&window,"Open video"));});
    QObject::connect(stop,&QPushButton::clicked,[&]{stopSource();message->setText("Stopped");});
    QObject::connect(&player,&QMediaPlayer::errorOccurred,[&](QMediaPlayer::Error,const QString& text){message->setText(text);});
    QObject::connect(help,&QPushButton::clicked,[&] {
        QMessageBox::information(&window,"PS5 on Linux",
          "Remote Play: enable Settings → System → Remote Play on PS5. Pair a Linux Remote Play client such as chiaki-ng. Run both clients with X11/XWayland, then use Capture window to select the Remote Play window. Keep controller input and audio in that client. Share screen captures an entire display and must not include Veyra itself.\n\n"
          "Capture card: PS5 HDMI OUT → card HDMI IN; card USB → PC. Disable PS5 Settings → System → HDMI → Enable HDCP. Select a Linux-compatible capture card and its supported format here, then Open capture card. Use SDR initially. The PC's display output shows the enhanced picture; card passthrough remains original.\n\n"
          "This experimental viewer supports NVIDIA video frame generation and sharpening. It does not yet port Veyra's neural rendering, HDR, node editor, or built-in PS5 pairing. Generated frames add latency. Audio is monitored without automatic video-delay compensation.");
    });
    auto* fullscreen=new QShortcut(QKeySequence(Qt::Key_F11),&window);
    auto setFullscreen=[&](bool enabled) {
        for(auto* row:{sourceRow,settings}) for(int i=0;i<row->count();++i)
            if(auto* widget=row->itemAt(i)->widget()) widget->setVisible(!enabled);
        runtime->setVisible(!enabled); message->setVisible(!enabled); counters->setVisible(!enabled);
        layout->setContentsMargins(enabled?0:9,enabled?0:9,enabled?0:9,enabled?0:9);
        enabled?window.showFullScreen():window.showNormal();
    };
    QObject::connect(fullscreen,&QShortcut::activated,[&]{setFullscreen(!window.isFullScreen());});
    auto* escape=new QShortcut(QKeySequence(Qt::Key_Escape),&window);
    QObject::connect(escape,&QShortcut::activated,[&]{setFullscreen(false);});
    QTimer statistics; statistics.setInterval(1000);
    QObject::connect(&statistics,&QTimer::timeout,[&] {
        quint64 overwritten; {std::lock_guard lock(mailboxMutex); overwritten=mailboxDrops;}
        counters->setText(QString("Frames received: %1 • duplicates skipped: %2 • generated: %3 • submitted: %4 • dropped: %5  |  Submitted counts are not physical display FPS. F11: fullscreen")
          .arg(preview->received).arg(preview->duplicates).arg(preview->generated).arg(preview->submitted).arg(preview->dropped+overwritten));
        const auto timing=preview->takeTimingSummary();
        if(!timing.isEmpty()) qInfo().noquote()<<timing;
    }); statistics.start();
    int demoIndex=0;
    QObject::connect(&demo,&QTimer::timeout,[&] {
        QImage image(1280,720,QImage::Format_RGBA8888); image.fill(QColor(20,28,45));
        QPainter p(&image); p.setPen(QColor(50,70,100));
        for(int x=0;x<1280;x+=80) p.drawLine(x,0,x,720);
        p.fillRect(80+(demoIndex*12)%1000,260,120,120,QColor(235,130,65));
        p.setPen(Qt::white); p.setFont(QFont("sans-serif",24));
        p.drawText(45,75,"Veyra Linux • 30 fps test source"); p.end();
        preview->submit(std::move(image),qint64(demoIndex++)*33333);
    });
    window.show();
    if(args.isSet("direct-frames")) {
        if(!directFrames.open(args.value("direct-frames").toStdString(),true)) {
            qCritical()<<"Cannot create private frame bridge; choose a new path in your runtime directory.";
            return 2;
        }
        message->setText("Waiting for direct decoded PS5 frames. Audio and controller input remain in Chiaki.");
        QObject::connect(&directTimer,&QTimer::timeout,[&] {
            QImage image;qint64 pts=0,receipt=0;
            {
                veyra::bridge::Lock lock(directFrames);
                if(!lock || !directFrames.compatible()) return;
                const auto& h=directFrames.header();
                if(!h.sequence || h.sequence==directSequence) return;
                if(h.width<16 || h.width>3840 || h.height<16 || h.height>2160 ||
                   h.stride!=h.width*4 || h.bytes!=h.stride*h.height || h.bytes>veyra::bridge::PixelBytes) return;
                image=QImage(directFrames.pixels(),h.width,h.height,h.stride,QImage::Format_RGBA8888).copy();
                pts=h.ptsUs;receipt=h.receivedUs;directSequence=h.sequence;
            }
            preview->submit(std::move(image),pts,monotonicUs()-receipt);
        });
        directTimer.start();
    }
    else if(args.isSet("demo")) demo.start();
    else if(args.isSet("capture-window")) openWindow(args.value("capture-window"));
    else if(!args.positionalArguments().isEmpty()) openFile(args.positionalArguments().first());
    if(args.isSet("fullscreen")) setFullscreen(true);
    if(quitSeconds) {
        QTimer::singleShot(quitSeconds*1000,&app,[&] {
            if(args.isSet("screenshot")) window.grab().save(args.value("screenshot"));
            qInfo()<<"Received"<<preview->received<<"generated"<<preview->generated<<"submitted"<<preview->submitted<<"dropped"<<preview->dropped;
            app.quit();
        });
    }
    const int result=app.exec();
    stopSource(); QObject::disconnect(connection);
    // Destroy the GL widget while callbacks and its supporting state still live.
    delete window.takeCentralWidget();
    return result;
}

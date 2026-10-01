#include "siderastack/core.hpp"
#include <QProcess>
#include <QThread>
#include <QtWidgets>
#include <algorithm>
#include <functional>
#include <memory>
#include <signal.h>
#include <sys/types.h>

namespace {
QString q(const std::string &s) {
    return QString::fromStdString(s);
}
QString value(double v) {
    return std::isfinite(v) ? QString::number(v, 'g', 5) : QString::fromUtf8("—");
}
const QStringList columns{"Use",        "ID",           "File",         "Type", "Filter",       "Night",
                          "Master",     "Bias removed", "FWHM",         "HFR",  "Eccentricity", "Stars",
                          "Background", "Noise",        "Transparency", "RMS",  "Status"};
class FrameModel : public QAbstractTableModel {
  public:
    std::vector<ss::Frame> frames;
    ss::Project *project = nullptr;
    bool busy = false;
    std::function<void()> changed;
    int rowCount(const QModelIndex &p = {}) const override {
        return p.isValid() ? 0 : int(frames.size());
    }
    int columnCount(const QModelIndex &p = {}) const override {
        return p.isValid() ? 0 : columns.size();
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (role != Qt::DisplayRole)
            return {};
        return orientation == Qt::Horizontal ? QVariant(columns[section]) : QVariant(section + 1);
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid())
            return {};
        const auto &f = frames[size_t(index.row())];
        int c = index.column();
        if (role == Qt::CheckStateRole) {
            if (c == 0)
                return f.selection < 0 ? Qt::Unchecked : Qt::Checked;
            if (c == 6)
                return f.master ? Qt::Checked : Qt::Unchecked;
            if (c == 7)
                return f.biasSubtracted ? Qt::Checked : Qt::Unchecked;
        }
        if (role == Qt::ToolTipRole)
            return q(f.error.empty() ? f.path.string() : f.error);
        if (role == Qt::ForegroundRole) {
            if (!f.error.empty())
                return QColor("#b93838");
            if (f.selection < 0)
                return QColor("#888888");
        }
        if (role != Qt::DisplayRole && role != Qt::EditRole)
            return {};
        switch (c) {
        case 1:
            return qlonglong(f.id);
        case 2:
            return q(f.path.filename().string());
        case 3:
            return q(f.kind);
        case 4:
            return q(f.filter);
        case 5:
            return q(f.session);
        case 8:
            return std::isfinite(f.metrics.fwhm) ? QVariant(f.metrics.fwhm) : QVariant("—");
        case 9:
            return std::isfinite(f.metrics.hfr) ? QVariant(f.metrics.hfr) : QVariant("—");
        case 10:
            return std::isfinite(f.metrics.eccentricity) ? QVariant(f.metrics.eccentricity) : QVariant("—");
        case 11:
            return f.metrics.stars;
        case 12:
            return std::isfinite(f.metrics.background) ? QVariant(f.metrics.background) : QVariant("—");
        case 13:
            return std::isfinite(f.metrics.noise) ? QVariant(f.metrics.noise) : QVariant("—");
        case 14:
            return std::isfinite(f.metrics.transparency) ? QVariant(f.metrics.transparency) : QVariant("—");
        case 15:
            return std::isfinite(f.metrics.residual) ? QVariant(f.metrics.residual) : QVariant("—");
        case 16:
            return !f.error.empty()    ? q(f.error)
                   : f.transform.valid ? "Ready"
                   : f.stars.empty()   ? "Not analyzed"
                                       : "Measured";
        default:
            return {};
        }
    }
    Qt::ItemFlags flags(const QModelIndex &index) const override {
        auto flags = QAbstractTableModel::flags(index);
        if (!busy) {
            int c = index.column();
            if (c == 0 || c == 6 || c == 7)
                flags |= Qt::ItemIsUserCheckable;
            if (c >= 3 && c <= 5)
                flags |= Qt::ItemIsEditable;
        }
        return flags;
    }
    bool setData(const QModelIndex &index, const QVariant &v, int role) override {
        if (busy || !project || !index.isValid())
            return false;
        auto f = frames[size_t(index.row())];
        int c = index.column();
        if (role == Qt::CheckStateRole) {
            if (c == 0)
                f.selection = v.toInt() == Qt::Checked ? 1 : -1;
            else if (c == 6)
                f.master = v.toInt() == Qt::Checked;
            else if (c == 7)
                f.biasSubtracted = v.toInt() == Qt::Checked;
            else
                return false;
        } else if (role == Qt::EditRole) {
            if (c == 3) {
                if (!QStringList{"unknown", "light", "dark", "flat", "bias", "darkflat"}.contains(
                        v.toString()))
                    return false;
                f.kind = v.toString().toStdString();
            } else if (c == 4)
                f.filter = v.toString().toStdString();
            else if (c == 5)
                f.session = v.toString().toStdString();
            else
                return false;
        } else
            return false;
        try {
            project->save(f);
            frames[size_t(index.row())] = std::move(f);
            Q_EMIT dataChanged(index, index);
            if (changed)
                changed();
            return true;
        } catch (const std::exception &e) {
            QMessageBox::critical(nullptr, "Project error", e.what());
            return false;
        }
    }
    void reload() {
        beginResetModel();
        frames = project ? project->frames() : std::vector<ss::Frame>{};
        endResetModel();
    }
};
class ImageView : public QGraphicsView {
  public:
    QGraphicsScene scene;
    QGraphicsPixmapItem *pixels = nullptr;
    std::vector<QGraphicsEllipseItem *> markers;
    bool starsVisible = true;
    ImageView() {
        setScene(&scene);
        setBackgroundBrush(QColor("#151920"));
        setDragMode(ScrollHandDrag);
        setTransformationAnchor(AnchorUnderMouse);
        setMinimumSize(320, 240);
    }
    void display(const QImage &image, const std::vector<ss::Star> &stars) {
        scene.clear();
        markers.clear();
        pixels = scene.addPixmap(QPixmap::fromImage(image));
        scene.setSceneRect(pixels->boundingRect());
        QPen pen(QColor("#48e5b0"));
        pen.setCosmetic(true);
        for (const auto &s : stars) {
            auto *item = scene.addEllipse(s.x - 6, s.y - 6, 12, 12, pen);
            item->setVisible(starsVisible);
            markers.push_back(item);
        }
        fitInView(scene.sceneRect(), Qt::KeepAspectRatio);
    }
    void wheelEvent(QWheelEvent *event) override {
        double f = event->angleDelta().y() > 0 ? 1.2 : 1 / 1.2;
        scale(f, f);
        event->accept();
    }
    void toggleStars(bool enabled) {
        starsVisible = enabled;
        for (auto *m : markers)
            m->setVisible(enabled);
    }
};
class MetricPlot : public QWidget {
  public:
    FrameModel *model;
    QString metric = "FWHM";
    std::function<void(int64_t)> select;
    std::vector<std::pair<QPointF, int64_t>> points;
    explicit MetricPlot(FrameModel *m) : model(m) {
        setMinimumHeight(150);
        setToolTip("Frame order on the horizontal axis; click a point to inspect its exposure.");
    }
    double measurement(const ss::Frame &f) const {
        if (metric == "HFR")
            return f.metrics.hfr;
        if (metric == "Eccentricity")
            return f.metrics.eccentricity;
        if (metric == "Stars")
            return f.metrics.stars;
        if (metric == "Background")
            return f.metrics.background;
        if (metric == "Noise")
            return f.metrics.noise;
        if (metric == "Transparency")
            return f.metrics.transparency;
        return f.metrics.fwhm;
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), palette().base());
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(palette().text().color());
        p.drawText(10, 20, metric + " · frame order");
        points.clear();
        double lo = INFINITY, hi = -INFINITY;
        for (const auto &f : model->frames)
            if (f.kind == "light" && std::isfinite(measurement(f))) {
                lo = std::min(lo, measurement(f));
                hi = std::max(hi, measurement(f));
            }
        if (!std::isfinite(lo)) {
            p.drawText(rect(), Qt::AlignCenter, "Analyze frames to compare quality");
            return;
        }
        if (hi == lo)
            hi = lo + 1;
        p.drawText(10, 42, value(hi));
        p.drawText(10, height() - 10, value(lo));
        size_t index = 0;
        for (const auto &f : model->frames) {
            double v = measurement(f);
            double x = 65 + double(index++) / std::max<size_t>(1, model->frames.size() - 1) * (width() - 85);
            if (f.kind != "light" || !std::isfinite(v))
                continue;
            double y = height() - 20 - (v - lo) / (hi - lo) * (height() - 65);
            p.setPen(Qt::NoPen);
            p.setBrush(f.selection < 0 ? QColor("#999999") : QColor("#258878"));
            p.drawEllipse(QPointF(x, y), 3, 3);
            points.emplace_back(QPointF(x, y), f.id);
        }
    }
    void mousePressEvent(QMouseEvent *event) override {
        double distance = 100;
        int64_t id = 0;
        for (auto [point, frame] : points) {
            double d = QLineF(point, event->position()).length();
            if (d < distance) {
                distance = d;
                id = frame;
            }
        }
        if (id && select)
            select(id);
    }
};
QImage preview(const ss::Image &im, double &black, double &white, bool common) {
    if (!common || !std::isfinite(black) || !std::isfinite(white)) {
        std::vector<float> sample;
        size_t stride = std::max<size_t>(1, im.samples() / 200000);
        for (size_t i = 0; i < im.samples(); i += stride)
            if (std::isfinite(im.pixels[i]))
                sample.push_back(im.pixels[i]);
        if (sample.empty())
            throw ss::Error("Image has no finite pixels");
        std::sort(sample.begin(), sample.end());
        black = sample[sample.size() / 100];
        white = sample[std::min(sample.size() - 1, sample.size() * 999 / 1000)];
        if (white <= black)
            white = black + 1;
    }
    QImage result(im.width, im.height, QImage::Format_RGB32);
    if (result.isNull())
        throw ss::Error("Preview allocation failed");
    for (int y = 0; y < im.height; ++y) {
        auto *row = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < im.width; ++x) {
            size_t i = size_t(y) * im.width + x;
            int rgb[3];
            for (int c = 0; c < 3; ++c) {
                double v = im.pixels[i + (im.channels == 3 ? size_t(c) * im.plane() : 0)];
                double t = std::isfinite(v) ? std::clamp((v - black) / (white - black), 0.0, 1.0) : 0;
                rgb[c] = int(255 * std::asinh(12 * t) / std::asinh(12.0));
            }
            row[x] = qRgb(rgb[0], rgb[1], rgb[2]);
        }
    }
    return result;
}
class Window : public QMainWindow {
  public:
    std::unique_ptr<ss::Project> project;
    FrameModel model;
    QSortFilterProxyModel proxy;
    QTableView table;
    ImageView view;
    MetricPlot plot{&model};
    QProcess worker;
    QByteArray output;
    QProgressBar progress;
    QLabel status, previewStatus;
    QPlainTextEdit log;
    QComboBox filter, metric;
    QTimer blink;
    QThread *previewThread = nullptr;
    int64_t pendingPreview = 0;
    uint64_t previewGeneration = 0;
    double black = NAN, white = NAN;
    bool commonStretch = true;
    QString lastOutput;
    QList<QAction *> jobActions;
    Window() {
        setWindowTitle("SideraStack");
        resize(1450, 900);
        setMinimumSize(900, 600);
        auto *toolbar = addToolBar("Workflow");
        toolbar->setMovable(false);
        toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        auto action = [&](QString title, auto fn, bool requiresProject = false) {
            auto *a = toolbar->addAction(title);
            connect(a, &QAction::triggered, this, fn);
            if (requiresProject) {
                a->setEnabled(false);
                jobActions << a;
            }
            return a;
        };
        action("New project", [this] {
            auto path =
                QFileDialog::getSaveFileName(this, "New SideraStack project", {}, "SideraStack (*.sidera)");
            if (path.isEmpty())
                return;
            if (!path.endsWith(".sidera"))
                path += ".sidera";
            try {
                if (ss::fs::exists(path.toStdString()))
                    throw ss::Error("Choose a new filename");
                {
                    ss::Project p(path.toStdString(), true);
                }
                open(path);
            } catch (const std::exception &e) {
                error(e.what());
            }
        });
        action("Open", [this] {
            auto p = QFileDialog::getOpenFileName(this, "Open project", {}, "SideraStack (*.sidera)");
            if (!p.isEmpty())
                open(p);
        });
        toolbar->addSeparator();
        action(
            "Import files",
            [this] {
                auto paths = QFileDialog::getOpenFileNames(
                    this, "Import exposures", {},
                    "Astro images (*.fits *.fit *.fts *.fz *.xisf);;All files (*)");
                if (!paths.isEmpty())
                    start("import", paths);
            },
            true);
        action(
            "Import folder",
            [this] {
                auto p = QFileDialog::getExistingDirectory(this, "Import exposures recursively");
                if (!p.isEmpty())
                    start("import", {p});
            },
            true);
        action("Calibration", [this] { calibration(); }, true);
        action(
            "Create masters",
            [this] {
                auto directory =
                    QFileDialog::getExistingDirectory(this, "Create calibration masters in a new directory");
                if (directory.isEmpty())
                    return;
                bool ok = false;
                auto choice = QInputDialog::getItem(this, "Flat calibration", "Calibrate flats using",
                                                    {"Bias", "Dark-flat", "Automatic (dark-flat preferred)",
                                                     "None (explicitly uncalibrated)"},
                                                    0, false, &ok);
                if (!ok)
                    return;
                QString policy = choice == "Bias"                 ? "bias"
                                 : choice == "Dark-flat"          ? "darkflat"
                                 : choice.startsWith("Automatic") ? "auto"
                                                                  : "none";
                QStringList ids;
                for (auto row : selectedRows()) {
                    const auto &f = model.frames[row];
                    if (f.kind != "light" && f.kind != "unknown" && f.selection >= 0)
                        ids << QString::number(f.id);
                }
                QStringList formats{"fits", "fits.fz", "xisf", "xisf-zstd"};
                QStringList arguments{directory, "--flat-calibration", policy, "--format",
                                      formats[int(project->settings().format)]};
                if (!ids.empty())
                    arguments << "--ids" << ids.join(',');
                start("masters", arguments);
            },
            true);
        action("Analyze", [this] { start("analyze"); }, true);
        action("Settings & grading", [this] { settings(); }, true);
        action(
            "Stack",
            [this] {
                auto p = QFileDialog::getExistingDirectory(this, "Choose an empty output directory");
                if (!p.isEmpty()) {
                    lastOutput = p;
                    project->record("output", {{"directory", p}});
                    start("stack", {p});
                }
            },
            true);
        action(
            "Resume",
            [this] {
                auto p = lastOutput;
                if (p.isEmpty())
                    p = QFileDialog::getExistingDirectory(this, "Resume into the previous output directory");
                if (!p.isEmpty())
                    start("resume", {p});
            },
            true);
        action(
            "Export",
            [this] {
                auto p = QFileDialog::getExistingDirectory(this, "Export completed masters");
                if (!p.isEmpty()) {
                    QStringList formats{"fits", "fits.fz", "xisf", "xisf-zstd"};
                    bool ok = false;
                    auto f = QInputDialog::getItem(this, "Export format", "Encoding", formats, 0, false, &ok);
                    if (ok)
                        start("export", {p, "--format", f});
                }
            },
            true);
        auto *cancel = action("Cancel", [this] {
            if (worker.state() != QProcess::NotRunning)
                ::kill(pid_t(worker.processId()), SIGINT);
        });
        cancel->setToolTip("Stop safely at the next processing checkpoint");
        auto *root = new QWidget;
        auto *layout = new QVBoxLayout(root);
        auto *intro = new QLabel(
            "Import → Analyze → Review → Stack → Export   ·   Linear masters for your image editor");
        intro->setMargin(8);
        layout->addWidget(intro);
        auto *controls = new QHBoxLayout;
        controls->addWidget(new QLabel("Show"));
        filter.addItem("All frames");
        controls->addWidget(&filter);
        metric.addItems({"FWHM", "HFR", "Eccentricity", "Stars", "Background", "Noise", "Transparency"});
        controls->addWidget(new QLabel("Plot"));
        controls->addWidget(&metric);
        auto *exclude = new QPushButton("Exclude selected");
        auto *include = new QPushButton("Include selected");
        auto *edit = new QPushButton("Edit selected");
        auto *blinkButton = new QPushButton("Blink selection");
        blinkButton->setCheckable(true);
        auto *stars = new QCheckBox("Star overlay");
        stars->setChecked(true);
        auto *common = new QCheckBox("Common stretch");
        common->setChecked(true);
        controls->addWidget(exclude);
        controls->addWidget(include);
        controls->addWidget(edit);
        controls->addStretch();
        controls->addWidget(stars);
        controls->addWidget(common);
        controls->addWidget(blinkButton);
        layout->addLayout(controls);
        proxy.setSourceModel(&model);
        proxy.setFilterKeyColumn(-1);
        proxy.setSortRole(Qt::DisplayRole);
        table.setModel(&proxy);
        table.setSelectionBehavior(QAbstractItemView::SelectRows);
        table.setSelectionMode(QAbstractItemView::ExtendedSelection);
        table.setSortingEnabled(true);
        table.setAlternatingRowColors(true);
        table.setWordWrap(false);
        table.setColumnWidth(2, 210);
        table.setColumnWidth(16, 220);
        auto *split = new QSplitter;
        split->addWidget(&table);
        auto *right = new QWidget;
        auto *rightLayout = new QVBoxLayout(right);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        rightLayout->addWidget(&previewStatus);
        rightLayout->addWidget(&view);
        rightLayout->addWidget(&plot);
        split->addWidget(right);
        split->setStretchFactor(0, 3);
        split->setStretchFactor(1, 2);
        layout->addWidget(split, 1);
        log.setReadOnly(true);
        log.setMaximumBlockCount(500);
        log.setMaximumHeight(100);
        layout->addWidget(&log);
        auto *bottom = new QHBoxLayout;
        bottom->addWidget(&status, 1);
        progress.setRange(0, 1000);
        progress.setMaximumWidth(350);
        bottom->addWidget(&progress);
        layout->addLayout(bottom);
        setCentralWidget(root);
        status.setText("Create or open a project to begin");
        model.changed = [this] {
            plot.update();
            summary();
        };
        plot.select = [this](int64_t id) {
            for (int i = 0; i < model.rowCount(); ++i)
                if (model.frames[size_t(i)].id == id) {
                    auto index = proxy.mapFromSource(model.index(i, 0));
                    table.selectRow(index.row());
                    table.scrollTo(index);
                    break;
                }
        };
        connect(&metric, &QComboBox::currentTextChanged, this, [this](const QString &s) {
            plot.metric = s;
            plot.update();
        });
        connect(&filter, &QComboBox::currentTextChanged, this,
                [this](const QString &s) { proxy.setFilterFixedString(s == "All frames" ? QString() : s); });
        connect(exclude, &QPushButton::clicked, this, [this] { select(-1); });
        connect(include, &QPushButton::clicked, this, [this] { select(1); });
        connect(edit, &QPushButton::clicked, this, [this] { editSelected(); });
        connect(stars, &QCheckBox::toggled, &view, &ImageView::toggleStars);
        connect(common, &QCheckBox::toggled, this, [this](bool on) {
            commonStretch = on;
            lastOutput = q(ss::str(project->record("output"), "directory"));
            ++previewGeneration;
            pendingPreview = 0;
            black = white = NAN;
            showCurrent();
        });
        connect(table.selectionModel(), &QItemSelectionModel::currentRowChanged, this,
                [this] { showCurrent(); });
        connect(blinkButton, &QPushButton::toggled, this, [this](bool on) {
            if (on)
                blink.start(800);
            else
                blink.stop();
        });
        connect(&blink, &QTimer::timeout, this, [this] {
            auto rows = table.selectionModel()->selectedRows();
            if (rows.size() < 2)
                return;
            int next = 0;
            for (int i = 0; i < rows.size(); ++i)
                if (rows[i].row() == table.currentIndex().row())
                    next = (i + 1) % rows.size();
            table.selectionModel()->setCurrentIndex(rows[next], QItemSelectionModel::NoUpdate);
        });
        connect(&worker, &QProcess::readyReadStandardOutput, this, [this] {
            output += worker.readAllStandardOutput();
            for (;;) {
                auto pos = output.indexOf('\n');
                if (pos < 0)
                    break;
                auto line = output.left(pos);
                output.remove(0, pos + 1);
                try {
                    auto o = ss::parseJson(line);
                    if (ss::str(o, "event") == "error") {
                        log.appendPlainText(o["message"].toString());
                        status.setText(o["message"].toString());
                    } else if (ss::str(o, "event") == "progress") {
                        double total = ss::number(o, "total", 0), done = ss::number(o, "done", 0);
                        progress.setValue(total > 0 ? int(1000 * done / total) : 0);
                        status.setText(o["stage"].toString() + " · " + o["message"].toString());
                    }
                } catch (...) {
                    log.appendPlainText(QString::fromUtf8(line));
                }
            }
        });
        connect(&worker, &QProcess::readyReadStandardError, this,
                [this] { log.appendPlainText(QString::fromUtf8(worker.readAllStandardError())); });
        connect(&worker, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus exit) {
                    setBusy(false);
                    reload();
                    if (code == 0 && exit == QProcess::NormalExit) {
                        progress.setValue(1000);
                        status.setText("Complete · " + status.text());
                    } else {
                        status.setText("Processing stopped — inspect the log and frame status");
                        log.appendPlainText("Worker exit code " + QString::number(code) +
                                            ". Completed calibration and analysis results remain available.");
                    }
                });
        connect(&worker, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
            setBusy(false);
            error(worker.errorString());
        });
    }
    ~Window() override {
        if (previewThread) {
            previewThread->requestInterruption();
            previewThread->wait();
        }
        if (worker.state() != QProcess::NotRunning) {
            ::kill(pid_t(worker.processId()), SIGINT);
            worker.waitForFinished(3000);
            if (worker.state() != QProcess::NotRunning) {
                worker.kill();
                worker.waitForFinished();
            }
        }
    }
    void error(const QString &message) {
        QMessageBox::critical(this, "SideraStack", message);
    }
    void setBusy(bool busy) {
        model.busy = busy;
        for (auto *a : jobActions)
            a->setEnabled(project && !busy);
    }
    void open(const QString &path) {
        if (worker.state() != QProcess::NotRunning)
            return;
        try {
            project = std::make_unique<ss::Project>(path.toStdString());
            model.project = project.get();
            setWindowTitle("SideraStack · " + QFileInfo(path).fileName());
            lastOutput = q(ss::str(project->record("output"), "directory"));
            ++previewGeneration;
            pendingPreview = 0;
            black = white = NAN;
            setBusy(false);
            reload();
        } catch (const std::exception &e) {
            error(e.what());
        }
    }
    void reload() {
        try {
            model.reload();
            QSignalBlocker blocker(filter);
            auto current = filter.currentText();
            filter.clear();
            filter.addItem("All frames");
            QSet<QString> names;
            for (const auto &f : model.frames)
                names.insert(q(f.filter));
            for (const auto &s : names)
                if (!s.isEmpty())
                    filter.addItem(s);
            filter.setCurrentText(current);
            plot.update();
            summary();
        } catch (const std::exception &e) {
            error(e.what());
        }
    }
    void summary() {
        int light = 0, excluded = 0, ready = 0;
        for (auto &f : model.frames) {
            light += f.kind == "light";
            excluded += f.selection < 0;
            ready += f.transform.valid;
        }
        status.setText(QString("%1 frames · %2 lights · %3 excluded · %4 registered")
                           .arg(model.frames.size())
                           .arg(light)
                           .arg(excluded)
                           .arg(ready));
    }
    void start(const QString &command, QStringList extra = {}) {
        if (!project || worker.state() != QProcess::NotRunning)
            return;
        auto binary = QCoreApplication::applicationDirPath() + "/siderastack-cli";
        if (!QFileInfo::exists(binary)) {
            error("siderastack-cli must be installed beside siderastack");
            return;
        }
        output.clear();
        setBusy(true);
        progress.setValue(0);
        log.appendPlainText(command + " started");
        QStringList args{command, q(project->path().string()), "--json"};
        args.append(extra);
        worker.start(binary, args);
    }
    std::vector<size_t> selectedRows() {
        std::vector<size_t> rows;
        for (auto index : table.selectionModel()->selectedRows())
            rows.push_back(size_t(proxy.mapToSource(index).row()));
        return rows;
    }
    void select(int selection) {
        if (!project || model.busy)
            return;
        try {
            project->transaction([&] {
                for (auto row : selectedRows()) {
                    auto &f = model.frames[row];
                    f.selection = selection;
                    project->save(f);
                }
            });
            model.dataChanged(model.index(0, 0), model.index(model.rowCount() - 1, 16));
            plot.update();
            summary();
        } catch (const std::exception &e) {
            error(e.what());
        }
    }
    void editSelected() {
        if (!project || model.busy || selectedRows().empty())
            return;
        QDialog dialog(this);
        dialog.setWindowTitle("Edit selected frames");
        QFormLayout form(&dialog);
        QComboBox kind;
        kind.addItems({"Keep current type", "light", "dark", "flat", "bias", "darkflat", "unknown"});
        QLineEdit band, night, camera, exposure, temperature, gain, offset, xbin, ybin, sampleScale;
        QComboBox cfa, rowOrder;
        cfa.addItems({"Keep current", "Mono / RGB", "RGGB", "BGGR", "GRBG", "GBRG"});
        rowOrder.addItems({"Keep current", "TOP-DOWN", "BOTTOM-UP"});
        form.addRow("Frame type", &kind);
        form.addRow("Filter (blank keeps current)", &band);
        form.addRow("Night (blank keeps current)", &night);
        form.addRow("Camera (blank keeps current)", &camera);
        form.addRow("Exposure seconds (blank keeps current)", &exposure);
        form.addRow("Temperature °C (blank keeps current)", &temperature);
        form.addRow("Gain (blank keeps current)", &gain);
        form.addRow("Offset (blank keeps current)", &offset);
        form.addRow("Horizontal binning (blank keeps current)", &xbin);
        form.addRow("Vertical binning (blank keeps current)", &ybin);
        form.addRow("Bayer pattern", &cfa);
        form.addRow("Stored row orientation", &rowOrder);
        form.addRow("Scale to relative units (blank uses metadata)", &sampleScale);
        QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form.addRow(&buttons);
        connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted)
            return;
        try {
            project->transaction([&] {
                for (auto row : selectedRows()) {
                    auto &f = model.frames[row];
                    QJsonObject changes, header;
                    if (kind.currentIndex())
                        changes["kind"] = kind.currentText();
                    if (!band.text().isEmpty())
                        changes["filter"] = band.text();
                    if (!night.text().isEmpty())
                        changes["session"] = night.text();
                    if (!camera.text().isEmpty())
                        header["INSTRUME"] = camera.text();
                    const std::array<std::pair<QLineEdit *, const char *>, 7> numbers{
                        {{&exposure, "EXPTIME"},
                         {&temperature, "CCD-TEMP"},
                         {&gain, "GAIN"},
                         {&offset, "OFFSET"},
                         {&xbin, "XBINNING"},
                         {&ybin, "YBINNING"},
                         {&sampleScale, "SS_SAMPLE_SCALE"}}};
                    for (const auto &[field, key] : numbers)
                        if (!field->text().isEmpty()) {
                            bool ok = false;
                            auto n = field->text().toDouble(&ok);
                            if (!ok || !std::isfinite(n))
                                throw ss::Error("Enter valid numeric acquisition metadata");
                            header[key] = n;
                        }
                    if (cfa.currentIndex()) {
                        header["BAYERPAT"] =
                            cfa.currentIndex() == 1 ? QJsonValue() : QJsonValue(cfa.currentText());
                        header["XBAYROFF"] = header["YBAYROFF"] = 0;
                    }
                    if (rowOrder.currentIndex())
                        header["ROWORDER"] = rowOrder.currentText();
                    changes["header"] = header;
                    ss::editFrame(f, changes);
                    project->save(f);
                }
            });
            reload();
        } catch (const std::exception &e) {
            error(e.what());
        }
    }
    void showCurrent() {
        if (!project)
            return;
        auto index = proxy.mapToSource(table.currentIndex());
        if (!index.isValid())
            return;
        ++previewGeneration;
        pendingPreview = model.frames[size_t(index.row())].id;
        if (!previewThread)
            loadPreview();
    }
    void loadPreview() {
        if (!pendingPreview)
            return;
        auto it = std::find_if(model.frames.begin(), model.frames.end(),
                               [this](const ss::Frame &f) { return f.id == pendingPreview; });
        if (it == model.frames.end())
            return;
        auto frame = *it;
        pendingPreview = 0;
        previewStatus.setText("Loading " + q(frame.path.filename().string()));
        double lo = black, hi = white;
        bool common = commonStretch;
        const auto generation = previewGeneration;
        const auto projectPath = project->path();
        previewThread = QThread::create([this, frame, lo, hi, common, projectPath, generation]() mutable {
            try {
                ss::Project snapshot(projectPath);
                bool calibrated = false;
                auto im = ss::previewFrame(snapshot, frame.id, calibrated);
                auto image = preview(im, lo, hi, common);
                QMetaObject::invokeMethod(
                    this,
                    [this, image, frame, lo, hi, generation, calibrated] {
                        if (generation != previewGeneration)
                            return;
                        black = lo;
                        white = hi;
                        view.display(image, frame.stars);
                        previewStatus.setText(q(frame.path.filename().string()) +
                                              (calibrated ? " · calibrated" : " · raw exposure") +
                                              " · preview stretch only");
                    },
                    Qt::QueuedConnection);
            } catch (const std::exception &e) {
                auto message = QString::fromUtf8(e.what());
                QMetaObject::invokeMethod(
                    this,
                    [this, message, generation] {
                        if (generation == previewGeneration)
                            previewStatus.setText(message);
                    },
                    Qt::QueuedConnection);
            }
        });
        connect(previewThread, &QThread::finished, this, [this] {
            auto *done = previewThread;
            previewThread = nullptr;
            done->deleteLater();
            if (pendingPreview)
                loadPreview();
        });
        previewThread->start();
    }
    void settings() {
        if (!project)
            return;
        auto s = project->settings();
        QDialog dialog(this);
        dialog.setWindowTitle("Processing and grading");
        QFormLayout form(&dialog);
        QDoubleSpinBox memory, scratch, low, high, fwhm, ecc, keep;
        QSpinBox threads, iterations;
        QLineEdit cache(q(s.cacheDirectory.string()));
        QCheckBox rejection, poly, uncal, diagnostics;
        QComboBox reference, format;
        memory.setRange(64, 1048576);
        memory.setValue(double(s.memory / ss::MiB));
        memory.setSuffix(" MiB");
        scratch.setRange(0, 100000);
        scratch.setValue(double(s.scratch) / ss::GiB);
        scratch.setSuffix(" GiB");
        threads.setRange(1, 1024);
        threads.setValue(s.threads);
        iterations.setRange(1, 10);
        iterations.setValue(s.iterations);
        low.setRange(0.1, 20);
        high.setRange(0.1, 20);
        low.setValue(s.lowSigma);
        high.setValue(s.highSigma);
        fwhm.setRange(0, 100);
        fwhm.setValue(s.maxFwhm);
        ecc.setRange(0, 1);
        ecc.setSingleStep(.05);
        ecc.setValue(s.maxEccentricity);
        keep.setRange(.1, 100);
        keep.setValue(s.keepPercent);
        keep.setSuffix(" %");
        rejection.setChecked(s.rejection);
        poly.setChecked(s.polynomial);
        uncal.setChecked(s.allowUncalibrated);
        diagnostics.setChecked(s.diagnostics);
        reference.addItem("Automatic sharp reference", qlonglong(0));
        for (const auto &f : model.frames)
            if (f.kind == "light")
                reference.addItem(QString::number(f.id) + " · " + q(f.path.filename().string()),
                                  qlonglong(f.id));
        reference.setCurrentIndex(std::max(0, reference.findData(qlonglong(s.reference))));
        format.addItems({"FITS — uncompressed", "FITS — lossless compressed", "XISF — uncompressed",
                         "XISF — Zstandard + shuffle"});
        format.setCurrentIndex(int(s.format));
        form.addRow("Memory budget", &memory);
        form.addRow("Scratch budget", &scratch);
        form.addRow("Scratch directory", &cache);
        form.addRow("CPU threads", &threads);
        form.addRow("Reference", &reference);
        form.addRow("Second-order distortion correction", &poly);
        form.addRow("Allow missing calibration", &uncal);
        form.addRow("Sigma rejection", &rejection);
        form.addRow("Low sigma", &low);
        form.addRow("High sigma", &high);
        form.addRow("Rejection iterations", &iterations);
        form.addRow("Maximum FWHM (0 disables)", &fwhm);
        form.addRow("Maximum eccentricity (0 disables)", &ecc);
        form.addRow("Keep best FWHM per filter", &keep);
        form.addRow("Master format", &format);
        form.addRow("Coverage, weights, and rejection maps", &diagnostics);
        auto *note = new QLabel("Manual inclusions override grading rules. Corrupt or unregistered frames "
                                "must be fixed or excluded.");
        note->setWordWrap(true);
        form.addRow(note);
        QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
        form.addRow(&buttons);
        connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted)
            return;
        s.memory = uint64_t(memory.value()) * ss::MiB;
        s.scratch = uint64_t(scratch.value() * ss::GiB);
        s.cacheDirectory = cache.text().toStdString();
        s.threads = threads.value();
        s.iterations = iterations.value();
        s.reference = reference.currentData().toLongLong();
        s.lowSigma = low.value();
        s.highSigma = high.value();
        s.maxFwhm = fwhm.value();
        s.maxEccentricity = ecc.value();
        s.keepPercent = keep.value();
        s.rejection = rejection.isChecked();
        s.polynomial = poly.isChecked();
        s.allowUncalibrated = uncal.isChecked();
        s.diagnostics = diagnostics.isChecked();
        s.format = ss::Format(format.currentIndex());
        try {
            project->settings(s);
            summary();
            auto selected = ss::selectedLights(model.frames, s);
            status.setText(status.text() + QString(" · %1 lights pass grading").arg(selected.size()));
        } catch (const std::exception &e) {
            error(e.what());
        }
    }
    void calibration() {
        if (!project)
            return;
        QDialog dialog(this);
        dialog.setWindowTitle("Calibration assignments");
        dialog.resize(900, 600);
        QVBoxLayout layout(&dialog);
        auto *text = new QPlainTextEdit;
        text->setReadOnly(true);
        auto plan = ss::calibrationPlan(model.frames);
        QStringList lines;
        for (const auto &v : plan) {
            auto row = v.toObject();
            QString line = "Frame " + QString::number(row["frame"].toInteger()) + " · " +
                           row["session"].toString() + " · " + row["filter"].toString();
            for (const char *kind : {"bias", "dark", "flat"})
                line += "\n  " + QString(kind) + ": " +
                        QString::fromUtf8(QJsonDocument(row[kind].toArray()).toJson(QJsonDocument::Compact));
            if (row.contains("error"))
                line += "\n  ERROR: " + row["error"].toString();
            lines << line;
        }
        text->setPlainText(lines.join("\n\n"));
        layout.addWidget(text);
        auto *hint =
            new QLabel("Choose overrides for the selected table rows. Automatic matches camera, binning, "
                       "gain/offset, exposure, filter, and night. Empty matches are visible above.");
        hint->setWordWrap(true);
        layout.addWidget(hint);
        QFormLayout form;
        std::array<QComboBox, 4> choices;
        QStringList kinds{"bias", "dark", "flat", "darkflat"};
        for (int i = 0; i < 4; ++i) {
            auto &combo = choices[size_t(i)];
            combo.addItem("Keep current assignment", "keep");
            combo.addItem("Automatic", "");
            combo.addItem("None — explicitly skip", "none");
            std::map<std::string, std::vector<int64_t>> groups;
            for (const auto &f : model.frames)
                if (q(f.kind) == kinds[i]) {
                    auto label = f.session + " / " + f.filter + " / " +
                                 ss::str(f.descriptor.header, "EXPTIME") +
                                 (f.master ? " / master " + std::to_string(f.id) : " / raw");
                    groups[label].push_back(f.id);
                }
            for (const auto &[label, ids] : groups) {
                QStringList list;
                for (auto id : ids)
                    list << QString::number(id);
                combo.addItem(q(label) + QString(" (%1 frames)").arg(ids.size()), list.join(','));
            }
            form.addRow(kinds[i], &combo);
        }
        layout.addLayout(&form);
        QDialogButtonBox buttons(QDialogButtonBox::Apply | QDialogButtonBox::Close);
        layout.addWidget(&buttons);
        connect(buttons.button(QDialogButtonBox::Close), &QPushButton::clicked, &dialog, &QDialog::reject);
        connect(buttons.button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [&] {
            if (selectedRows().empty()) {
                QMessageBox::information(
                    &dialog, "Select frames",
                    "Select the lights or flats receiving these assignments in the main table first.");
                return;
            }
            try {
                project->transaction([&] {
                    for (auto row : selectedRows()) {
                        auto &f = model.frames[row];
                        for (int i = 0; i < 4; ++i) {
                            auto id = choices[size_t(i)].currentData().toString();
                            if (id == "keep")
                                continue;
                            auto key = "SS_" + kinds[i].toUpper();
                            if (id.isEmpty())
                                f.descriptor.header.remove(key);
                            else
                                f.descriptor.header[key] = id;
                        }
                        project->save(f);
                    }
                });
                dialog.accept();
                reload();
            } catch (const std::exception &e) {
                error(e.what());
            }
        });
        dialog.exec();
    }
    void closeEvent(QCloseEvent *event) override {
        if (worker.state() != QProcess::NotRunning) {
            auto answer = QMessageBox::question(this, "Processing is running",
                                                "Cancel processing and close? Completed stages are saved.");
            if (answer != QMessageBox::Yes) {
                event->ignore();
                return;
            }
        }
        event->accept();
    }
};
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("SideraStack");
    app.setOrganizationName("SideraStack");
    app.setApplicationVersion(SIDERASTACK_VERSION);
    Window window;
    auto args = app.arguments();
    if (args.size() > 1 && !args[1].startsWith("--"))
        window.open(args[1]);
    window.show();
    if (args.contains("--smoke-test"))
        QTimer::singleShot(500, &app, &QCoreApplication::quit);
    return app.exec();
}

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
    QHash<qlonglong, int> rowsById;
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
            if (!f.error.empty())
                return q(f.error);
            if (f.kind == "unknown")
                return "Assign frame type";
            if (f.kind != "light")
                return f.master ? "Calibration master" : "Calibration frame";
            if (f.master)
                return "Light master";
            return f.transform.valid ? "Ready"
                   : f.stars.empty() ? (f.calibrationKey.empty() ? "Needs calibration" : "Calibrated")
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
    void updateFrame(int64_t id) {
        if (!project)
            return;
        auto frame = project->frame(id);
        if (!frame)
            return;
        if (auto it = rowsById.find(id); it != rowsById.end()) {
            frames[size_t(*it)] = std::move(*frame);
            Q_EMIT dataChanged(index(*it, 0), index(*it, columns.size() - 1));
        } else {
            int row = int(frames.size());
            beginInsertRows({}, row, row);
            rowsById[id] = row;
            frames.push_back(std::move(*frame));
            endInsertRows();
        }
    }
    void reload() {
        beginResetModel();
        frames = project ? project->frames() : std::vector<ss::Frame>{};
        rowsById.clear();
        for (int row = 0; row < int(frames.size()); ++row)
            rowsById[frames[size_t(row)].id] = row;
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
    void display(const QImage &image, const std::vector<ss::Star> &stars, bool preserve = false) {
        auto previousSize = scene.sceneRect().size();
        auto previousCenter = mapToScene(viewport()->rect().center());
        auto previousTransform = transform();
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
        if (preserve && previousSize.width() > 0 && previousSize.height() > 0) {
            setTransform(previousTransform);
            scale(previousSize.width() / image.width(), previousSize.height() / image.height());
            centerOn(previousCenter.x() * image.width() / previousSize.width(),
                     previousCenter.y() * image.height() / previousSize.height());
        } else
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
QImage preview(const ss::Image &im, double &black, double &white, bool common, int maxEdge = 0) {
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
    auto size = QSize(im.width, im.height);
    if (maxEdge > 0 && std::max(im.width, im.height) > maxEdge)
        size.scale(maxEdge, maxEdge, Qt::KeepAspectRatio);
    QImage result(size, QImage::Format_RGB32);
    if (result.isNull())
        throw ss::Error("Preview allocation failed");
    for (int y = 0; y < result.height(); ++y) {
        if (QThread::currentThread()->isInterruptionRequested())
            throw ss::Error("Preview cancelled");
        auto *row = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) {
            size_t i = size_t(y * int64_t(im.height) / result.height()) * im.width +
                       x * int64_t(im.width) / result.width();
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
struct CachedPreview {
    QImage image;
    bool calibrated = false;
    double black = NAN, white = NAN;
};
QString previewKey(const ss::Frame &frame, const ss::fs::path &projectPath, double lo, double hi, bool common,
                   const QString &calibration) {
    QFileInfo source(q(frame.path.string())), prepared(q(frame.calibratedPath.string()));
    QJsonObject state{{"project", q(projectPath.string())},
                      {"id", qint64(frame.id)},
                      {"source", source.absoluteFilePath()},
                      {"identity", q(frame.identity)},
                      {"size", source.size()},
                      {"modified", source.lastModified().toMSecsSinceEpoch()},
                      {"prepared", q(frame.calibrationKey)},
                      {"preparedPath", prepared.absoluteFilePath()},
                      {"preparedModified", prepared.lastModified().toMSecsSinceEpoch()},
                      {"preparedSize", prepared.size()},
                      {"header", frame.descriptor.header},
                      {"cfa", q(frame.descriptor.cfa)},
                      {"common", common},
                      {"calibration", calibration},
                      {"version", 1}};
    if (common) {
        ss::put(state, "black", lo);
        ss::put(state, "white", hi);
    }
    return q(ss::hash(ss::json(state).toStdString()));
}
CachedPreview readPreviewCache(const ss::Settings &settings, const QString &key) {
    CachedPreview result;
    if (!ss::previewDiskBudget(settings))
        return result;
    auto path = q((settings.cacheDirectory / "previews").string()) + "/preview-" + key + ".png";
    QImage image(path);
    if (image.isNull() || image.text("key") != key || image.width() > 2048 || image.height() > 2048) {
        QFile::remove(path);
        return result;
    }
    bool loOk = false, hiOk = false;
    result.black = image.text("black").toDouble(&loOk);
    result.white = image.text("white").toDouble(&hiOk);
    if (!loOk || !hiOk || !std::isfinite(result.black) || !std::isfinite(result.white) ||
        result.white <= result.black) {
        QFile::remove(path);
        return {};
    }
    result.image = image;
    result.calibrated = image.text("calibrated") == "1";
    QFile file(path);
    if (file.open(QIODevice::ReadOnly))
        file.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
    return result;
}
void writePreviewCache(const ss::Settings &settings, const QString &key, const CachedPreview &preview) {
    const auto limit = ss::previewDiskBudget(settings);
    if (!limit || QThread::currentThread()->isInterruptionRequested())
        return;
    auto image = preview.image;
    image.setText("key", key);
    image.setText("black", QString::number(preview.black, 'g', 17));
    image.setText("white", QString::number(preview.white, 'g', 17));
    image.setText("calibrated", preview.calibrated ? "1" : "0");
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG") || uint64_t(encoded.size()) > limit)
        return;
    QDir directory(q((settings.cacheDirectory / "previews").string()));
    if (!directory.mkpath("."))
        return;
    const auto path = directory.filePath("preview-" + key + ".png");
    if (directory.exists("preview-" + key + ".png"))
        return;
    auto files = directory.entryInfoList({"preview-*.png"}, QDir::Files, QDir::Time | QDir::Reversed);
    uint64_t used = 0;
    for (const auto &file : files)
        used += file.size();
    for (const auto &file : files) {
        if (used + uint64_t(encoded.size()) <= limit)
            break;
        if (QFile::remove(file.absoluteFilePath()))
            used -= file.size();
    }
    if (used + uint64_t(encoded.size()) > limit || directory.exists("preview-" + key + ".png"))
        return;
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly) && file.write(encoded) == encoded.size())
        file.commit();
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
    QTimer blink, updateTimer;
    QSet<qlonglong> changedFrameIds;
    QCache<QString, CachedPreview> previewCache;
    QSet<QString> failedPreviewKeys;
    QString previewCalibration;
    QPushButton *blinkButton = nullptr;
    QDoubleSpinBox blinkInterval;
    std::vector<int64_t> blinkIds;
    size_t blinkCursor = 0;
    bool pendingIsPrefetch = false;
    int64_t displayedFrame = 0;
    QAction *analyzeAction = nullptr;
    size_t previewRenders = 0;
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
        action("Calibration assignments", [this] { calibration(); }, true);
        action("Calibrate", [this] { start("calibrate"); }, true);
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
        analyzeAction = action("Analyze", [this] { start("analyze"); }, true);
        analyzeAction->setToolTip(
            "Measure prepared lights. Run Calibrate first if preparation is missing or stale.");
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
        auto *intro = new QLabel("Import → Calibrate → Analyze → Review → Stack → Export   ·   Linear "
                                 "masters for your image editor");
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
        blinkButton = new QPushButton("Start blink");
        blinkButton->setEnabled(false);
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
        blinkInterval.setRange(0.1, 60);
        blinkInterval.setSingleStep(0.1);
        blinkInterval.setDecimals(1);
        blinkInterval.setValue(1);
        blinkInterval.setSuffix(" s / frame");
        blinkInterval.setToolTip("Seconds between frames during blink playback");
        controls->addWidget(&blinkInterval);
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
            blinkButton->setChecked(false);
            previewCache.clear();
            black = white = NAN;
            previewCalibration = q(ss::calibrationRevision(model.frames, project->settings()));
            showCurrent();
            plot.update();
            summary();
            updateReadiness();
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
        connect(&filter, &QComboBox::currentTextChanged, this, [this](const QString &s) {
            blinkButton->setChecked(false);
            proxy.setFilterFixedString(s == "All frames" ? QString() : s);
        });
        connect(exclude, &QPushButton::clicked, this, [this] { select(-1); });
        connect(include, &QPushButton::clicked, this, [this] { select(1); });
        connect(edit, &QPushButton::clicked, this, [this] { editSelected(); });
        connect(stars, &QCheckBox::toggled, &view, &ImageView::toggleStars);
        connect(common, &QCheckBox::toggled, this, [this](bool on) {
            blinkButton->setChecked(false);
            commonStretch = on;
            ++previewGeneration;
            pendingPreview = 0;
            black = white = NAN;
            showCurrent();
        });
        connect(table.selectionModel(), &QItemSelectionModel::currentRowChanged, this,
                [this] { showCurrent(); });
        connect(table.selectionModel(), &QItemSelectionModel::selectionChanged, this, [this] {
            blinkButton->setChecked(false);
            blinkButton->setEnabled(project && table.selectionModel()->selectedRows().size() >= 2);
        });
        connect(table.horizontalHeader(), &QHeaderView::sortIndicatorChanged, this,
                [this] { blinkButton->setChecked(false); });
        connect(blinkButton, &QPushButton::toggled, this, [this](bool on) {
            blink.stop();
            ++previewGeneration;
            pendingPreview = 0;
            if (previewThread)
                previewThread->requestInterruption();
            blinkButton->setText(on ? "Stop blink" : "Start blink");
            blinkIds.clear();
            if (on) {
                failedPreviewKeys.clear();
                auto rows = table.selectionModel()->selectedRows();
                std::sort(rows.begin(), rows.end(),
                          [](const auto &a, const auto &b) { return a.row() < b.row(); });
                for (auto row : rows)
                    blinkIds.push_back(model.frames[size_t(proxy.mapToSource(row).row())].id);
                if (blinkIds.size() < 2) {
                    blinkButton->setChecked(false);
                    return;
                }
                auto current = proxy.mapToSource(table.currentIndex());
                auto id = current.isValid() ? model.frames[size_t(current.row())].id : blinkIds.front();
                auto it = std::find(blinkIds.begin(), blinkIds.end(), id);
                blinkCursor = it == blinkIds.end() ? 0 : size_t(it - blinkIds.begin());
            }
            showCurrent();
        });
        connect(&blinkInterval, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this](double seconds) {
                    if (blink.isActive())
                        blink.start(qRound(seconds * 1000));
                });
        connect(&blink, &QTimer::timeout, this, [this] {
            blink.stop();
            if (!blinkButton->isChecked() || blinkIds.size() < 2)
                return;
            blinkCursor = (blinkCursor + 1) % blinkIds.size();
            auto it = model.rowsById.find(blinkIds[blinkCursor]);
            if (it == model.rowsById.end()) {
                blinkButton->setChecked(false);
                return;
            }
            auto next = proxy.mapFromSource(model.index(*it, 0));
            if (next == table.currentIndex())
                showCurrent();
            else
                table.selectionModel()->setCurrentIndex(next, QItemSelectionModel::NoUpdate);
        });
        updateTimer.setSingleShot(true);
        updateTimer.setInterval(100);
        connect(&updateTimer, &QTimer::timeout, this, [this] { updateFrames(); });
        connect(&worker, &QProcess::readyReadStandardOutput, this, [this] { readWorkerOutput(); });
        connect(&worker, &QProcess::readyReadStandardError, this,
                [this] { log.appendPlainText(QString::fromUtf8(worker.readAllStandardError())); });
        connect(&worker, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus exit) {
                    readWorkerOutput();
                    updateFrames();
                    setBusy(false);
                    updateReadiness();
                    summary();
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
            updateFrames();
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
        updateReadiness();
    }
    void open(const QString &path) {
        if (worker.state() != QProcess::NotRunning)
            return;
        try {
            blinkButton->setChecked(false);
            changedFrameIds.clear();
            updateTimer.stop();
            previewCache.clear();
            displayedFrame = 0;
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
            previewCache.clear();
            failedPreviewKeys.clear();
            black = white = NAN;
            previewCalibration = q(ss::calibrationRevision(model.frames, project->settings()));
            previewCache.setMaxCost(
                int(std::min<uint64_t>(256 * ss::MiB, project->settings().memory / 16) / 1024));
            updateReadiness();
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
    void readWorkerOutput() {
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
                } else if (ss::str(o, "event") == "frame-updated") {
                    changedFrameIds.insert(o["id"].toInteger());
                    if (!updateTimer.isActive())
                        updateTimer.start();
                } else if (ss::str(o, "event") == "progress") {
                    double total = ss::number(o, "total", 0), done = ss::number(o, "done", 0);
                    progress.setValue(total > 0 ? int(1000 * done / total) : 0);
                    status.setText(o["stage"].toString() + " · " + o["message"].toString());
                }
            } catch (...) {
                log.appendPlainText(QString::fromUtf8(line));
            }
        }
    }
    void updateReadiness() {
        if (analyzeAction)
            analyzeAction->setEnabled(project && !model.busy &&
                                      ss::preparationReady(model.frames, project->settings()));
    }
    void updateFrames() {
        updateTimer.stop();
        auto ids = std::exchange(changedFrameIds, {});
        if (!project)
            return;
        try {
            for (auto id : ids) {
                auto previousRow = model.rowsById.value(id, -1);
                auto previousKey =
                    previousRow >= 0 ? model.frames[size_t(previousRow)].calibrationKey : std::string{};
                model.updateFrame(id);
                const auto row = model.rowsById.value(id, -1);
                if (row >= 0) {
                    const auto &frame = model.frames[size_t(row)];
                    auto name = q(frame.filter);
                    if (!name.isEmpty() && filter.findText(name) < 0)
                        filter.addItem(name);
                    if (id == displayedFrame && frame.calibrationKey != previousKey) {
                        black = white = NAN;
                        showCurrent();
                    } else if (id == displayedFrame && view.pixels) {
                        // Updating metrics/catalogs must not decode or stretch the image again.
                        auto image = view.pixels->pixmap().toImage();
                        view.display(image, displayStars(frame, image), true);
                    }
                }
            }
            auto calibration = q(ss::calibrationRevision(model.frames, project->settings()));
            if (calibration != previewCalibration) {
                previewCalibration = calibration;
                previewCache.clear();
                black = white = NAN;
                showCurrent();
            }
            plot.update();
            if (!model.busy)
                updateReadiness();
        } catch (const std::exception &e) {
            log.appendPlainText(QString::fromUtf8(e.what()));
        }
    }
    std::vector<ss::Star> displayStars(const ss::Frame &frame, const QImage &image) {
        auto stars = frame.stars;
        for (auto &star : stars) {
            star.x *= double(image.width()) / std::max(1, frame.descriptor.width);
            star.y *= double(image.height()) / std::max(1, frame.descriptor.height);
        }
        return stars;
    }
    void presentPreview(const ss::Frame &frame, const CachedPreview &preview) {
        black = preview.black;
        white = preview.white;
        view.display(preview.image, displayStars(frame, preview.image),
                     blinkButton->isChecked() || displayedFrame == frame.id);
        displayedFrame = frame.id;
        previewStatus.setText(q(frame.path.filename().string()) +
                              (preview.calibrated ? " · calibrated" : " · uncalibrated") +
                              " · preview stretch only");
        if (blinkButton->isChecked())
            blink.start(qRound(blinkInterval.value() * 1000));
    }
    void prefetchNext() {
        if (!project || previewThread || pendingPreview || !blinkButton->isChecked() || blinkIds.size() < 2)
            return;
        for (size_t offset = 1; offset <= std::min<size_t>(2, blinkIds.size() - 1); ++offset) {
            auto id = blinkIds[(blinkCursor + offset) % blinkIds.size()];
            auto row = model.rowsById.value(id, -1);
            if (row < 0)
                continue;
            const auto &frame = model.frames[size_t(row)];
            auto size = QSize(frame.descriptor.width, frame.descriptor.height);
            if (std::max(size.width(), size.height()) > 2048)
                size.scale(2048, 2048, Qt::KeepAspectRatio);
            auto cost = (int64_t(size.width()) * size.height() * 4 + 1023) / 1024;
            if (cost * int64_t(offset + 1) > previewCache.maxCost())
                continue;
            auto key = previewKey(frame, project->path(), black, white, commonStretch, previewCalibration);
            if (!previewCache.contains(key) && !failedPreviewKeys.contains(key)) {
                pendingPreview = id;
                pendingIsPrefetch = true;
                loadPreview();
                return;
            }
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
            updateReadiness();
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
        const auto &frame = model.frames[size_t(index.row())];
        if (blinkButton->isChecked()) {
            auto key = previewKey(frame, project->path(), black, white, commonStretch, previewCalibration);
            if (auto cached = previewCache.object(key)) {
                pendingPreview = 0;
                presentPreview(frame, *cached);
                prefetchNext();
                return;
            }
        }
        pendingPreview = frame.id;
        pendingIsPrefetch = false;
        if (!previewThread)
            loadPreview();
    }
    void loadPreview() {
        if (!project || !pendingPreview)
            return;
        auto row = model.rowsById.value(pendingPreview, -1);
        if (row < 0) {
            pendingPreview = 0;
            return;
        }
        auto frame = model.frames[size_t(row)];
        const auto prefetch = pendingIsPrefetch;
        pendingPreview = 0;
        const bool reduced = blinkButton->isChecked();
        if (!prefetch)
            previewStatus.setText("Loading " + q(frame.path.filename().string()));
        double lo = black, hi = white;
        bool common = commonStretch;
        const auto generation = previewGeneration;
        const auto projectPath = project->path();
        const auto settings = project->settings();
        const auto calibration = previewCalibration;
        const auto key = previewKey(frame, projectPath, lo, hi, common, calibration);
        previewThread = QThread::create([this, frame, lo, hi, common, projectPath, generation, prefetch,
                                         reduced, settings, key, calibration]() mutable {
            try {
                auto preview = reduced ? readPreviewCache(settings, key) : CachedPreview{};
                bool rendered = preview.image.isNull();
                if (rendered) {
                    ss::Project snapshot(projectPath);
                    auto im = ss::previewFrame(snapshot, frame.id, preview.calibrated);
                    preview.image = ::preview(im, lo, hi, common, reduced ? 2048 : 0);
                    preview.black = lo;
                    preview.white = hi;
                    if (reduced)
                        writePreviewCache(
                            settings,
                            previewKey(frame, projectPath, preview.black, preview.white, common, calibration),
                            preview);
                }
                if (QThread::currentThread()->isInterruptionRequested())
                    return;
                QMetaObject::invokeMethod(
                    this,
                    [this, preview, frame, generation, prefetch, reduced, key, projectPath, rendered,
                     calibration, common] {
                        if (!project || project->path() != projectPath)
                            return;
                        previewRenders += rendered;
                        if (reduced) {
                            int cost = int((preview.image.sizeInBytes() + 1023) / 1024);
                            auto resolved = previewKey(frame, projectPath, preview.black, preview.white,
                                                       common, calibration);
                            previewCache.insert(resolved, new CachedPreview(preview), cost);
                        }
                        if (!prefetch && generation == previewGeneration) {
                            auto row = model.rowsById.value(frame.id, -1);
                            if (row >= 0)
                                presentPreview(model.frames[size_t(row)], preview);
                        }
                    },
                    Qt::QueuedConnection);
            } catch (const std::exception &e) {
                auto message = QString::fromUtf8(e.what());
                QMetaObject::invokeMethod(
                    this,
                    [this, message, generation, prefetch, key] {
                        if (prefetch)
                            failedPreviewKeys.insert(key);
                        if (!prefetch && generation == previewGeneration) {
                            previewStatus.setText(message);
                            blinkButton->setChecked(false);
                        }
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
            else
                prefetchNext();
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
        auto plan = ss::calibrationPlan(model.frames);
        QTableWidget assignments(int(plan.size()), 7);
        assignments.setHorizontalHeaderLabels(
            {"Night", "Filter", "Lights", "Dark", "Bias", "Flat", "Details"});
        assignments.setEditTriggers(QAbstractItemView::NoEditTriggers);
        assignments.setWordWrap(true);
        for (int i = 0; i < plan.size(); ++i) {
            auto row = plan[i].toObject();
            QStringList notes;
            QStringList names[3];
            for (const auto &entry : row["details"].toArray()) {
                auto detail = entry.toObject();
                const auto kind = detail["kind"].toString();
                names[kind == "dark" ? 0 : kind == "bias" ? 1 : 2] << detail["file"].toString();
                const auto sources = detail["inferred"].toObject();
                QStringList inferred;
                for (auto it = sources.begin(); it != sources.end(); ++it)
                    if (it.value() == "filename")
                        inferred << it.key();
                if (!inferred.empty())
                    notes << detail["file"].toString() + ": " + inferred.join(", ") + " from filename";
                QStringList missing;
                for (auto value : detail["missing"].toArray())
                    missing << value.toString();
                if (!missing.empty())
                    notes << detail["file"].toString() + ": missing " + missing.join(", ");
            }
            for (auto warning : row["warnings"].toArray())
                notes << warning.toString();
            if (row.contains("error"))
                notes << "Resolve: " + row["error"].toString();
            QStringList cells{row["session"].toString(), row["filter"].toString(),
                              QString::number(row["count"].toInt())};
            for (const auto &list : names)
                cells << (list.empty() ? "None" : list.join("\n"));
            cells << notes.join("\n");
            for (int column = 0; column < cells.size(); ++column) {
                auto *item = new QTableWidgetItem(cells[column]);
                item->setToolTip(cells[column]);
                if (row.contains("error"))
                    item->setForeground(QColor("#b93838"));
                assignments.setItem(i, column, item);
            }
        }
        assignments.horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        assignments.horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
        assignments.resizeRowsToContents();
        layout.addWidget(&assignments);
        auto *hint =
            new QLabel("Calibrate automatically matches and prepares your lights. Known conflicts "
                       "are rejected; missing metadata is disclosed above. To override a match, "
                       "select its lights or flats in the main table and apply an assignment below.");
        hint->setWordWrap(true);
        layout.addWidget(hint);
        QLineEdit directory(q(
            ss::str(project->record("calibration"), "directory", project->path().string() + ".calibrated")));
        QPushButton browse("Browse…");
        QHBoxLayout directoryLayout;
        directoryLayout.addWidget(new QLabel("Prepared light images"));
        directoryLayout.addWidget(&directory, 1);
        directoryLayout.addWidget(&browse);
        layout.addLayout(&directoryLayout);
        connect(&browse, &QPushButton::clicked, &dialog, [&] {
            auto chosen = QFileDialog::getExistingDirectory(&dialog, "Prepared light images directory",
                                                            directory.text());
            if (!chosen.isEmpty())
                directory.setText(chosen);
        });
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
                if (q(f.kind) == kinds[i] && f.selection >= 0) {
                    auto label = f.master ? f.path.filename().string()
                                          : f.session + " / " + f.filter + " / " +
                                                ss::str(f.descriptor.header, "EXPTIME") + "s / " +
                                                ss::str(f.descriptor.header, "INSTRUME") + " / gain " +
                                                ss::str(f.descriptor.header, "GAIN") + " / offset " +
                                                ss::str(f.descriptor.header, "OFFSET") + " / temp " +
                                                ss::str(f.descriptor.header, "CCD-TEMP") + " / bin " +
                                                ss::str(f.descriptor.header, "XBINNING") + "x" +
                                                ss::str(f.descriptor.header, "YBINNING") + " / " +
                                                ss::str(f.descriptor.header, "READOUTM") + " / raw";
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
            bool changes = false;
            for (const auto &combo : choices)
                changes |= combo.currentData().toString() != "keep";
            if (changes && selectedRows().empty()) {
                QMessageBox::information(&dialog, "Select frames",
                                         "Select the receiving lights or flats first.");
                return;
            }
            try {
                if (directory.text().trimmed().isEmpty())
                    throw ss::Error("Choose a prepared-image directory");
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
                project->record("calibration", {{"directory", directory.text().trimmed()}});
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

#pragma once
#include "stellastack/core.hpp"
#include <QtWidgets>
#include <algorithm>
#include <functional>

namespace ss::gui {
inline QString q(const std::string &s) {
    return QString::fromStdString(s);
}
inline QString value(double v) {
    return std::isfinite(v) ? QString::number(v, 'g', 5) : QString::fromUtf8("—");
}
inline const QStringList columns{
    "Use", "ID",           "File",  "Type",       "Filter", "Night",        "Master", "Bias removed", "FWHM",
    "HFR", "Eccentricity", "Stars", "Background", "Noise",  "Transparency", "RMS",    "Status"};
class FrameModel : public QAbstractTableModel {
  public:
    std::vector<ss::Frame> frames;
    ss::Project *project = nullptr;
    bool busy = false;
    QHash<qlonglong, int> rowsById;
    std::function<void()> changed;
    QHash<qlonglong, SelectionDecision> decisions;
    QHash<qlonglong, QString> problems;
    bool included(const Frame &f) const {
        return decisions.contains(f.id) ? decisions[f.id].included : f.selection >= 0;
    }
    QString state(const Frame &f) const {
        if (!included(f))
            return decisions.contains(f.id) ? q(decisions[f.id].reason) : QString("Manually excluded");
        if (!f.error.empty())
            return q(f.error);
        if (problems.contains(f.id))
            return problems[f.id];
        if (f.kind == "unknown")
            return "Assign frame type";
        if (f.kind == "light" && f.master)
            return "Light master";
        if (f.kind != "light")
            return f.master ? "Calibration master" : "Calibration frame";
        return f.transform.valid ? "Ready"
               : f.stars.empty() ? (f.calibrationKey.empty() ? "Needs calibration" : "Calibrated")
                                 : "Measured";
    }
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
                return included(f) ? Qt::Checked : Qt::Unchecked;
            if (c == 6)
                return f.master ? Qt::Checked : Qt::Unchecked;
            if (c == 7)
                return f.biasSubtracted ? Qt::Checked : Qt::Unchecked;
        }
        if (role == Qt::ToolTipRole)
            return q(f.path.string()) + "\n" + state(f) + "\n" +
                   (decisions.contains(f.id) ? q(decisions[f.id].reason) : QString());
        if (role == Qt::ForegroundRole) {
            if (!f.error.empty())
                return QColor("#b93838");
            if (!included(f))
                return QApplication::palette().color(QPalette::Disabled, QPalette::Text);
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
            return state(f);
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
class FrameProxy : public QSortFilterProxyModel {
  public:
    QString kind, band, night, state, search;
    bool filterAcceptsRow(int row, const QModelIndex &) const override {
        auto *frames = static_cast<FrameModel *>(sourceModel());
        const auto &f = frames->frames[size_t(row)];
        if (!kind.isEmpty() && q(f.kind) != kind)
            return false;
        if (!band.isEmpty() && q(f.filter) != band)
            return false;
        if (!night.isEmpty() && q(f.session) != night)
            return false;
        if (!search.isEmpty() && !q(f.path.filename().string()).contains(search, Qt::CaseInsensitive))
            return false;
        if (state == "Included" && !frames->included(f))
            return false;
        if (state == "Excluded" && frames->included(f))
            return false;
        if (state == "Needs attention" &&
            !(f.kind == "unknown" || !f.error.empty() || frames->problems.contains(f.id)))
            return false;
        return true;
    }
    void refresh() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        beginFilterChange();
        endFilterChange(Direction::Rows);
#else
        invalidateFilter();
#endif
    }
};
class FrameDelegate : public QStyledItemDelegate {
  public:
    explicit FrameDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
    QString displayText(const QVariant &value, const QLocale &locale) const override {
        if (value.metaType().id() == QMetaType::Double)
            return locale.toString(value.toDouble(), 'g', 5);
        return QStyledItemDelegate::displayText(value, locale);
    }
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override {
        if (index.column() != 3)
            return QStyledItemDelegate::createEditor(parent, option, index);
        auto *editor = new QComboBox(parent);
        for (auto type : QStringList{"unknown", "light", "dark", "flat", "bias", "darkflat"})
            editor->addItem(type == "darkflat" ? "Dark-flat" : type.left(1).toUpper() + type.mid(1), type);
        return editor;
    }
    void setEditorData(QWidget *editor, const QModelIndex &index) const override {
        if (auto *combo = qobject_cast<QComboBox *>(editor))
            combo->setCurrentIndex(combo->findData(index.data(Qt::EditRole)));
        else
            QStyledItemDelegate::setEditorData(editor, index);
    }
    void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const override {
        if (auto *combo = qobject_cast<QComboBox *>(editor))
            model->setData(index, combo->currentData(), Qt::EditRole);
        else
            QStyledItemDelegate::setModelData(editor, model, index);
    }
};
class ImportDropArea : public QFrame {
  public:
    std::function<void(const QStringList &)> import;
    ImportDropArea() {
        setAcceptDrops(true);
    }
    void dragEnterEvent(QDragEnterEvent *event) override {
        auto urls = event->mimeData()->urls();
        if (isEnabled() && event->mimeData()->hasUrls() &&
            std::all_of(urls.begin(), urls.end(), [](const QUrl &url) { return url.isLocalFile(); }))
            event->acceptProposedAction();
    }
    void dropEvent(QDropEvent *event) override {
        QStringList paths;
        for (const auto &url : event->mimeData()->urls())
            if (url.isLocalFile())
                paths << url.toLocalFile();
        if (!paths.empty() && import) {
            import(paths);
            event->acceptProposedAction();
        }
    }
};
class ImageView : public QGraphicsView {
  public:
    QGraphicsScene scene;
    QGraphicsPixmapItem *pixels = nullptr;
    std::vector<QGraphicsEllipseItem *> markers;
    bool starsVisible = true;
    bool autoFit = true;
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
            fitImage();
    }
    void wheelEvent(QWheelEvent *event) override {
        autoFit = false;
        double f = event->angleDelta().y() > 0 ? 1.2 : 1 / 1.2;
        scale(f, f);
        event->accept();
    }
    void toggleStars(bool enabled) {
        starsVisible = enabled;
        for (auto *m : markers)
            m->setVisible(enabled);
    }
    void fitImage() {
        autoFit = true;
        if (pixels)
            fitInView(scene.sceneRect(), Qt::KeepAspectRatio);
    }
    void resizeEvent(QResizeEvent *event) override {
        QGraphicsView::resizeEvent(event);
        if (autoFit && pixels)
            fitInView(scene.sceneRect(), Qt::KeepAspectRatio);
    }
};
class MetricPlot : public QWidget {
  public:
    FrameModel *model;
    FrameProxy *visible = nullptr;
    int64_t current = 0;
    QString metric = "FWHM";
    std::function<void(int64_t)> select;
    std::vector<std::pair<QPointF, int64_t>> points;
    explicit MetricPlot(FrameModel *m) : model(m) {
        setMinimumHeight(150);
        setToolTip("Frame order on the horizontal axis; click a point to inspect its exposure.");
    }
    QSize sizeHint() const override {
        return {400, 140};
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
            if (f.kind == "light" && std::isfinite(measurement(f)) &&
                (!visible ||
                 visible->mapFromSource(model->index(model->rowsById.value(f.id), 0)).isValid())) {
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
        const int count = visible ? visible->rowCount() : model->rowCount();
        for (int index = 0; index < count; ++index) {
            const auto &f =
                model->frames[size_t(visible ? visible->mapToSource(visible->index(index, 0)).row() : index)];
            double v = measurement(f);
            double x = 65 + double(index) / std::max(1, count - 1) * (width() - 85);
            if (f.kind != "light" || !std::isfinite(v))
                continue;
            double y = height() - 20 - (v - lo) / (hi - lo) * (height() - 65);
            p.setPen(Qt::NoPen);
            p.setBrush(model->included(f) ? palette().highlight() : palette().mid());
            p.drawEllipse(QPointF(x, y), f.id == current ? 5 : 3, f.id == current ? 5 : 3);
            points.emplace_back(QPointF(x, y), f.id);
        }
    }
    void mousePressEvent(QMouseEvent *event) override {
        double distance = 20;
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
struct CachedPreview {
    QImage image;
    bool calibrated = false;
    double black = NAN, white = NAN;
};
QImage preview(const ss::Image &, double &, double &, bool, int maxEdge = 0);
QString previewKey(const ss::Frame &, const ss::fs::path &, double, double, bool, const QString &);
CachedPreview readPreviewCache(const ss::Settings &, const QString &);
void writePreviewCache(const ss::Settings &, const QString &, const CachedPreview &);
} // namespace ss::gui

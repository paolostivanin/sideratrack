#include "widgets.hpp"

namespace ss::gui {
QImage preview(const ss::Image &im, double &black, double &white, bool common, int maxEdge) {
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
} // namespace ss::gui

#include "converter.hpp"
#include "siderastack/core.hpp"
#include <QCryptographicHash>
#include <QFile>
#include <QLockFile>
#include <QSysInfo>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <list>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <tbb/global_control.h>
#include <tbb/parallel_for.h>
#include <tbb/task_group.h>

namespace ss {
namespace {
constexpr const char *algorithm = "siderastack-pipeline-4";
void emit(const Progress &p, const std::string &stage, size_t n, size_t total, const std::string &detail) {
    if (p)
        p(stage, n, total, detail);
}
double median(std::vector<float> values) {
    values.erase(std::remove_if(values.begin(), values.end(), [](float v) { return !std::isfinite(v); }),
                 values.end());
    if (values.empty())
        throw Error("Image contains no finite samples");
    auto mid = values.begin() + ptrdiff_t(values.size() / 2);
    std::nth_element(values.begin(), mid, values.end());
    return values.size() & 1 ? double(*mid) : (double(*mid) + *std::max_element(values.begin(), mid)) / 2;
}
bool geometry(const Frame &a, const Frame &b) {
    return a.descriptor.width == b.descriptor.width && a.descriptor.height == b.descriptor.height &&
           a.descriptor.channels == b.descriptor.channels && a.descriptor.cfa == b.descriptor.cfa;
}
bool compatible(const Frame &a, const Frame &b) {
    if (!geometry(a, b))
        return false;
    for (const char *key : {"INSTRUME", "XBINNING", "YBINNING", "GAIN", "OFFSET", "READOUTM", "ROWORDER"})
        if (str(a.descriptor.header, key) != str(b.descriptor.header, key))
            return false;
    return true;
}
std::vector<Frame> choose(const Frame &light, const std::vector<Frame> &all, const std::string &kind) {
    const auto overrideKey = "SS_" + QString::fromStdString(kind).toUpper().toStdString();
    auto specified = str(light.descriptor.header, overrideKey.c_str());
    std::set<int64_t> ids;
    if (specified == "none")
        return {};
    if (!specified.empty()) {
        for (const auto &item : QString::fromStdString(specified).split(',')) {
            bool ok = false;
            auto id = item.trimmed().toLongLong(&ok);
            if (!ok || id <= 0)
                throw Error("Invalid calibration ID override");
            ids.insert(id);
        }
    }
    std::vector<Frame> found;
    for (const auto &f : all) {
        if (f.kind != kind || f.selection < 0)
            continue;
        if (!ids.empty()) {
            if (ids.contains(f.id)) {
                if (!geometry(light, f))
                    throw Error("Calibration override has incompatible geometry/CFA");
                found.push_back(f);
            }
            continue;
        }
        if (!compatible(light, f))
            continue;
        if (kind == "flat" && f.filter != light.filter)
            continue;
        if (kind == "dark" || kind == "darkflat") {
            auto exp =
                number(light.descriptor.header, "EXPTIME", number(light.descriptor.header, "EXPOSURE"));
            auto other = number(f.descriptor.header, "EXPTIME", number(f.descriptor.header, "EXPOSURE"));
            if (!std::isfinite(exp) || !std::isfinite(other) ||
                std::abs(exp - other) > 1e-6 * std::max(1.0, exp))
                continue;
        }
        if (kind == "bias" || kind == "dark" || kind == "darkflat") {
            auto t = number(light.descriptor.header, "CCD-TEMP"), u = number(f.descriptor.header, "CCD-TEMP");
            if (std::isfinite(t) != std::isfinite(u) || (std::isfinite(t) && std::abs(t - u) > 1))
                continue;
        }
        found.push_back(f);
    }
    if (!ids.empty() && found.size() != ids.size())
        throw Error("Calibration override refers to absent, excluded, or wrong-type frames");
    if (ids.empty()) {
        bool same = std::any_of(found.begin(), found.end(),
                                [&](const Frame &f) { return f.session == light.session; });
        if (same)
            std::erase_if(found, [&](const Frame &f) { return f.session != light.session; });
        if (kind == "flat" && !same) {
            std::set<std::string> nights;
            for (const auto &f : found)
                nights.insert(f.session);
            if (nights.size() > 1)
                throw Error("Multiple flat sessions match; assign a flat group explicitly");
        }
    }
    std::vector<Frame> masters;
    for (const auto &f : found)
        if (f.master)
            masters.push_back(f);
    if (masters.size() > 1)
        throw Error("Multiple " + kind + " masters match; choose one with a calibration override");
    if (!masters.empty())
        return masters;
    return found;
}
std::string calibrationSignature(const std::vector<Frame> &all, bool allowUncalibrated) {
    std::string key = std::string(algorithm) + std::to_string(allowUncalibrated);
    for (const auto &f : all)
        if (f.kind != "light" && f.kind != "unknown" && f.selection >= 0)
            key += std::to_string(f.id) + f.identity + f.kind + f.filter + f.session +
                   std::to_string(f.master) + std::to_string(f.biasSubtracted) +
                   json(f.descriptor.header).toStdString();
    return hash(key);
}
std::string dependencyKey(const Frame &light, const std::string &calibration) {
    return hash(std::string(algorithm) + calibration + light.identity + light.filter + light.session +
                json(light.descriptor.header).toStdString());
}
struct Engine {
    Settings settings;
    std::vector<Frame> &frames;
    Progress progress;
    std::string calibrationKey;
    uint64_t cacheUsed = 0;
    std::map<std::string, std::shared_ptr<Image>> masters;
    uint64_t masterBytes = 0, hotBudget = 0, hotBytes = 0;
    struct HotEntry {
        std::shared_ptr<Image> image;
        std::list<std::string>::iterator age;
    };
    std::map<std::string, HotEntry> hot;
    std::list<std::string> hotAge;
    void setHotBudget(uint64_t budget) {
        hot.clear();
        hotAge.clear();
        hotBytes = 0;
        hotBudget = budget;
    }
    std::shared_ptr<const Image> sharedProcessed(const Frame &frame) {
        auto key = dependencyKey(frame, calibrationKey);
        if (auto found = hot.find(key); found != hot.end()) {
            hotAge.splice(hotAge.end(), hotAge, found->second.age);
            return found->second.image;
        }
        auto image = std::make_shared<Image>(processed(frame));
        const uint64_t bytes = image->samples() * sizeof(float);
        if (bytes <= hotBudget) {
            while (hotBytes + bytes > hotBudget && !hotAge.empty()) {
                auto oldest = hot.find(hotAge.front());
                hotBytes -= oldest->second.image->samples() * sizeof(float);
                hot.erase(oldest);
                hotAge.pop_front();
            }
            hotAge.push_back(key);
            hot.emplace(key, HotEntry{image, std::prev(hotAge.end())});
            hotBytes += bytes;
        }
        return image;
    }
    Engine(Settings s, std::vector<Frame> &f, Progress p)
        : settings(std::move(s)), frames(f), progress(std::move(p)),
          calibrationKey(calibrationSignature(f, settings.allowUncalibrated)) {
        fs::create_directories(settings.cacheDirectory);
        std::vector<fs::directory_entry> files;
        for (const auto &item : fs::directory_iterator(settings.cacheDirectory))
            if (item.is_regular_file() && item.path().filename().string().rfind("cache-", 0) == 0 &&
                item.path().extension() == ".fits") {
                cacheUsed += item.file_size();
                files.push_back(item);
            }
        std::sort(files.begin(), files.end(),
                  [](auto &a, auto &b) { return a.last_write_time() < b.last_write_time(); });
        for (const auto &item : files) {
            if (cacheUsed <= settings.scratch)
                break;
            cacheUsed -= item.file_size();
            fs::remove(item.path());
        }
    }
    fs::path cachedPath(const std::string &key) const {
        return settings.cacheDirectory / ("cache-" + key + ".fits");
    }
    bool cacheGet(const std::string &key, Image &im) {
        auto path = cachedPath(key);
        if (!fs::exists(path))
            return false;
        try {
            im = readImage(path, false, settings.memory / 2);
            return true;
        } catch (...) {
            checkpoint();
            auto n = fs::file_size(path);
            fs::remove(path);
            cacheUsed = cacheUsed >= n ? cacheUsed - n : 0;
            return false;
        }
    }
    void cachePut(const std::string &key, const Image &im) {
        auto estimate = uint64_t(im.samples()) * 4 + MiB;
        auto path = cachedPath(key);
        if (fs::exists(path) || estimate > settings.scratch - cacheUsed)
            return;
        auto space = fs::space(settings.cacheDirectory);
        if (space.available < estimate + 64 * MiB)
            return;
        writeImage(path, im, Format::Fits);
        cacheUsed += fs::file_size(path);
    }
    Image read(const Frame &f) {
        auto im = readImage(f.path, false, settings.memory / 2);
        if (im.width != f.descriptor.width || im.height != f.descriptor.height ||
            im.channels != f.descriptor.channels)
            throw Error("Source dimensions changed: " + f.path.string());
        im.header = f.descriptor.header;
        im.cfa = f.descriptor.cfa;
        im.pixelScale = f.descriptor.pixelScale;
        im.normalized = f.descriptor.normalized;
        normalizeMetadata(im);
        if (im.normalized) {
            for (auto &value : im.pixels)
                value = float(value * im.pixelScale);
            im.pixelScale = 1;
            im.header.remove("SAMPLESCL");
            im.header.remove("SS_SAMPLE_SCALE");
            im.header["BUNIT"] = "relative";
        }
        return im;
    }
    void subtract(Image &im, const Image &m) {
        if (im.normalized != m.normalized)
            throw Error("Ambiguous calibration sample units; set the acquisition sample scale explicitly");
        if (im.samples() != m.samples())
            throw Error("Calibration dimensions differ");
        tbb::parallel_for(size_t(0), im.samples(), [&](size_t i) { im.pixels[i] -= m.pixels[i]; });
    }
    std::shared_ptr<Image> master(const std::vector<Frame> &group, const std::string &kind) {
        if (group.empty())
            return {};
        std::string signature = std::string(algorithm) + kind;
        for (const auto &f : group)
            signature += dependencyKey(f, calibrationKey) + std::to_string(f.id);
        auto key = hash(signature);
        if (auto it = masters.find(key); it != masters.end())
            return it->second;
        auto out = std::make_shared<Image>();
        if (!cacheGet(key, *out)) {
            emit(progress, "calibration", 0, group.size(), "Building " + kind + " master");
            if (group.size() == 1 && group.front().master)
                *out = read(group.front());
            else {
                *out = group.front().descriptor;
                out->pixels.assign(out->samples(), 0);
                const size_t n = out->samples();
                size_t band = std::max<size_t>(
                    1, std::min<uint64_t>(n, settings.memory / 8 /
                                                 (sizeof(float) * std::max<size_t>(group.size(), 16))));
                for (size_t start = 0; start < n; start += band) {
                    checkpoint();
                    size_t count = std::min(band, n - start);
                    std::vector<float> values(count * group.size());
                    for (size_t j = 0; j < group.size(); ++j) {
                        auto im = read(group[j]);
                        if (kind == "flat") {
                            auto df = master(choose(group[j], frames, "darkflat"), "darkflat");
                            auto bias = master(choose(group[j], frames, "bias"), "bias");
                            if (df) {
                                subtract(im, *df);
                                if (df->header["BIASSUB"].toBool()) {
                                    if (!bias)
                                        throw Error("Bias-subtracted dark-flat requires a matching bias");
                                    subtract(im, *bias);
                                }
                            } else if (bias)
                                subtract(im, *bias);
                            else if (!settings.allowUncalibrated)
                                throw Error("Flat has neither matching dark-flat nor bias; resolve "
                                            "calibration or enable uncalibrated processing");
                            double level = median(im.pixels);
                            if (!std::isfinite(level) || level <= 0)
                                throw Error("Flat has nonpositive median");
                            for (auto &v : im.pixels)
                                v = float(v / level);
                        }
                        std::copy_n(im.pixels.begin() + ptrdiff_t(start), count,
                                    values.begin() + ptrdiff_t(j * count));
                        emit(progress, "calibration", j + 1, group.size(), kind);
                    }
                    tbb::parallel_for(size_t(0), count, [&](size_t i) {
                        std::vector<float> v;
                        for (size_t j = 0; j < group.size(); ++j) {
                            float x = values[j * count + i];
                            if (std::isfinite(x))
                                v.push_back(x);
                        }
                        if (v.empty()) {
                            out->pixels[start + i] = NAN;
                            return;
                        }
                        if (v.size() < 10) {
                            out->pixels[start + i] = float(median(std::move(v)));
                            return;
                        }
                        double mean = 0, sigma = INFINITY;
                        for (int pass = 0; pass < 3; ++pass) {
                            double sum = 0, sum2 = 0;
                            size_t used = 0;
                            for (float x : v)
                                if (pass == 0 || std::abs(x - mean) <= 3 * sigma) {
                                    sum += x;
                                    sum2 += double(x) * x;
                                    ++used;
                                }
                            if (used < 2)
                                break;
                            mean = sum / used;
                            sigma = std::sqrt(std::max(0.0, (sum2 - sum * mean) / (used - 1)));
                        }
                        out->pixels[start + i] = float(mean);
                    });
                }
            }
            if (kind == "flat") {
                double norm = median(out->pixels);
                if (norm <= 0)
                    throw Error("Invalid flat normalization");
                for (auto &v : out->pixels)
                    v = std::isfinite(v) && v > norm * 1e-6 ? float(v / norm) : NAN;
            }
            out->pixelScale = 1;
            out->normalized = group.front().descriptor.normalized || kind == "flat";
            out->header.remove("SS_SAMPLE_SCALE");
            out->header.remove("SAMPLESCL");
            if (out->normalized)
                out->header["BUNIT"] = "relative";
            out->header["IMAGETYP"] = QString::fromStdString("Master " + kind);
            out->header["BIASSUB"] = group.front().master && group.front().biasSubtracted;
            cachePut(key, *out);
        }
        uint64_t bytes = out->samples() * 4;
        if (masterBytes + bytes > settings.memory / 8) {
            masters.clear();
            masterBytes = 0;
        }
        if (bytes <= settings.memory / 8) {
            masters[key] = out;
            masterBytes += bytes;
        }
        return out;
    }
    Image processed(const Frame &light) {
        auto key = hash("processed" + dependencyKey(light, calibrationKey));
        Image im;
        if (cacheGet(key, im))
            return im;
        im = read(light);
        auto darkGroup = choose(light, frames, "dark");
        auto flatGroup = choose(light, frames, "flat");
        auto biasGroup = choose(light, frames, "bias");
        if (darkGroup.empty() && biasGroup.empty() && flatGroup.empty() && !settings.allowUncalibrated)
            throw Error("No matching calibration for " + light.path.filename().string() +
                        "; review assignments or explicitly allow uncalibrated processing");
        if (!darkGroup.empty()) {
            auto dark = master(darkGroup, "dark");
            subtract(im, *dark);
            if (darkGroup.front().master && darkGroup.front().biasSubtracted) {
                auto bias = master(biasGroup, "bias");
                if (!bias)
                    throw Error("Bias-subtracted dark requires a matching bias master");
                subtract(im, *bias);
            }
        } else if (auto bias = master(biasGroup, "bias"))
            subtract(im, *bias);
        if (auto flat = master(flatGroup, "flat"))
            tbb::parallel_for(size_t(0), im.samples(), [&](size_t i) {
                im.pixels[i] = std::isfinite(flat->pixels[i]) && flat->pixels[i] > 0
                                   ? im.pixels[i] / flat->pixels[i]
                                   : NAN;
            });
        if (!im.cfa.empty())
            im = debayer(im);
        cachePut(key, im);
        return im;
    }
};
void preflight(const std::vector<Frame> &frames, const Settings &settings) {
    const Frame *first = nullptr;
    for (const auto &f : frames)
        if (f.kind == "light" && f.selection >= 0 && !f.master) {
            if (!f.error.empty() && f.descriptor.width == 0)
                throw Error("Unreadable selected input: " + f.path.string());
            if (uint64_t(f.descriptor.samples()) * 80 > settings.memory)
                throw Error("Memory budget is too small for this frame size; increase it before analysis");
            if (first && first->descriptor.normalized != f.descriptor.normalized)
                throw Error(
                    "Mixed physical and relative sample units; set sample scales explicitly before analysis");
            if (first && (!geometry(*first, f) ||
                          str(first->descriptor.header, "INSTRUME") != str(f.descriptor.header, "INSTRUME")))
                throw Error("A project must use one camera, image size, and CFA mode");
            first = &f;
        }
    for (const auto &f : frames)
        if (f.selection >= 0 && f.kind == "unknown")
            throw Error("Assign a frame type or exclude unknown inputs before processing");
    if (!first)
        throw Error("Project has no included lights");
}
std::unique_ptr<QLockFile> lockProject(const Project &project) {
    auto lock =
        std::make_unique<QLockFile>(QString::fromStdString(project.path().string() + ".processing.lock"));
    lock->setStaleLockTime(0);
    if (!lock->tryLock())
        throw Error("Another process is already processing this project");
    return lock;
}
void identify(Project &project, std::vector<Frame> &frames, const Progress &progress, bool invalidate) {
    size_t done = 0;
    for (auto &f : frames) {
        checkpoint();
        if (f.selection >= 0) {
            auto digest = fingerprint(f.path);
            if (!invalidate && !f.identity.empty() && digest != f.identity)
                throw Error("Input changed; run Analyze again: " + f.path.string());
            f.identity = digest;
            project.save(f);
        }
        emit(progress, "verify", ++done, frames.size(), f.path.filename().string());
    }
}
std::string safeName(const std::string &name) {
    auto s = QString::fromStdString(name);
    for (auto &c : s)
        if (!c.isLetterOrNumber() && c != '-' && c != '_')
            c = '_';
    return s.isEmpty() ? "channel" : s.toStdString();
}
// Each completed band is durably saved before its SQLite progress record. A checksum
// protects checkpoint reuse after crashes or disk corruption. These are private native
// float32 intermediates; tagged byte order prevents reuse on a different architecture.
QByteArray bandBytes(const std::array<Image *, 4> &images, size_t start, size_t n) {
    QByteArray data(qsizetype(n * 16), Qt::Uninitialized);
    for (size_t j = 0; j < images.size(); ++j)
        std::memcpy(data.data() + j * n * 4, images[j]->pixels.data() + start, n * 4);
    return data;
}
bool restoreBand(const fs::path &path, const std::string &key, const std::array<Image *, 4> &images,
                 size_t start, size_t n) {
    QFile in(QString::fromStdString(path.string()));
    if (!in.exists())
        return false;
    try {
        if (!in.open(QIODevice::ReadOnly))
            throw Error("Cannot read band checkpoint");
        auto meta = parseJson(in.readLine(4096));
        if (str(meta, "key") != key || size_t(meta["start"].toInteger()) != start ||
            size_t(meta["count"].toInteger()) != n || meta["byteOrder"].toInt() != QSysInfo::ByteOrder ||
            in.size() - in.pos() != qint64(n * 16))
            throw Error("Invalid band checkpoint");
        auto data = in.readAll();
        if (sha256(data) != meta["sha256"].toString().toLatin1())
            throw Error("Damaged band checkpoint");
        for (size_t j = 0; j < images.size(); ++j)
            std::memcpy(images[j]->pixels.data() + start, data.data() + j * n * 4, n * 4);
        return true;
    } catch (...) {
        checkpoint();
        in.close();
        fs::remove(path); // Derived checkpoint only; immutable inputs are never modified.
        return false;
    }
}
void saveBand(const fs::path &path, const std::string &key, const std::array<Image *, 4> &images,
              size_t start, size_t n) {
    auto data = bandBytes(images, start, n);
    auto meta = json({{"key", QString::fromStdString(key)},
                      {"start", qint64(start)},
                      {"count", qint64(n)},
                      {"byteOrder", QSysInfo::ByteOrder},
                      {"sha256", QString::fromLatin1(sha256(data))}});
    x2f::TemporaryOutput temp(path);
    QFile out(QString::fromStdString(temp.file.string()));
    if (!out.open(QIODevice::WriteOnly) || out.write(meta + '\n') != meta.size() + 1 ||
        out.write(data) != data.size() || !out.flush())
        throw Error("Cannot save band checkpoint; check output disk space");
    out.close();
    x2f::publish(temp.file, path, false);
}
bool verifiedBundle(const QJsonObject &result) {
    if (result["files"].toArray().empty())
        return false;
    for (const auto &v : result["files"].toArray()) {
        auto row = v.toObject();
        auto path = fs::path(str(row, "path"));
        if (!fs::exists(path) || fingerprint(path) != str(row, "sha256"))
            return false;
    }
    return true;
}
void publishBundle(const QJsonObject &pending) {
    for (const auto &v : pending["files"].toArray()) {
        checkpoint();
        auto row = v.toObject();
        auto target = fs::path(str(row, "path")), staged = fs::path(str(row, "staged"));
        auto digest = str(row, "sha256");
        if (fs::exists(target)) {
            if (fingerprint(target) != digest)
                throw Error("Output exists with different contents: " + target.string());
        } else {
            if (!fs::exists(staged) || fingerprint(staged) != digest)
                throw Error("Staged export is missing or damaged: " + staged.string());
            x2f::publish(staged, target, false);
        }
    }
}
} // namespace
QJsonArray calibrationPlan(const std::vector<Frame> &frames) {
    QJsonArray result;
    std::set<std::string> seen;
    for (const auto &f : frames)
        if (f.kind == "light" && f.selection >= 0 && !f.master) {
            QJsonObject grouping{{"session", QString::fromStdString(f.session)},
                                 {"filter", QString::fromStdString(f.filter)},
                                 {"width", f.descriptor.width},
                                 {"height", f.descriptor.height},
                                 {"channels", f.descriptor.channels},
                                 {"cfa", QString::fromStdString(f.descriptor.cfa)}};
            for (const char *key :
                 {"INSTRUME", "XBINNING", "YBINNING", "GAIN", "OFFSET", "READOUTM", "ROWORDER", "EXPTIME",
                  "EXPOSURE", "CCD-TEMP", "SS_BIAS", "SS_DARK", "SS_FLAT"})
                grouping[key] = f.descriptor.header[key];
            auto signature = json(grouping).toStdString();
            if (!seen.insert(signature).second)
                continue;
            QJsonObject row{{"frame", qint64(f.id)},
                            {"session", QString::fromStdString(f.session)},
                            {"filter", QString::fromStdString(f.filter)}};
            try {
                for (const char *kind : {"bias", "dark", "flat"}) {
                    QJsonArray ids;
                    for (const auto &c : choose(f, frames, kind))
                        ids.append(qint64(c.id));
                    row[kind] = ids;
                }
            } catch (const std::exception &e) {
                row["error"] = e.what();
            }
            result.append(row);
        }
    return result;
}
Image previewFrame(Project &project, int64_t id, bool &calibrated) {
    auto frames = project.frames();
    auto found = std::find_if(frames.begin(), frames.end(), [&](const Frame &f) { return f.id == id; });
    if (found == frames.end())
        throw Error("Preview frame no longer exists");
    auto settings = project.settings();
    auto key = dependencyKey(*found, calibrationSignature(frames, settings.allowUncalibrated));
    auto cached = settings.cacheDirectory / ("cache-" + hash("processed" + key) + ".fits");
    calibrated = false;
    if (found->analysisKey == key && fs::exists(cached)) {
        try {
            auto im = readImage(cached, false, settings.memory / 2);
            calibrated = true;
            return im;
        } catch (...) {
            checkpoint();
        }
    }
    auto im = readImage(found->path, false, settings.memory / 2);
    im.header = found->descriptor.header;
    im.cfa = found->descriptor.cfa;
    im.pixelScale = found->descriptor.pixelScale;
    im.normalized = found->descriptor.normalized;
    normalizeMetadata(im);
    // Raw previews keep their stored sample units; RCD requires its own unit conversion below.
    if (im.normalized) {
        for (auto &v : im.pixels)
            v = float(v * im.pixelScale);
        im.pixelScale = 1;
    }
    return im.cfa.empty() ? im : debayer(im);
}
void analyze(Project &project, const Progress &progress) {
    auto lock = lockProject(project);
    auto settings = project.settings();
    auto frames = project.frames();
    preflight(frames, settings);
    tbb::global_control workers(tbb::global_control::max_allowed_parallelism, size_t(settings.threads));
    identify(project, frames, progress, true);
    Engine engine(settings, frames, progress);
    size_t done = 0, total = 0;
    for (auto &f : frames)
        if (f.kind == "light" && !f.master && f.selection >= 0)
            ++total;
    std::mutex saveMutex;
    std::atomic<size_t> measured{0};
    uint64_t perFrame = 0;
    for (const auto &f : frames)
        if (f.kind == "light" && !f.master && f.selection >= 0)
            perFrame = std::max<uint64_t>(perFrame, f.descriptor.samples() * 80);
    const size_t pendingLimit = std::max<size_t>(
        1, std::min<uint64_t>(size_t(settings.threads), settings.memory / std::max<uint64_t>(1, perFrame) > 1
                                                            ? settings.memory / perFrame - 1
                                                            : 1));
    size_t pending = 0;
    tbb::task_group measurements;
    for (auto &f : frames)
        if (f.kind == "light" && !f.master && f.selection >= 0) {
            checkpoint();
            auto key = dependencyKey(f, engine.calibrationKey);
            if (f.analysisKey == key && !f.stars.empty()) {
                emit(progress, "analyze", ++measured, total, f.path.filename().string());
                continue;
            }
            f.error.clear();
            f.transform = {};
            Image image;
            try {
                image =
                    engine.processed(f); // One bounded reader/calibrator; overlap independent measurements.
            } catch (const std::exception &e) {
                checkpoint();
                f.error = e.what();
                f.stars.clear();
                f.analysisKey.clear();
                std::lock_guard guard(saveMutex);
                project.save(f);
                emit(progress, "analyze", ++measured, total, f.path.filename().string());
                continue;
            }
            auto *frame = &f;
            auto owned = std::make_shared<Image>(std::move(image));
            measurements.run([&, frame, owned, key] {
                try {
                    frame->metrics = {};
                    frame->stars = measure(*owned, frame->metrics);
                    frame->analysisKey = key;
                } catch (const std::exception &e) {
                    checkpoint();
                    frame->error = e.what();
                    frame->stars.clear();
                    frame->analysisKey.clear();
                }
                std::lock_guard guard(saveMutex);
                project.save(*frame);
                emit(progress, "analyze", ++measured, total, frame->path.filename().string());
            });
            if (++pending >= pendingLimit) {
                measurements.wait();
                pending = 0;
            }
        }
    measurements.wait();
    Frame *reference = nullptr;
    if (settings.reference) {
        for (auto &f : frames)
            if (f.id == settings.reference && !f.stars.empty() && f.selection >= 0)
                reference = &f;
        if (!reference)
            throw Error("Selected reference is excluded or has no usable stars");
    } else
        for (auto &f : frames)
            if (f.kind == "light" && !f.stars.empty() && f.selection >= 0 &&
                (!reference || f.metrics.fwhm < reference->metrics.fwhm))
                reference = &f;
    if (!reference)
        throw Error("No frames passed analysis; inspect frame errors");
    settings.reference = reference->id;
    project.settings(settings);
    done = 0;
    for (auto &f : frames)
        if (f.kind == "light" && !f.master && f.selection >= 0) {
            checkpoint();
            if (!f.stars.empty()) {
                try {
                    f.transform = f.id == reference->id
                                      ? Transform{}
                                      : registerStars(f.stars, reference->stars, settings.polynomial);
                    if (f.id == reference->id) {
                        f.transform.valid = true;
                        f.transform.rms = 0;
                        f.transform.matches = int(f.stars.size());
                    }
                    f.metrics.residual = f.transform.rms;
                    f.error.clear();
                } catch (const std::exception &e) {
                    checkpoint();
                    f.error = e.what();
                    f.transform.valid = false;
                }
                project.save(f);
            }
            emit(progress, "register", ++done, total, f.path.filename().string());
        }
    // Relative transparency is measured only against the same filter.
    std::map<std::string, Frame *> anchors;
    for (auto &f : frames)
        if (f.transform.valid && f.selection >= 0 && !anchors.contains(f.filter))
            anchors[f.filter] = &f;
    for (auto &f : frames)
        if (f.transform.valid && f.selection >= 0) {
            auto *anchor = anchors.at(f.filter);
            std::vector<float> ratios;
            // Match catalogs in the common reference coordinate system by predicting anchor stars into this
            // frame. Same-filter registration supplies a robust flux ratio without comparing different
            // emission bands.
            if (f.id == anchor->id)
                f.metrics.transparency = 1;
            else {
                try {
                    auto mapping = registerStars(f.stars, anchor->stars, false);
                    for (const auto &star : anchor->stars) {
                        auto xy = mapping.apply(star.x, star.y);
                        const Star *best = nullptr;
                        double distance = 4;
                        for (const auto &s : f.stars) {
                            double d = std::hypot(s.x - xy[0], s.y - xy[1]);
                            if (d < distance) {
                                distance = d;
                                best = &s;
                            }
                        }
                        if (best && star.flux > 0)
                            ratios.push_back(float(best->flux / star.flux));
                    }
                    if (ratios.size() < 6)
                        throw Error("Insufficient photometric matches");
                    f.metrics.transparency = median(ratios);
                } catch (const std::exception &e) {
                    checkpoint();
                    f.metrics.transparency = missing;
                    f.error = std::string("Photometric normalization: ") + e.what();
                }
            }
            project.save(f);
        }
    project.record("analysis", {{"reference", qint64(settings.reference)},
                                {"polynomial", settings.polynomial},
                                {"algorithm", algorithm}});
}
void stack(Project &project, const fs::path &output, const Progress &progress) {
    auto lock = lockProject(project);
    auto settings = project.settings();
    auto frames = project.frames();
    preflight(frames, settings);
    tbb::global_control workers(tbb::global_control::max_allowed_parallelism, size_t(settings.threads));
    auto state = project.record("analysis");
    if (state["reference"].toInteger() != settings.reference ||
        state["polynomial"].toBool() != settings.polynomial || str(state, "algorithm") != algorithm)
        throw Error("Run Analyze with the current reference and registration settings first");
    identify(project, frames, progress, false);
    auto selected = selectedLights(frames, settings);
    if (selected.empty())
        throw Error("No lights remain after grading");
    Frame *reference = nullptr;
    for (auto &f : frames)
        if (f.id == settings.reference)
            reference = &f;
    if (!reference)
        throw Error("Reference is missing");
    const auto calibrationKey = calibrationSignature(frames, settings.allowUncalibrated);
    std::map<std::string, std::vector<Frame>> groups;
    for (const auto &f : selected) {
        if (!f.error.empty() || !f.transform.valid || f.analysisKey != dependencyKey(f, calibrationKey) ||
            !std::isfinite(f.metrics.transparency) || f.metrics.transparency <= 0)
            throw Error("Frame " + std::to_string(f.id) + " needs analysis or exclusion: " + f.error);
        groups[f.filter].push_back(f);
    }
    fs::create_directories(output);
    std::set<std::string> outputNames;
    for (const auto &[filter, group] : groups)
        if (!outputNames.insert(safeName(filter)).second)
            throw Error("Filter names produce the same output filename; rename the filters first");
    Engine engine(settings, frames, progress);
    QJsonArray results;
    for (auto &[filter, group] : groups) {
        auto first = engine.processed(group.front());
        Image master = group.front().descriptor;
        master.header = first.header;
        master.channels = first.channels;
        master.pixelScale = first.pixelScale;
        master.normalized = first.normalized;
        master.cfa = first.cfa;
        first.pixels.clear();
        first.pixels.shrink_to_fit();
        master.width = reference->descriptor.width;
        master.height = reference->descriptor.height;
        master.pixels.assign(master.samples(), NAN);
        Image coverage = master, weights = master, rejected = master;
        coverage.pixels.assign(master.samples(), 0);
        weights.pixels.assign(master.samples(), 0);
        rejected.pixels.assign(master.samples(), 0);
        auto numericalSettings = settings.toJson();
        for (const char *key : {"memory", "scratch", "cacheDirectory", "threads"})
            numericalSettings.remove(key);
        std::string signature = std::string(algorithm) + json(numericalSettings).toStdString() + filter;
        for (const auto &f : group)
            signature += std::to_string(f.id) + f.analysisKey + json(f.transform.toJson()).toStdString() +
                         json(f.metrics.toJson()).toStdString();
        auto runKey = hash(signature);
        fs::path final = output / (safeName(filter) + "-master" + extension(settings.format));
        auto old = project.record("result:" + filter);
        if (str(old, "key") == runKey && str(old, "path") == fs::absolute(final).string() &&
            verifiedBundle(old)) {
            results.append(old);
            emit(progress, "stack", group.size(), group.size(), "Reused verified " + filter + " master");
            continue;
        }
        const auto staging = fs::absolute(output) / (".siderastack-" + runKey);
        auto pending = project.record("pending:" + filter);
        if (str(pending, "key") == runKey && str(pending, "path") == fs::absolute(final).string()) {
            publishBundle(pending);
            project.record("result:" + filter, pending);
            project.record("pending:" + filter, {});
            results.append(pending);
            emit(progress, "export", 1, 1, "Recovered " + filter + " output publication");
            std::error_code ignored;
            fs::remove_all(staging, ignored);
            continue;
        }
        for (const auto &name : {final.filename().string(), safeName(filter) + "-coverage.fits",
                                 safeName(filter) + "-weights.fits", safeName(filter) + "-rejection.fits"})
            if (fs::exists(output / name))
                throw Error("Output already exists; choose a new output directory: " + name);
        fs::create_directories(staging);
        // Checkpoint and staged verified files need storage independently of the cache budget.
        if (fs::space(output).available < master.samples() * 40 + 64 * MiB)
            throw Error("Insufficient output disk space for masters, diagnostics, and recovery checkpoints");
        // A bounded spatial band is shared by both cached and uncached execution.
        // The latter repeats sequential reads; no per-exposure image cube is required.
        const size_t band =
            std::max<size_t>(1, std::min<uint64_t>(master.samples(), settings.memory / 8 / 64));
        // Reserve all accumulator/maps, master storage, a whole-block reader/RCD
        // workspace and headroom before admitting immutable calibrated frames.
        const uint64_t reserve = master.samples() * 16 + band * 64 + settings.memory / 8 +
                                 group.front().descriptor.samples() * 32 + 64 * MiB;
        engine.setHotBudget(settings.memory > reserve ? settings.memory - reserve : 0);
        emit(progress, "resources", 0, group.size(),
             "Using " + std::to_string(settings.threads) + " CPU threads; RAM frame cache limit " +
                 std::to_string(engine.hotBudget / MiB) + " MiB");
        std::array<Image *, 4> bandImages{&master, &coverage, &weights, &rejected};
        for (size_t start = 0; start < master.samples(); start += band) {
            checkpoint();
            size_t n = std::min(band, master.samples() - start);
            auto bandPath = staging / (std::to_string(start) + "-" + std::to_string(n) + ".band");
            if (restoreBand(bandPath, runKey, bandImages, start, n)) {
                emit(progress, "checkpoint", start + n, master.samples(),
                     "Restored verified band " + std::to_string(start / band + 1));
                continue;
            }
            std::vector<double> previousMean(n), previousSigma(n, INFINITY), sum(n), sum2(n), weight(n),
                weighted(n);
            std::vector<uint32_t> count(n), initial(n);
            int passes = settings.rejection && group.size() >= 10 ? settings.iterations + 1 : 1;
            for (int pass = 0; pass < passes; ++pass) {
                checkpoint();
                std::fill(sum.begin(), sum.end(), 0);
                std::fill(sum2.begin(), sum2.end(), 0);
                std::fill(weight.begin(), weight.end(), 0);
                std::fill(weighted.begin(), weighted.end(), 0);
                std::fill(count.begin(), count.end(), 0);
                size_t done = 0;
                for (const auto &f : group) {
                    auto shared = engine.sharedProcessed(f);
                    const auto &im = *shared;
                    double scale = 1 / f.metrics.transparency,
                           offset = group.front().metrics.background - f.metrics.background * scale;
                    double variance = std::pow(f.metrics.noise * scale, 2);
                    double w = variance > 0 ? 1 / variance : 1;
                    tbb::parallel_for(size_t(0), n, [&](size_t i) {
                        size_t pos = start + i, p = pos % master.plane();
                        int c = int(pos / master.plane());
                        auto xy = f.transform.apply(double(p % master.width), double(p / master.width));
                        float raw = interpolate(im, c, xy[0], xy[1]);
                        if (!std::isfinite(raw))
                            return;
                        double v = raw * scale + offset;
                        if (pass && initial[i] >= 10 && previousSigma[i] > 0 &&
                            (v < previousMean[i] - settings.lowSigma * previousSigma[i] ||
                             v > previousMean[i] + settings.highSigma * previousSigma[i]))
                            return;
                        // Welford variance avoids cancellation for bright backgrounds.
                        double delta = v - sum[i];
                        sum[i] += delta / double(count[i] + 1);
                        sum2[i] += delta * (v - sum[i]);
                        weight[i] += w;
                        weighted[i] += v * w;
                        ++count[i];
                    });
                    emit(progress, "stack", ++done, group.size(),
                         filter + " · pass " + std::to_string(pass + 1) + "/" + std::to_string(passes) +
                             " · band " + std::to_string(start / band + 1));
                }
                tbb::parallel_for(size_t(0), n, [&](size_t i) {
                    if (!pass)
                        initial[i] = count[i];
                    if (count[i]) {
                        previousMean[i] = sum[i];
                        previousSigma[i] =
                            count[i] > 1 ? std::sqrt(std::max(0.0, sum2[i] / (count[i] - 1))) : 0;
                    }
                });
            }
            tbb::parallel_for(size_t(0), n, [&](size_t i) {
                master.pixels[start + i] = weight[i] > 0 ? float(weighted[i] / weight[i]) : NAN;
                coverage.pixels[start + i] = float(count[i]);
                weights.pixels[start + i] = float(weight[i]);
                rejected.pixels[start + i] = float(initial[i] - count[i]);
            });
            saveBand(bandPath, runKey, bandImages, start, n);
            project.record("checkpoint", {{"stage", "stack"},
                                          {"key", QString::fromStdString(runKey)},
                                          {"filter", QString::fromStdString(filter)},
                                          {"bandCompleted", qint64(start + n)}});
            emit(progress, "checkpoint", start + n, master.samples(), "Saved integration band");
        }
        // All spatial metadata belongs to the chosen reference, including SIP/PV
        // distortion and row orientation. Copying the complete header also avoids
        // silently mixing WCS conventions from different exposures.
        master.header = reference->descriptor.header;
        master.header.remove("SAMPLESCL");
        if (master.normalized)
            master.header["BUNIT"] = "relative";
        if (master.cfa.empty())
            for (const char *key : {"BAYERPAT", "XBAYROFF", "YBAYROFF", "XBAYEROFF", "YBAYEROFF"})
                master.header.remove(key);
        double exposure = 0;
        for (const auto &f : group)
            exposure += number(f.descriptor.header, "EXPTIME", number(f.descriptor.header, "EXPOSURE", 0));
        master.header["FILTER"] = QString::fromStdString(filter);
        master.header["IMAGETYP"] = "Master Light";
        master.header["NCOMBINE"] = int(group.size());
        master.header["EXPTIME"] = exposure;
        master.header["CREATOR"] = "SideraStack " SIDERASTACK_VERSION;
        master.header["HISTORY"] =
            "Linear weighted average; calibrated before debayering; common reference " +
            QString::number(settings.reference) + "; inverse-variance weights; " +
            (settings.rejection ? "iterative sigma clipping" : "no pixel rejection");
        for (auto it = master.header.begin(); it != master.header.end();) {
            if (it.key().startsWith("SS_"))
                it = master.header.erase(it);
            else
                ++it;
        }
        emit(progress, "export", 0, 1, final.filename().string());
        coverage.header = weights.header = rejected.header = master.header;
        std::array<const Image *, 4> products{&master, &coverage, &weights, &rejected};
        std::array<fs::path, 4> destinations{final, output / (safeName(filter) + "-coverage.fits"),
                                             output / (safeName(filter) + "-weights.fits"),
                                             output / (safeName(filter) + "-rejection.fits")};
        QJsonArray files;
        for (size_t i = 0; i < (settings.diagnostics ? products.size() : 1); ++i) {
            auto staged = staging / destinations[i].filename();
            // A crash before the pending manifest can leave a staged derived file.
            writeImage(staged, *products[i], i == 0 ? settings.format : Format::Fits, true);
            files.append(QJsonObject{{"path", QString::fromStdString(fs::absolute(destinations[i]).string())},
                                     {"staged", QString::fromStdString(staged.string())},
                                     {"sha256", QString::fromStdString(fingerprint(staged))}});
        }
        QJsonObject result{{"key", QString::fromStdString(runKey)},
                           {"path", QString::fromStdString(fs::absolute(final).string())},
                           {"sha256", files[0].toObject()["sha256"]},
                           {"files", files},
                           {"filter", QString::fromStdString(filter)},
                           {"frames", int(group.size())}};
        project.record("pending:" + filter, result);
        emit(progress, "publication", 0, files.size(), "Verified output bundle ready");
        publishBundle(result);
        project.record("result:" + filter, result);
        project.record("pending:" + filter, {});
        results.append(result);
        std::error_code ignored;
        fs::remove_all(staging, ignored);
        emit(progress, "export", 1, 1, final.filename().string());
    }
    project.record("results", {{"masters", results}});
    project.record("checkpoint", {{"stage", "complete"}});
}
void createCalibrationMasters(Project &project, const fs::path &output, const std::string &policy,
                              const std::vector<int64_t> &ids, Format format, const Progress &progress) {
    auto lock = lockProject(project);
    if (!std::set<std::string>{"auto", "bias", "darkflat", "none"}.contains(policy))
        throw Error("Flat calibration must be auto, bias, darkflat, or none");
    auto settings = project.settings();
    auto frames = project.frames();
    tbb::global_control workers(tbb::global_control::max_allowed_parallelism, size_t(settings.threads));
    std::set<int64_t> requested(ids.begin(), ids.end()), found;
    size_t done = 0;
    for (auto &f : frames)
        if (f.kind != "light" && f.kind != "unknown" && f.selection >= 0) {
            if (f.descriptor.samples() * 80 > settings.memory)
                throw Error("Memory budget is too small for calibration frames");
            f.identity = fingerprint(f.path);
            project.save(f);
            emit(progress, "verify", ++done, frames.size(), f.path.filename().string());
        }
    std::map<std::string, std::vector<Frame>> groups;
    std::map<std::string, std::string> labels;
    for (auto &f : frames) {
        if (f.kind == "light" || f.kind == "unknown" || f.selection < 0 ||
            (!requested.empty() && !requested.contains(f.id)) || (requested.empty() && f.master))
            continue;
        found.insert(f.id);
        if (f.kind == "flat" && !f.master) {
            if (policy == "bias") {
                f.descriptor.header["SS_DARKFLAT"] = "none";
                if (choose(f, frames, "bias").empty())
                    throw Error("Bias calibration selected but no matching bias group exists");
            } else if (policy == "darkflat") {
                if (choose(f, frames, "darkflat").empty())
                    throw Error("Dark-flat calibration selected but no matching dark-flat group exists");
            } else if (policy == "none") {
                f.descriptor.header["SS_DARKFLAT"] = "none";
                f.descriptor.header["SS_BIAS"] = "none";
                settings.allowUncalibrated = true;
            }
        }
        QJsonObject key{{"kind", QString::fromStdString(f.kind)},
                        {"session", QString::fromStdString(f.session)},
                        {"width", f.descriptor.width},
                        {"height", f.descriptor.height},
                        {"channels", f.descriptor.channels},
                        {"cfa", QString::fromStdString(f.descriptor.cfa)},
                        {"master", f.master},
                        {"biasSubtracted", f.biasSubtracted}};
        if (f.master)
            key["id"] = qint64(f.id);
        if (f.kind == "flat")
            key["filter"] = QString::fromStdString(f.filter);
        for (const char *name : {"INSTRUME", "XBINNING", "YBINNING", "GAIN", "OFFSET", "READOUTM", "ROWORDER",
                                 "EXPTIME", "EXPOSURE", "CCD-TEMP", "SS_BIAS", "SS_DARKFLAT"})
            key[name] = f.descriptor.header[name];
        auto groupKey = hash(json(key).toStdString());
        groups[groupKey].push_back(f);
        labels[groupKey] =
            f.kind + (f.kind == "flat" ? "-" + f.filter : "") + "-" + f.session + "-" + groupKey.substr(0, 8);
    }
    if (!requested.empty() && found != requested)
        throw Error("Master selection includes absent, excluded, or non-calibration frames");
    if (groups.empty())
        throw Error("No included raw calibration frames; import or select calibrations first");
    fs::create_directories(output);
    Engine engine(settings, frames, progress);
    QJsonArray results;
    for (const auto &[groupKey, group] : groups) {
        auto destination =
            fs::absolute(output / (safeName(labels.at(groupKey)) + "-master" + extension(format)));
        std::string signature = std::string(algorithm) + groupKey + policy + std::to_string(int(format));
        for (const auto &f : group)
            signature += dependencyKey(f, engine.calibrationKey);
        auto runKey = hash(signature);
        auto recordKey = "calibration-result:" + groupKey;
        auto old = project.record(recordKey);
        if (str(old, "key") == runKey && str(old, "path") == destination.string() && verifiedBundle(old)) {
            results.append(old);
            emit(progress, "export", 1, 1, "Reused " + destination.filename().string());
            continue;
        }
        auto pendingKey = "calibration-pending:" + groupKey;
        auto pending = project.record(pendingKey);
        if (str(pending, "key") == runKey && str(pending, "path") == destination.string()) {
            publishBundle(pending);
            project.record(recordKey, pending);
            project.record(pendingKey, {});
            results.append(pending);
            std::error_code ignored;
            fs::remove_all(fs::absolute(output) / (".siderastack-calibration-" + runKey), ignored);
            continue;
        }
        if (fs::exists(destination))
            throw Error("Calibration output already exists; choose a new directory");
        if (fs::space(output).available < group.front().descriptor.samples() * 16 + 64 * MiB)
            throw Error("Insufficient output disk space for calibration master");
        auto product = engine.master(group, group.front().kind);
        auto im = *product;
        im.header["NCOMBINE"] = int(group.size());
        im.header["CREATOR"] = "SideraStack " SIDERASTACK_VERSION;
        if (group.front().kind == "flat") {
            im.header["CALMETH"] = QString::fromStdString(policy);
            im.header["HISTORY"] =
                "Normalized master flat; selected calibration policy " + QString::fromStdString(policy);
        }
        for (auto it = im.header.begin(); it != im.header.end();)
            if (it.key().startsWith("SS_"))
                it = im.header.erase(it);
            else
                ++it;
        const auto staging = fs::absolute(output) / (".siderastack-calibration-" + runKey);
        fs::create_directories(staging);
        auto staged = staging / ("calibration" + extension(format));
        // The named staged file and manifest survive a cancelled publication.
        writeImage(staged, im, format, true);
        QJsonArray files{QJsonObject{{"path", QString::fromStdString(destination.string())},
                                     {"staged", QString::fromStdString(staged.string())},
                                     {"sha256", QString::fromStdString(fingerprint(staged))}}};
        QJsonObject result{{"key", QString::fromStdString(runKey)},
                           {"path", QString::fromStdString(destination.string())},
                           {"sha256", files[0].toObject()["sha256"]},
                           {"files", files},
                           {"kind", QString::fromStdString(group.front().kind)},
                           {"frames", int(group.size())}};
        project.record(pendingKey, result);
        publishBundle(result);
        project.record(recordKey, result);
        project.record(pendingKey, {});
        results.append(result);
        std::error_code ignored;
        fs::remove_all(staging, ignored);
        emit(progress, "export", 1, 1, destination.filename().string());
    }
    project.record("calibration-results", {{"masters", results}});
}
void exportMasters(Project &project, const fs::path &output, Format format, const Progress &progress) {
    auto lock = lockProject(project);
    auto results = project.record("results")["masters"].toArray();
    if (results.empty())
        throw Error("No completed masters to export");
    fs::create_directories(output);
    size_t done = 0;
    for (const auto &value : results) {
        auto row = value.toObject();
        auto path = fs::path(str(row, "path"));
        if (fingerprint(path) != str(row, "sha256"))
            throw Error("Completed master changed since integration");
        auto im = readImage(path, false, project.settings().memory / 2);
        auto destination = output / (safeName(str(row, "filter")) + "-master" + extension(format));
        writeImage(destination, im, format);
        emit(progress, "export", ++done, results.size(), destination.filename().string());
    }
}
} // namespace ss

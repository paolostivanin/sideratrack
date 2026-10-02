#include "stellastack/core.hpp"
#include <QDateTime>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sqlite3.h>
#include <sstream>
#include <thread>

namespace ss {
QJsonObject Metrics::toJson() const {
    QJsonObject o;
    put(o, "fwhm", fwhm);
    put(o, "hfr", hfr);
    put(o, "eccentricity", eccentricity);
    put(o, "background", background);
    put(o, "noise", noise);
    put(o, "transparency", transparency);
    put(o, "residual", residual);
    o["stars"] = stars;
    return o;
}
Metrics Metrics::fromJson(const QJsonObject &o) {
    Metrics m;
    m.fwhm = number(o, "fwhm");
    m.hfr = number(o, "hfr");
    m.eccentricity = number(o, "eccentricity");
    m.background = number(o, "background");
    m.noise = number(o, "noise");
    m.transparency = number(o, "transparency");
    m.residual = number(o, "residual");
    m.stars = o["stars"].toInt();
    return m;
}
std::array<double, 2> Transform::apply(double x, double y) const {
    return {v[0] + v[1] * x + v[2] * y + v[3] * x * x + v[4] * x * y + v[5] * y * y,
            v[6] + v[7] * x + v[8] * y + v[9] * x * x + v[10] * x * y + v[11] * y * y};
}
QJsonObject Transform::toJson() const {
    QJsonArray a;
    for (double d : v)
        a.append(d);
    QJsonObject o{{"v", a}, {"valid", valid}, {"matches", matches}};
    put(o, "rms", rms);
    return o;
}
Transform Transform::fromJson(const QJsonObject &o) {
    Transform t;
    auto a = o["v"].toArray();
    if (a.size() != 12)
        return t;
    for (int i = 0; i < 12; ++i) {
        if (!a[i].isDouble() || !std::isfinite(a[i].toDouble()))
            throw Error("Invalid saved transform");
        t.v[i] = a[i].toDouble();
    }
    t.valid = o["valid"].toBool();
    t.rms = number(o, "rms");
    t.matches = o["matches"].toInt();
    return t;
}
QJsonObject Frame::toJson(bool catalog) const {
    QJsonObject o{{"id", qint64(id)},
                  {"path", QString::fromStdString(path.string())},
                  {"identity", QString::fromStdString(identity)},
                  {"kind", QString::fromStdString(kind)},
                  {"filter", QString::fromStdString(filter)},
                  {"session", QString::fromStdString(session)},
                  {"master", master},
                  {"biasSubtracted", biasSubtracted},
                  {"selection", selection},
                  {"error", QString::fromStdString(error)},
                  {"analysisKey", QString::fromStdString(analysisKey)},
                  {"calibrationKey", QString::fromStdString(calibrationKey)},
                  {"calibratedPath", QString::fromStdString(calibratedPath.string())},
                  {"metadataSources", metadataSources},
                  {"width", descriptor.width},
                  {"height", descriptor.height},
                  {"channels", descriptor.channels},
                  {"cfa", QString::fromStdString(descriptor.cfa)},
                  {"pixelScale", descriptor.pixelScale},
                  {"normalized", descriptor.normalized},
                  {"header", descriptor.header},
                  {"metrics", metrics.toJson()},
                  {"transform", transform.toJson()}};
    if (catalog) {
        QJsonArray a;
        for (const auto &s : stars)
            a.append(QJsonArray{s.x, s.y, s.flux, s.fwhm, s.hfr, s.eccentricity});
        o["catalog"] = a;
    }
    return o;
}
Frame Frame::fromJson(const QJsonObject &o) {
    Frame f;
    f.id = o["id"].toInteger();
    f.path = str(o, "path");
    f.identity = str(o, "identity");
    f.kind = str(o, "kind", "unknown");
    f.filter = str(o, "filter");
    f.session = str(o, "session");
    f.master = o["master"].toBool();
    f.biasSubtracted = o["biasSubtracted"].toBool();
    f.selection = o["selection"].toInt();
    f.error = str(o, "error");
    f.analysisKey = str(o, "analysisKey");
    f.calibrationKey = str(o, "calibrationKey");
    f.calibratedPath = str(o, "calibratedPath");
    f.metadataSources = o["metadataSources"].toObject();
    f.descriptor.width = o["width"].toInt();
    f.descriptor.height = o["height"].toInt();
    f.descriptor.channels = o["channels"].toInt(1);
    f.descriptor.cfa = str(o, "cfa");
    f.descriptor.pixelScale = number(o, "pixelScale", 1);
    f.descriptor.normalized = o["normalized"].toBool();
    f.descriptor.header = o["header"].toObject();
    f.metrics = Metrics::fromJson(o["metrics"].toObject());
    f.transform = Transform::fromJson(o["transform"].toObject());
    for (const auto &v : o["catalog"].toArray()) {
        auto a = v.toArray();
        if (a.size() != 6)
            throw Error("Invalid saved star catalog");
        f.stars.push_back({a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble(missing),
                           a[4].toDouble(missing), a[5].toDouble(missing)});
    }
    return f;
}
void editFrame(Frame &frame, const QJsonObject &changes) {
    auto o = frame.toJson();
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        if (it.key() == "header") {
            if (!it.value().isObject())
                throw Error("Metadata overrides must be an object");
            auto h = frame.descriptor.header;
            const auto updates = it.value().toObject();
            for (auto field = updates.begin(); field != updates.end(); ++field) {
                if (field.value().isNull())
                    h.remove(field.key());
                else if (field.value().isString() || field.value().isBool() || field.value().isDouble())
                    h[field.key()] = field.value();
                else
                    throw Error("Metadata values must be text, numbers, logicals, or null");
            }
            o["header"] = h;
            auto sources = o["metadataSources"].toObject();
            for (auto field = updates.begin(); field != updates.end(); ++field)
                sources[field.key()] = "manual";
            o["metadataSources"] = sources;
        } else if (it.key() == "kind" || it.key() == "filter" || it.key() == "session") {
            if (!it.value().isString() || it.value().toString().trimmed().isEmpty())
                throw Error("Frame type, filter, and night must be nonempty text");
            o[it.key()] = it.value().toString().trimmed();
        } else if (it.key() == "master" || it.key() == "biasSubtracted") {
            if (!it.value().isBool())
                throw Error("Calibration state must be a boolean");
            o[it.key()] = it.value();
        } else if (it.key() == "selection") {
            if (!it.value().isDouble() ||
                (it.value().toDouble() != -1 && it.value().toDouble() != 0 && it.value().toDouble() != 1))
                throw Error("Selection must be -1 (exclude), 0 (automatic), or 1 (include)");
            o[it.key()] = it.value();
        } else
            throw Error("Unsupported editable field: " + it.key().toStdString());
    }
    auto updated = Frame::fromJson(o);
    if (!std::set<std::string>{"unknown", "light", "dark", "darkflat", "flat", "bias"}.contains(updated.kind))
        throw Error("Unknown frame type");
    normalizeMetadata(updated.descriptor);
    frame = std::move(updated);
}
void inferMetadata(Frame &frame) {
    const auto name = QString::fromStdString(frame.path.stem().string());
    struct Token {
        const char *token, *key;
    };
    for (const auto &[token, key] :
         {Token{"GAIN", "GAIN"}, Token{"TEMP", "CCD-TEMP"}, Token{"EXPOSURE", "EXPTIME"}}) {
        if (frame.descriptor.header.contains(key) || frame.metadataSources[key] == "manual" ||
            (std::string(key) == "EXPTIME" && frame.descriptor.header.contains("EXPOSURE")))
            continue;
        QRegularExpression pattern("(?:^|__)" + QString(token) +
                                       "_([+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+))" +
                                       (std::string(token) == "EXPOSURE" ? "s" : "") + "(?=__|$)",
                                   QRegularExpression::CaseInsensitiveOption);
        auto match = pattern.match(name);
        if (!match.hasMatch())
            continue;
        // Conflicting repeated named tokens cannot establish a value.
        if (pattern.match(name, match.capturedEnd()).hasMatch())
            continue;
        bool ok = false;
        auto value = match.captured(1).toDouble(&ok);
        if (!ok || !std::isfinite(value) || (std::string(token) == "EXPOSURE" && value < 0))
            continue;
        frame.descriptor.header[key] = value;
        frame.metadataSources[key] = "filename";
    }
}
QJsonObject Settings::toJson() const {
    return {{"memory", qint64(memory)},
            {"scratch", qint64(scratch)},
            {"cacheDirectory", QString::fromStdString(cacheDirectory.string())},
            {"threads", threads},
            {"iterations", iterations},
            {"reference", qint64(reference)},
            {"lowSigma", lowSigma},
            {"highSigma", highSigma},
            {"maxFwhm", maxFwhm},
            {"maxEccentricity", maxEccentricity},
            {"keepPercent", keepPercent},
            {"rejection", rejection},
            {"polynomial", polynomial},
            {"allowUncalibrated", allowUncalibrated},
            {"diagnostics", diagnostics},
            {"format", int(format)}};
}
Settings Settings::fromJson(const QJsonObject &o) {
    Settings s;
    s.memory = uint64_t(o["memory"].toInteger());
    s.scratch = uint64_t(o["scratch"].toInteger());
    s.cacheDirectory = str(o, "cacheDirectory");
    s.threads = o["threads"].toInt();
    s.iterations = o["iterations"].toInt(3);
    s.reference = o["reference"].toInteger();
    s.lowSigma = number(o, "lowSigma", 3);
    s.highSigma = number(o, "highSigma", 3);
    s.maxFwhm = number(o, "maxFwhm", 0);
    s.maxEccentricity = number(o, "maxEccentricity", 0);
    s.keepPercent = number(o, "keepPercent", 100);
    s.rejection = o["rejection"].toBool(true);
    s.polynomial = o["polynomial"].toBool();
    s.allowUncalibrated = o["allowUncalibrated"].toBool();
    s.diagnostics = o["diagnostics"].toBool(true);
    s.format = Format(o["format"].toInt());
    s.validate();
    return s;
}
Settings Settings::defaults(const fs::path &project) {
    Settings s;
    uint64_t available = 2 * GiB;
    std::ifstream mem("/proc/meminfo");
    std::string line;
    while (std::getline(mem, line))
        if (line.rfind("MemAvailable:", 0) == 0) {
            std::istringstream in(line.substr(13));
            in >> available;
            available *= 1024;
            break;
        }
    s.memory = available * 6 / 10;
    s.threads = int(std::max(1u, std::thread::hardware_concurrency()));
    auto root = fs::path(QStandardPaths::writableLocation(QStandardPaths::CacheLocation).toStdString());
    if (root.empty())
        root = project.parent_path();
    fs::create_directories(root);
    s.cacheDirectory = root / hash(fs::absolute(project).string()).substr(0, 16);
    s.scratch = std::min<uint64_t>(100 * GiB, fs::space(root).available / 4);
    return s;
}
void Settings::validate() const {
    if (memory < 64 * MiB || memory > uint64_t(INT64_MAX) || scratch > uint64_t(INT64_MAX))
        throw Error("Memory budget must be at least 64 MiB; budgets must fit signed 64 bits");
    if (threads < 1 || threads > 1024 || iterations < 1 || iterations > 10 || lowSigma <= 0 ||
        highSigma <= 0 || !std::isfinite(lowSigma) || !std::isfinite(highSigma) || keepPercent <= 0 ||
        keepPercent > 100 || !std::isfinite(keepPercent) || maxFwhm < 0 || maxEccentricity < 0 ||
        maxEccentricity > 1 || !std::isfinite(maxFwhm) || !std::isfinite(maxEccentricity) ||
        cacheDirectory.empty() || int(format) < 0 || int(format) > 3)
        throw Error("Invalid processing settings");
}
namespace {
class Statement {
  public:
    sqlite3_stmt *p = nullptr;
    sqlite3 *db;
    Statement(sqlite3 *d, const char *sql) : db(d) {
        if (sqlite3_prepare_v2(db, sql, -1, &p, nullptr) != SQLITE_OK)
            throw Error(sqlite3_errmsg(db));
    }
    ~Statement() {
        sqlite3_finalize(p);
    }
    void text(int i, const std::string &s) {
        if (sqlite3_bind_text(p, i, s.c_str(), int(s.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw Error(sqlite3_errmsg(db));
    }
    int step() {
        int r = sqlite3_step(p);
        if (r != SQLITE_ROW && r != SQLITE_DONE)
            throw Error(sqlite3_errmsg(db));
        return r;
    }
    QByteArray bytes(int i) {
        auto pstr = reinterpret_cast<const char *>(sqlite3_column_text(p, i));
        return QByteArray(pstr, sqlite3_column_bytes(p, i));
    }
};
std::string kindOf(const Image &im) {
    auto v = QString::fromStdString(str(im.header, "IMAGETYP", str(im.header, "FRAME"))).toLower();
    if (v.contains("dark") && v.contains("flat"))
        return "darkflat";
    if (v.contains("bias") || v.contains("offset"))
        return "bias";
    if (v.contains("dark"))
        return "dark";
    if (v.contains("flat"))
        return "flat";
    if (v.contains("light") || v.contains("object"))
        return "light";
    return "unknown";
}
} // namespace
void Project::exec(const std::string &sql) const {
    char *error = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        std::string message = error ? error : "SQLite failure";
        sqlite3_free(error);
        throw Error(message);
    }
}
Project::Project(const fs::path &path, bool create) : path_(fs::absolute(path)) {
    Settings initial;
    if (create)
        initial = Settings::defaults(path_);
    if (!create && !fs::exists(path_))
        throw Error("Project does not exist: " + path_.string());
    if (sqlite3_open_v2(path_.c_str(), &db_, SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0),
                        nullptr) != SQLITE_OK) {
        std::string error = db_ ? sqlite3_errmsg(db_) : "Cannot open project";
        sqlite3_close(db_);
        db_ = nullptr;
        throw Error(error);
    }
    try {
        sqlite3_busy_timeout(db_, 10000);
        exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;");
        Statement version(db_, "PRAGMA user_version");
        version.step();
        int v = sqlite3_column_int(version.p, 0);
        if (v > 1)
            throw Error("This project requires a newer Stellastack version");
        if (v == 0) {
            if (!create)
                throw Error("Not a Stellastack project");
            exec(
                "CREATE TABLE frames(id INTEGER PRIMARY KEY,path TEXT NOT NULL UNIQUE,record TEXT NOT NULL); "
                "CREATE TABLE records(key TEXT PRIMARY KEY,value TEXT NOT NULL); PRAGMA user_version=1;");
            settings(initial);
        }
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}
Project::~Project() {
    sqlite3_close(db_);
}
std::vector<Frame> Project::frames() const {
    Statement q(db_, "SELECT id,record FROM frames ORDER BY id");
    std::vector<Frame> v;
    while (q.step() == SQLITE_ROW) {
        auto f = Frame::fromJson(parseJson(q.bytes(1)));
        f.id = sqlite3_column_int64(q.p, 0);
        v.push_back(std::move(f));
    }
    return v;
}
std::vector<Frame> Project::calibrationFrames() const {
    Statement query(db_, "SELECT id,record FROM frames WHERE json_extract(record,'$.kind') "
                         "IN ('bias','dark','flat','darkflat') ORDER BY id");
    std::vector<Frame> result;
    while (query.step() == SQLITE_ROW) {
        auto frame = Frame::fromJson(parseJson(query.bytes(1)));
        frame.id = sqlite3_column_int64(query.p, 0);
        result.push_back(std::move(frame));
    }
    return result;
}
std::optional<Frame> Project::frame(int64_t id) const {
    Statement q(db_, "SELECT record FROM frames WHERE id=?");
    sqlite3_bind_int64(q.p, 1, id);
    if (q.step() != SQLITE_ROW)
        return {};
    auto result = Frame::fromJson(parseJson(q.bytes(0)));
    result.id = id;
    return result;
}
void Project::save(Frame &f) {
    const auto bytes = json(f.toJson()).toStdString();
    if (f.id) {
        Statement q(db_, "UPDATE frames SET record=? WHERE id=?");
        q.text(1, bytes);
        sqlite3_bind_int64(q.p, 2, f.id);
        q.step();
    } else {
        Statement q(db_, "INSERT INTO frames(path,record) VALUES(?,?)");
        q.text(1, f.path.string());
        q.text(2, bytes);
        q.step();
        f.id = sqlite3_last_insert_rowid(db_);
    }
    if (inTransaction_)
        changedFrames_.insert(f.id);
    else if (frameChanged)
        frameChanged(f.id);
}
void Project::record(const std::string &key, const QJsonObject &value) {
    Statement q(
        db_,
        "INSERT INTO records(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    q.text(1, key);
    q.text(2, json(value).toStdString());
    q.step();
}
QJsonObject Project::record(const std::string &key) const {
    Statement q(db_, "SELECT value FROM records WHERE key=?");
    q.text(1, key);
    return q.step() == SQLITE_ROW ? parseJson(q.bytes(0)) : QJsonObject{};
}
Settings Project::settings() const {
    return Settings::fromJson(record("settings"));
}
void Project::settings(const Settings &s) {
    s.validate();
    record("settings", s.toJson());
}
void Project::transaction(const std::function<void()> &fn) {
    exec("BEGIN IMMEDIATE");
    inTransaction_ = true;
    changedFrames_.clear();
    try {
        fn();
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        inTransaction_ = false;
        changedFrames_.clear();
        throw;
    }
    inTransaction_ = false;
    auto changed = std::move(changedFrames_);
    changedFrames_.clear();
    if (frameChanged)
        for (auto id : changed)
            frameChanged(id);
}
void importFiles(Project &project, const std::vector<fs::path> &roots, const Progress &progress) {
    std::set<fs::path> paths, existing;
    for (const auto &f : project.frames())
        existing.insert(f.path);
    auto add = [&](const fs::path &p) {
        auto name = QString::fromStdString(p.filename().string()).toLower();
        if (name.endsWith(".fit") || name.endsWith(".fits") || name.endsWith(".fts") ||
            name.endsWith(".fits.fz") || name.endsWith(".fit.fz") || name.endsWith(".xisf"))
            paths.insert(fs::canonical(p));
    };
    for (const auto &root : roots) {
        checkpoint();
        if (fs::is_directory(root)) {
            for (const auto &item : fs::recursive_directory_iterator(root)) {
                checkpoint();
                if (item.is_regular_file())
                    add(item.path());
            }
        } else
            add(root);
    }
    size_t done = 0;
    for (const auto &path : paths) {
        checkpoint();
        if (!existing.contains(path)) {
            Frame f;
            f.path = path;
            try {
                f.descriptor = readImage(path, true);
                if (f.descriptor.header.contains("SSPREP")) {
                    if (progress)
                        progress("import", ++done, paths.size(),
                                 "Skipping managed prepared image " + path.filename().string());
                    continue;
                }
                f.kind = kindOf(f.descriptor);
                f.filter = str(f.descriptor.header, "FILTER",
                               f.descriptor.channels == 3 || !f.descriptor.cfa.empty() ? "OSC" : "Unknown");
                f.session = str(f.descriptor.header, "DATE-OBS", path.parent_path().filename().string())
                                .substr(0, 10);
                f.master = QString::fromStdString(str(f.descriptor.header, "IMAGETYP"))
                               .contains("master", Qt::CaseInsensitive);
                f.biasSubtracted = f.descriptor.header["BIASSUB"].toBool();
                inferMetadata(f);
            } catch (const std::exception &e) {
                f.error = e.what();
            }
            project.save(f);
        }
        if (progress)
            progress("import", ++done, paths.size(), path.filename().string());
    }
}
std::vector<SelectionDecision> evaluateSelection(const std::vector<Frame> &frames, const Settings &s) {
    std::vector<SelectionDecision> decisions;
    std::map<std::string, std::vector<size_t>> groups;
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto &f = frames[i];
        SelectionDecision decision{f.id, false, "Calibration frame"};
        if (f.selection < 0)
            decision.reason = "Manually excluded";
        else if (f.kind != "light" || f.master)
            decision.included = true;
        else if (f.selection == 0 && s.maxFwhm > 0 &&
                 (!std::isfinite(f.metrics.fwhm) || f.metrics.fwhm > s.maxFwhm))
            decision.reason = "Excluded by FWHM limit";
        else if (f.selection == 0 && s.maxEccentricity > 0 &&
                 (!std::isfinite(f.metrics.eccentricity) || f.metrics.eccentricity > s.maxEccentricity))
            decision.reason = "Excluded by eccentricity limit";
        else
            groups[f.filter].push_back(i);
        decisions.push_back(std::move(decision));
    }
    for (auto &[filter, group] : groups) {
        std::stable_sort(group.begin(), group.end(), [&](size_t a, size_t b) {
            auto x = frames[a].metrics.fwhm, y = frames[b].metrics.fwhm;
            return (std::isfinite(x) ? x : INFINITY) < (std::isfinite(y) ? y : INFINITY);
        });
        size_t keep = size_t(std::ceil(group.size() * s.keepPercent / 100));
        for (size_t i = 0; i < group.size(); ++i) {
            auto &decision = decisions[group[i]];
            decision.included = i < keep || frames[group[i]].selection > 0;
            decision.reason = frames[group[i]].selection > 0 ? "Manually included"
                              : decision.included            ? "Included automatically"
                                                             : "Excluded by best-FWHM percentage";
        }
    }
    return decisions;
}
std::vector<Frame> selectedLights(const std::vector<Frame> &frames, const Settings &s) {
    auto decisions = evaluateSelection(frames, s);
    std::vector<Frame> result;
    for (size_t i = 0; i < frames.size(); ++i)
        if (decisions[i].included && frames[i].kind == "light" && !frames[i].master)
            result.push_back(frames[i]);
    std::sort(result.begin(), result.end(), [](const Frame &a, const Frame &b) { return a.id < b.id; });
    return result;
}
} // namespace ss

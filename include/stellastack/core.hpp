#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct sqlite3;
namespace ss {
namespace fs = std::filesystem;
inline constexpr double missing = std::numeric_limits<double>::quiet_NaN();
inline constexpr uint64_t MiB = 1024ULL * 1024, GiB = MiB * 1024;
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
void checkpoint();
void cancel();
void clearCancellation();
std::string hash(const std::string &);
std::string fingerprint(const fs::path &);
QByteArray sha256(const QByteArray &);
QByteArray json(const QJsonObject &);
QJsonObject parseJson(const QByteArray &);
std::string str(const QJsonObject &, const char *key, const std::string &fallback = {});
double number(const QJsonObject &, const char *key, double fallback = missing);
void put(QJsonObject &, const char *, double);

struct Image {
    int width = 0, height = 0, channels = 1;
    std::string cfa;
    double pixelScale = 1;   // Multiplier from stored samples to nominal relative units.
    bool normalized = false; // True when a nominal range is explicitly known.
    QJsonObject header;
    std::vector<float> pixels; // Planar, physical sample values; NaN means invalid.
    size_t plane() const {
        return size_t(width) * size_t(height);
    }
    size_t samples() const {
        return plane() * size_t(channels);
    }
};
enum class Format { Fits, FitsCompressed, Xisf, XisfCompressed };
Format parseFormat(const std::string &);
std::string extension(Format);
Image readImage(const fs::path &, bool headerOnly = false, uint64_t budget = 4 * GiB);
void writeImage(const fs::path &, const Image &, Format, bool overwrite = false);
Image luminance(const Image &);
void normalizeMetadata(Image &);

struct Star {
    double x = 0, y = 0, flux = 0, fwhm = missing, hfr = missing, eccentricity = missing;
};
struct Metrics {
    double fwhm = missing, hfr = missing, eccentricity = missing, background = missing;
    double noise = missing, transparency = missing, residual = missing;
    int stars = 0;
    QJsonObject toJson() const;
    static Metrics fromJson(const QJsonObject &);
};
struct Transform {
    // Map reference/output (x,y) to source coordinates; polynomial terms 1,x,y,x²,xy,y².
    std::array<double, 12> v{0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0};
    bool valid = false;
    double rms = missing;
    int matches = 0;
    std::array<double, 2> apply(double x, double y) const;
    QJsonObject toJson() const;
    static Transform fromJson(const QJsonObject &);
};
struct Frame {
    int64_t id = 0;
    fs::path path;
    std::string identity, kind = "unknown", filter, session, error, analysisKey;
    std::string calibrationKey;
    fs::path calibratedPath;
    QJsonObject metadataSources;
    bool master = false, biasSubtracted = false;
    int selection = 0; // -1 excluded, 0 automatic, +1 manually included.
    Image descriptor;
    Metrics metrics;
    Transform transform;
    std::vector<Star> stars;
    QJsonObject toJson(bool catalog = true) const;
    static Frame fromJson(const QJsonObject &);
};
void editFrame(Frame &, const QJsonObject &);
void inferMetadata(Frame &);
struct Settings {
    uint64_t memory = 0, scratch = 0;
    fs::path cacheDirectory;
    int threads = 0, iterations = 3;
    int64_t reference = 0;
    double lowSigma = 3, highSigma = 3, maxFwhm = 0, maxEccentricity = 0, keepPercent = 100;
    bool rejection = true, polynomial = false, allowUncalibrated = false, diagnostics = true;
    Format format = Format::Fits;
    QJsonObject toJson() const;
    static Settings fromJson(const QJsonObject &);
    static Settings defaults(const fs::path &);
    void validate() const;
};
class Project {
  public:
    explicit Project(const fs::path &path, bool create = false);
    ~Project();
    Project(const Project &) = delete;
    Project &operator=(const Project &) = delete;
    std::vector<Frame> frames() const;
    std::vector<Frame> calibrationFrames() const;
    std::optional<Frame> frame(int64_t id) const;
    std::function<void(int64_t)> frameChanged;
    void save(Frame &);
    Settings settings() const;
    void settings(const Settings &);
    void record(const std::string &key, const QJsonObject &);
    QJsonObject record(const std::string &key) const;
    fs::path path() const {
        return path_;
    }
    void transaction(const std::function<void()> &);

  private:
    sqlite3 *db_ = nullptr;
    fs::path path_;
    bool inTransaction_ = false;
    std::set<int64_t> changedFrames_;
    void exec(const std::string &) const;
};
using Progress = std::function<void(const std::string &, size_t, size_t, const std::string &)>;
void importFiles(Project &, const std::vector<fs::path> &, const Progress & = {});
std::vector<Star> measure(const Image &, Metrics &);
Transform registerStars(const std::vector<Star> &source, const std::vector<Star> &reference, bool polynomial);
Image debayer(const Image &);
float interpolate(const Image &, int channel, double x, double y);
std::vector<Frame> selectedLights(const std::vector<Frame> &, const Settings &);
struct SelectionDecision {
    int64_t id = 0;
    bool included = false;
    std::string reason;
};
// One decision per input, in input order. Does not copy image/star data.
std::vector<SelectionDecision> evaluateSelection(const std::vector<Frame> &, const Settings &);
struct WorkflowReport {
    bool canPrepare = false, prepared = false, analyzed = false, canStack = false, resultsCurrent = false,
         resumable = false;
    int lights = 0, unknown = 0;
    std::vector<std::string> prepareBlockers, stackBlockers, warnings;
    std::map<int64_t, std::string> frameProblems;
    QJsonArray groups, assignments;
};
// Metadata-only advisory report. Workers still verify fingerprints and outputs.
WorkflowReport workflowReport(const Project &, const std::vector<Frame> &, const Settings &);
QJsonArray calibrationPlan(const std::vector<Frame> &);
std::string calibrationRevision(const std::vector<Frame> &, const Settings &);
bool preparationReady(const std::vector<Frame> &, const Settings &);
uint64_t previewDiskBudget(const Settings &);
void calibrate(Project &, const fs::path &directory = {}, const Progress & = {});
Image previewFrame(Project &, int64_t id, bool &calibrated);
void analyze(Project &, const Progress & = {});
void stack(Project &, const fs::path &outputDirectory, const Progress & = {});
void exportMasters(Project &, const fs::path &outputDirectory, Format, const Progress & = {});
void createCalibrationMasters(Project &, const fs::path &, const std::string &flatCalibration,
                              const std::vector<int64_t> &ids, Format, const Progress & = {});
} // namespace ss

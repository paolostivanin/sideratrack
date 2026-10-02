#include "stellastack/core.hpp"
#include <Eigen/Dense>
#include <rtprocess/librtprocess.h>
#include <tbb/parallel_for.h>
extern "C" {
#include <sep.h>
}
#include <algorithm>
#include <map>
#include <mutex>
#include <numbers>
#include <numeric>
#include <set>
#include <unordered_map>

namespace ss {
namespace {
double median(std::vector<double> v) {
    if (v.empty())
        return missing;
    size_t i = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + ptrdiff_t(i), v.end());
    double m = v[i];
    if (!(v.size() & 1))
        m = (m + *std::max_element(v.begin(), v.begin() + ptrdiff_t(i))) / 2;
    return m;
}
void sepCheck(int s) {
    if (s) {
        char msg[512]{};
        sep_get_errmsg(s, msg);
        throw Error(std::string("Star extraction: ") + msg);
    }
}
#ifndef STELLASTACK_THREADSAFE_SEP
std::mutex sepMutex;
#endif
} // namespace
std::vector<Star> measure(const Image &input, Metrics &metrics) {
    auto im = luminance(input);
    std::vector<unsigned char> mask(im.plane());
    for (size_t i = 0; i < im.plane(); ++i)
        if (!std::isfinite(im.pixels[i])) {
            mask[i] = 1;
            im.pixels[i] = 0;
        }
    sep_image view{};
    view.data = im.pixels.data();
    view.dtype = SEP_TFLOAT;
    view.w = im.width;
    view.h = im.height;
    view.mask = mask.data();
    view.mdtype = SEP_TBYTE;
    view.maskthresh = 0;
#ifndef STELLASTACK_THREADSAFE_SEP
    std::lock_guard lock(sepMutex);
#endif
    sep_bkg *rawBackground = nullptr;
    sepCheck(sep_background(&view, 64, 64, 3, 3, 0, &rawBackground));
    std::unique_ptr<sep_bkg, decltype(&sep_bkg_free)> background(rawBackground, sep_bkg_free);
    metrics.background = sep_bkg_global(background.get());
    metrics.noise = sep_bkg_globalrms(background.get());
    if (!std::isfinite(metrics.noise) || metrics.noise <= 0)
        throw Error("No measurable background noise; image may be empty or invalid");
    sepCheck(sep_bkg_subarray(background.get(), im.pixels.data(), SEP_TFLOAT));
    view.noise_type = SEP_NOISE_STDDEV;
    view.noiseval = metrics.noise;
    const float filter[]{1, 2, 1, 2, 4, 2, 1, 2, 1};
    sep_catalog *rawCatalog = nullptr;
    sepCheck(sep_extract(&view, 5, SEP_THRESH_REL, 5, filter, 3, 3, SEP_FILTER_CONV, 32, 0.005, 1, 1,
                         &rawCatalog));
    std::unique_ptr<sep_catalog, decltype(&sep_catalog_free)> catalog(rawCatalog, sep_catalog_free);
    std::vector<int> indexes(size_t(catalog->nobj));
    std::iota(indexes.begin(), indexes.end(), 0);
    std::stable_sort(indexes.begin(), indexes.end(),
                     [&](int a, int b) { return catalog->flux[a] > catalog->flux[b]; });
    std::vector<Star> stars;
    std::vector<double> widths, radii, shapes;
    for (int i : indexes) {
        checkpoint();
        if (stars.size() >= 500)
            break;
        double x = catalog->x[i], y = catalog->y[i];
        int cx = int(std::round(x)), cy = int(std::round(y));
        if (cx < 8 || cy < 8 || cx >= im.width - 8 || cy >= im.height - 8 || catalog->flag[i] ||
            catalog->flux[i] <= 0 || catalog->a[i] > 12 || catalog->b[i] < 0.6)
            continue;
        Eigen::Matrix<double, 6, 6> normal = Eigen::Matrix<double, 6, 6>::Zero();
        Eigen::Matrix<double, 6, 1> rhs = Eigen::Matrix<double, 6, 1>::Zero();
        std::vector<std::pair<double, double>> aperture;
        double total = 0;
        int count = 0;
        for (int yy = cy - 7; yy <= cy + 7; ++yy)
            for (int xx = cx - 7; xx <= cx + 7; ++xx) {
                double dx = xx - x, dy = yy - y, signal = im.pixels[size_t(yy) * im.width + xx];
                if (mask[size_t(yy) * im.width + xx])
                    continue;
                if (signal > 0) {
                    aperture.emplace_back(std::hypot(dx, dy), signal);
                    total += signal;
                }
                if (signal < 3 * metrics.noise)
                    continue;
                Eigen::Matrix<double, 6, 1> row;
                row << 1, dx, dy, dx * dx, dx * dy, dy * dy;
                double weight = signal * signal;
                normal.noalias() += weight * row * row.transpose();
                rhs.noalias() += weight * row * std::log(signal);
                ++count;
            }
        if (count < 10 || total <= 0)
            continue;
        auto fit = normal.ldlt().solve(rhs).eval();
        Eigen::Matrix2d precision;
        precision << -2 * fit[3], -fit[4], -fit[4], -2 * fit[5];
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix2d> eigen(precision);
        if (eigen.info() != Eigen::Success || eigen.eigenvalues()[0] <= 0 || !fit.allFinite())
            continue;
        double major = 1 / std::sqrt(eigen.eigenvalues()[0]), minor = 1 / std::sqrt(eigen.eigenvalues()[1]);
        if (major > 12 || minor < 0.4)
            continue;
        double fwhm = 2.354820045 * std::sqrt(major * minor),
               ecc = std::sqrt(std::max(0.0, 1 - minor * minor / (major * major)));
        std::sort(aperture.begin(), aperture.end());
        double cumulative = 0, hfr = 0;
        for (auto [r, f] : aperture) {
            cumulative += f;
            if (cumulative >= total / 2) {
                hfr = r;
                break;
            }
        }
        stars.push_back({x, y, double(catalog->flux[i]), fwhm, hfr, ecc});
        widths.push_back(fwhm);
        radii.push_back(hfr);
        shapes.push_back(ecc);
    }
    metrics.stars = int(stars.size());
    metrics.fwhm = median(widths);
    metrics.hfr = median(radii);
    metrics.eccentricity = median(shapes);
    if (stars.size() < 6)
        throw Error("Fewer than six usable stars; inspect focus, exposure, or frame type");
    return stars;
}
namespace {
struct Triangle {
    std::array<int, 3> index;
    double a, b;
};
std::vector<Triangle> triangles(const std::vector<Star> &stars) {
    std::vector<Triangle> out;
    int n = int(std::min<size_t>(40, stars.size()));
    auto dist = [&](int i, int j) { return std::hypot(stars[i].x - stars[j].x, stars[i].y - stars[j].y); };
    for (int a = 0; a < n; ++a)
        for (int b = a + 1; b < n; ++b)
            for (int c = b + 1; c < n; ++c) {
                std::array<std::pair<double, int>, 3> sides{
                    {{dist(b, c), a}, {dist(a, c), b}, {dist(a, b), c}}};
                std::sort(sides.begin(), sides.end());
                if (sides[0].first < 10 || sides[0].first + sides[1].first < sides[2].first * 1.03)
                    continue;
                out.push_back({{sides[0].second, sides[1].second, sides[2].second},
                               sides[0].first / sides[2].first,
                               sides[1].first / sides[2].first});
            }
    std::sort(out.begin(), out.end(), [](auto &a, auto &b) { return a.a < b.a; });
    return out;
}
using Matches = std::vector<std::pair<int, int>>; // Reference index, source index.
Transform fitTransform(const Matches &pairs, const std::vector<Star> &src, const std::vector<Star> &ref,
                       bool polynomial) {
    const int terms = polynomial ? 6 : 3;
    Eigen::MatrixXd a(pairs.size(), terms), b(pairs.size(), 2);
    for (size_t i = 0; i < pairs.size(); ++i) {
        const auto &r = ref[pairs[i].first];
        const auto &s = src[pairs[i].second];
        a(i, 0) = 1;
        a(i, 1) = r.x;
        a(i, 2) = r.y;
        if (polynomial) {
            a(i, 3) = r.x * r.x;
            a(i, 4) = r.x * r.y;
            a(i, 5) = r.y * r.y;
        }
        b(i, 0) = s.x;
        b(i, 1) = s.y;
    }
    auto qr = a.colPivHouseholderQr();
    Transform t;
    if (qr.rank() < terms)
        return t;
    Eigen::MatrixXd coeff = qr.solve(b);
    if (!coeff.allFinite())
        return t;
    t.v.fill(0);
    for (int j = 0; j < terms; ++j) {
        t.v[j] = coeff(j, 0);
        t.v[6 + j] = coeff(j, 1);
    }
    t.valid = true;
    t.matches = int(pairs.size());
    double sum = 0;
    for (auto [r, s] : pairs) {
        auto p = t.apply(ref[r].x, ref[r].y);
        sum += std::pow(p[0] - src[s].x, 2) + std::pow(p[1] - src[s].y, 2);
    }
    t.rms = std::sqrt(sum / pairs.size());
    return t;
}
} // namespace
Transform registerStars(const std::vector<Star> &src, const std::vector<Star> &ref, bool polynomial) {
    if (src.size() < 6 || ref.size() < 6)
        throw Error("Registration needs at least six stars");
    std::map<std::pair<int, int>, std::vector<int>> cells;
    for (size_t i = 0; i < src.size(); ++i)
        cells[{int(src[i].x / 4), int(src[i].y / 4)}].push_back(int(i));
    auto match = [&](const Transform &t, double radius) {
        Matches result;
        std::set<int> used;
        for (size_t i = 0; i < ref.size(); ++i) {
            auto p = t.apply(ref[i].x, ref[i].y);
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || std::abs(p[0]) > 1e7 || std::abs(p[1]) > 1e7)
                continue;
            int x = int(std::floor(p[0] / 4)), y = int(std::floor(p[1] / 4));
            double best = radius * radius;
            int chosen = -1;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    auto it = cells.find({x + dx, y + dy});
                    if (it == cells.end())
                        continue;
                    for (int j : it->second) {
                        double d = std::pow(p[0] - src[j].x, 2) + std::pow(p[1] - src[j].y, 2);
                        if (d < best && !used.contains(j)) {
                            best = d;
                            chosen = j;
                        }
                    }
                }
            if (chosen >= 0) {
                result.emplace_back(int(i), chosen);
                used.insert(chosen);
            }
        }
        return result;
    };
    auto st = triangles(src), rt = triangles(ref);
    Matches best;
    size_t trials = 0;
    for (const auto &r : rt) {
        checkpoint();
        auto begin = std::lower_bound(st.begin(), st.end(), r.a - 0.006,
                                      [](const Triangle &t, double a) { return t.a < a; });
        for (auto it = begin; it != st.end() && it->a < r.a + 0.006; ++it) {
            if (std::abs(it->b - r.b) > 0.006)
                continue;
            Matches seed;
            for (int i = 0; i < 3; ++i)
                seed.emplace_back(r.index[i], it->index[i]);
            auto t = fitTransform(seed, src, ref, false);
            double determinant = t.v[1] * t.v[8] - t.v[2] * t.v[7];
            if (!t.valid || std::abs(determinant) < 0.5 || std::abs(determinant) > 2)
                continue;
            auto pairs = match(t, 2);
            if (pairs.size() > best.size())
                best = std::move(pairs);
            if (++trials >= 4000 || best.size() >= std::min(src.size(), ref.size()) * 0.7)
                break;
        }
        if (trials >= 4000 || best.size() >= std::min(src.size(), ref.size()) * 0.7)
            break;
    }
    if (best.size() < 6)
        throw Error("Registration failed: insufficient consistent star matches");
    Transform t = fitTransform(best, src, ref, false);
    for (int pass = 0; pass < 3; ++pass) {
        auto pairs = match(t, std::clamp(3 * t.rms, 0.5, 2.0));
        if (pairs.size() < 6)
            break;
        best = std::move(pairs);
        t = fitTransform(best, src, ref, false);
    }
    if (polynomial && best.size() >= 30) {
        Matches train, test;
        for (size_t i = 0; i < best.size(); ++i)
            (i % 4 ? train : test).push_back(best[i]);
        auto trial = fitTransform(train, src, ref, true);
        double a = 0, b = 0;
        for (auto [r, s] : test) {
            auto p = t.apply(ref[r].x, ref[r].y), q = trial.apply(ref[r].x, ref[r].y);
            a += std::hypot(p[0] - src[s].x, p[1] - src[s].y);
            b += std::hypot(q[0] - src[s].x, q[1] - src[s].y);
        }
        if (trial.valid && b < a * 0.8)
            t = fitTransform(best, src, ref, true);
    }
    if (!t.valid || t.rms > 1.5)
        throw Error("Registration residual exceeds 1.5 pixels");
    return t;
}
Image debayer(const Image &input) {
    if (input.cfa.empty())
        return input;
    if (input.channels != 1 || input.width < 16 || input.height < 16)
        throw Error("RCD requires a mono Bayer image at least 16×16");
    Image out = input;
    out.channels = 3;
    out.cfa.clear();
    out.header.remove("BAYERPAT");
    out.header.remove("XBAYROFF");
    out.header.remove("YBAYROFF");
    out.pixels.resize(out.samples());
    std::vector<float> raw = input.pixels;
    const float factor = input.normalized ? 65535.0f : 1.0f;
    if (factor != 1)
        for (auto &v : raw)
            v *= factor;
    float shift = 0;
    for (float v : raw)
        if (std::isfinite(v))
            shift = std::max(shift, -v);
    if (shift > 0)
        shift += 1;
    for (size_t i = 0; i < raw.size(); ++i)
        raw[i] = std::isfinite(raw[i]) ? raw[i] + shift : shift;
    std::vector<float *> rows(input.height), r(input.height), g(input.height), b(input.height);
    for (int y = 0; y < input.height; ++y) {
        rows[y] = raw.data() + size_t(y) * input.width;
        r[y] = out.pixels.data() + size_t(y) * input.width;
        g[y] = r[y] + out.plane();
        b[y] = g[y] + out.plane();
    }
    unsigned cfa[2][2]{};
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            cfa[y][x] = input.cfa[y * 2 + x] == 'R' ? 0 : input.cfa[y * 2 + x] == 'G' ? 1 : 2;
    auto status = rcd_demosaic(
        input.width, input.height, rows.data(), r.data(), g.data(), b.data(), cfa,
        [](double) {
            checkpoint();
            return false;
        },
        2, false, false);
    if (status != RP_NO_ERROR)
        throw Error("RCD demosaicing failed (" + std::to_string(status) + ")");
    for (size_t i = 0; i < out.samples(); ++i)
        out.pixels[i] = std::isfinite(input.pixels[i % input.plane()])
                            ? (out.pixels[i] - shift) / factor
                            : std::numeric_limits<float>::quiet_NaN();
    return out;
}
float interpolate(const Image &im, int channel, double x, double y) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x > im.width - 1 || y > im.height - 1)
        return nan;
    int ix = int(std::floor(x)), iy = int(std::floor(y));
    if (std::abs(x - std::round(x)) < 1e-8 && std::abs(y - std::round(y)) < 1e-8)
        return im.pixels[size_t(channel) * im.plane() + size_t(std::lround(y)) * im.width +
                         size_t(std::lround(x))];
    if (ix < 2 || iy < 2 || ix + 3 >= im.width || iy + 3 >= im.height)
        return nan;
    static const auto table = [] {
        std::array<std::array<double, 6>, 4096> a{};
        for (size_t k = 0; k < a.size(); ++k)
            for (int j = 0; j < 6; ++j) {
                double d = double(j - 2) - double(k) / a.size();
                a[k][j] = std::abs(d) < 1e-12
                              ? 1
                              : std::sin(std::numbers::pi * d) * std::sin(std::numbers::pi * d / 3) /
                                    (std::numbers::pi * std::numbers::pi * d * d / 3);
            }
        return a;
    }();
    const auto &wx = table[std::min(4095, int((x - ix) * 4096))];
    const auto &wy = table[std::min(4095, int((y - iy) * 4096))];
    double value = 0, total = 0, lo = INFINITY, hi = -INFINITY;
    for (int j = 0; j < 6; ++j)
        for (int i = 0; i < 6; ++i) {
            double w = wx[i] * wy[j];
            if (std::abs(w) < 1e-14)
                continue;
            float v = im.pixels[size_t(channel) * im.plane() + size_t(iy + j - 2) * im.width + ix + i - 2];
            if (!std::isfinite(v))
                return nan;
            value += w * v;
            total += w;
            lo = std::min(lo, double(v));
            hi = std::max(hi, double(v));
        }
    return std::abs(total) > 1e-10 ? float(std::clamp(value / total, lo, hi)) : nan;
}
} // namespace ss

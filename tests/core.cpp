#include "siderastack/core.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <iostream>
#include <random>

void require(bool ok, const char *message) {
    if (!ok)
        throw ss::Error(message);
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("SideraStack-test");
    try {
        QTemporaryDir temp;
        require(temp.isValid(), "temporary directory");
        auto dir = ss::fs::path(temp.path().toStdString());
        ss::Image im;
        im.width = 64;
        im.height = 48;
        im.channels = 3;
        im.header["OBJECT"] = "Test's nebula";
        im.header["EXPTIME"] = 120.5;
        im.pixels.resize(im.samples());
        for (size_t i = 0; i < im.samples(); ++i)
            im.pixels[i] = float(int(i % 100) - 50) / 7;
        im.pixels[3] = NAN;
        for (auto format :
             {ss::Format::Fits, ss::Format::FitsCompressed, ss::Format::Xisf, ss::Format::XisfCompressed}) {
            auto p = dir / (std::to_string(int(format)) + ss::extension(format));
            ss::writeImage(p, im, format);
            auto out = ss::readImage(p);
            require(out.channels == 3 && out.width == 64 && out.height == 48, "geometry roundtrip");
            for (size_t i = 0; i < im.samples(); ++i)
                require(out.pixels[i] == im.pixels[i] ||
                            (std::isnan(out.pixels[i]) && std::isnan(im.pixels[i])),
                        "exact pixel roundtrip");
            require(ss::str(out.header, "OBJECT") == "Test's nebula", "metadata roundtrip");
            bool failed = false;
            try {
                ss::writeImage(p, im, format);
            } catch (...) {
                failed = true;
            }
            require(failed, "no overwrite by default");
        }
        ss::Project project(dir / "test.sidera", true);
        ss::Frame f;
        f.path = dir / "example.fits";
        f.kind = "light";
        f.filter = "Ha";
        project.save(f);
        require(f.id > 0, "persistent ID");
        f.selection = -1;
        project.save(f);
        require(project.frames().at(0).selection == -1, "persistent selection");
        std::mt19937 rng(17);
        std::uniform_real_distribution<double> position(30, 450);
        std::vector<ss::Star> reference, source;
        const double angle = 0.19, c = std::cos(angle), s = std::sin(angle);
        for (int i = 0; i < 70; ++i) {
            double x = position(rng), y = position(rng);
            reference.push_back({x, y, double(10000 - i * 100), 3, 2, 0.1});
            source.push_back({c * x - s * y + 14, s * x + c * y - 9, double(10000 - i * 100), 3, 2, 0.1});
        }
        auto transform = ss::registerStars(source, reference, false);
        require(transform.valid && transform.rms < 1e-7, "rotated registration");
        auto p = transform.apply(123, 234);
        require(std::abs(p[0] - (c * 123 - s * 234 + 14)) < 1e-6, "registration direction");
        for (auto &star : source) {
            star.x = 512 - star.x;
            star.y = 512 - star.y;
        }
        transform = ss::registerStars(source, reference, false);
        require(transform.valid && transform.rms < 1e-7, "meridian flip registration");
        ss::Image mono;
        mono.width = 128;
        mono.height = 128;
        mono.pixels.assign(mono.samples(), -3.5f);
        require(ss::interpolate(mono, 0, 34.2, 55.7) == -3.5f, "negative interpolation preservation");
        mono.cfa = "RGGB";
        auto rgb = ss::debayer(mono);
        require(rgb.channels == 3 && rgb.cfa.empty(), "RCD dimensions");
        require(std::abs(rgb.pixels[64 * 128 + 64] + 3.5) < 1e-3, "negative RCD pedestal");
        std::normal_distribution<float> noise(0, 2);
        for (auto &v : mono.pixels)
            v = 100 + noise(rng);
        mono.cfa.clear();
        for (int y0 : {22, 54, 86, 110})
            for (int x0 : {22, 54, 86, 110})
                for (int y = y0 - 8; y <= y0 + 8; ++y)
                    for (int x = x0 - 8; x <= x0 + 8; ++x)
                        mono.pixels[size_t(y) * 128 + x] +=
                            float(1000 * std::exp(-((x - x0) * (x - x0) + (y - y0) * (y - y0)) / 8.0));
        ss::Metrics metrics;
        auto stars = ss::measure(mono, metrics);
        require(stars.size() >= 12, "SEP detections");
        require(std::abs(metrics.fwhm - 4.70964) < 0.25, "PSF fitted FWHM");
        std::cout << "All core tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

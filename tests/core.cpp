#include "stellastack/core.hpp"
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
    app.setApplicationName("Stellastack-test");
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
        ss::Project project(dir / "test.stella", true);
        ss::Frame f;
        f.path = dir / "example.fits";
        f.kind = "light";
        f.filter = "Ha";
        project.save(f);
        require(f.id > 0, "persistent ID");
        f.selection = -1;
        project.save(f);
        require(project.frames().at(0).selection == -1, "persistent selection");
        int notifications = 0;
        project.frameChanged = [&](int64_t id) {
            require(project.frame(id).has_value(), "frame update is readable after commit");
            ++notifications;
        };
        project.transaction([&] {
            project.save(f);
            project.save(f);
            require(notifications == 0, "transaction updates are deferred");
        });
        require(notifications == 1, "committed frame updates are coalesced");
        try {
            project.transaction([&] {
                project.save(f);
                throw ss::Error("rollback");
            });
        } catch (const ss::Error &) {
        }
        require(notifications == 1, "rolled-back frames are not announced");
        ss::Frame light;
        light.id = 10;
        light.kind = "light";
        light.filter = "L";
        light.session = "night";
        light.descriptor.width = light.descriptor.height = 160;
        light.descriptor.header = {{"INSTRUME", "QHY268M"}, {"XBINNING", "1.0"},
                                   {"YBINNING", 1},         {"GAIN", "0.0"},
                                   {"OFFSET", 11},          {"READOUTM", "High Gain 2CMS"},
                                   {"CCD-TEMP", "-0.0"},    {"EXPTIME", 30.0}};
        auto master = light;
        master.id = 11;
        master.kind = "dark";
        master.master = true;
        master.session = "other";
        master.descriptor.header.remove("GAIN");
        master.descriptor.header.remove("CCD-TEMP");
        master.descriptor.header.remove("OFFSET");
        master.descriptor.header.remove("READOUTM");
        master.path = dir / "masterDark__EXPOSURE_30.00s__GAIN_0__TEMP_0.00.xisf";
        ss::inferMetadata(master);
        require(ss::number(master.descriptor.header, "GAIN") == 0 &&
                    ss::number(master.descriptor.header, "CCD-TEMP") == 0,
                "filename metadata recovery");
        auto cold = master;
        cold.id = 12;
        cold.descriptor.header["CCD-TEMP"] = -5;
        auto plan = ss::calibrationPlan({light, master, cold});
        require(plan[0].toObject()["dark"].toArray() == QJsonArray{11},
                "unique compatible incomplete master");
        require(!plan[0].toObject()["details"].toArray()[0].toObject()["missing"].toArray().isEmpty(),
                "incomplete metadata is disclosed");
        auto duplicate = master;
        duplicate.id = 13;
        require(ss::calibrationPlan({light, master, duplicate})[0].toObject().contains("error"),
                "equally ranked masters require an override");
        master.descriptor.header["GAIN"] = 1;
        require(ss::calibrationPlan({light, master})[0].toObject()["dark"].toArray().isEmpty(),
                "known gain conflict is rejected");
        ss::inferMetadata(master);
        require(ss::number(master.descriptor.header, "GAIN") == 1, "headers win over filename tokens");
        ss::editFrame(master, {{"header", QJsonObject{{"GAIN", QJsonValue()}}}});
        ss::inferMetadata(master);
        require(!master.descriptor.header.contains("GAIN"), "manual missing values remain authoritative");
        auto wrongExposure = cold;
        wrongExposure.descriptor.header["CCD-TEMP"] = 0;
        wrongExposure.descriptor.header["EXPTIME"] = 80;
        require(ss::calibrationPlan({light, wrongExposure})[0].toObject()["dark"].toArray().isEmpty(),
                "dark exposure mismatch is rejected");
        auto raw = light;
        raw.id = 14;
        raw.kind = "dark";
        raw.session = "night";
        require(ss::calibrationPlan({light, cold, raw})[0].toObject()["dark"].toArray() == QJsonArray{14},
                "more complete raw calibration remains usable");
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

#include "siderastack/core.hpp"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QSet>
#include <csignal>
#include <iostream>
#include <mutex>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("SideraStack");
    app.setApplicationVersion(SIDERASTACK_VERSION);
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "SideraStack: linear astrophotography masters\nCommands: init, import, analyze, stack, resume, "
        "export, masters, inspect, convert, list, calibration, settings, edit");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({"json", "Emit structured progress events"});
    parser.addOption({"format", "Export encoding: fits, fits.fz, xisf, xisf-zstd", "format", "fits"});
    parser.addOption({"set", "JSON object containing settings or frame edits", "json"});
    parser.addOption({"ids", "Comma-separated frame IDs for edit", "ids"});
    parser.addOption({"flat-calibration", "Flat calibration: auto, bias, darkflat, none", "policy", "auto"});
    parser.addOption({"overwrite", "Replace an explicitly named conversion output"});
    parser.addPositionalArgument("command", "Command to execute");
    parser.addPositionalArgument("paths", "Project/input/output paths", "[paths...]");
    parser.process(app);
    std::signal(SIGINT, [](int) { ss::cancel(); });
    std::signal(SIGTERM, [](int) { ss::cancel(); });
    auto args = parser.positionalArguments();
    bool events = parser.isSet("json");
    std::recursive_mutex eventMutex;
    auto event = [&](const QJsonObject &o) {
        std::lock_guard guard(eventMutex);
        std::cout << ss::json(o).constData() << '\n' << std::flush;
    };
    ss::Progress progress = [&](const std::string &stage, size_t done, size_t total,
                                const std::string &message) {
        std::lock_guard guard(eventMutex);
        if (events)
            event({{"event", "progress"},
                   {"stage", QString::fromStdString(stage)},
                   {"done", qint64(done)},
                   {"total", qint64(total)},
                   {"message", QString::fromStdString(message)}});
        else
            std::cerr << stage << " " << done << "/" << total << " " << message << '\n';
    };
    try {
        if (args.size() < 2)
            parser.showHelp(2);
        auto command = args[0];
        ss::fs::path path = args[1].toStdString();
        if (command == "inspect") {
            auto im = ss::readImage(path);
            event({{"width", im.width},
                   {"height", im.height},
                   {"channels", im.channels},
                   {"cfa", QString::fromStdString(im.cfa)},
                   {"header", im.header},
                   {"sha256", QString::fromStdString(ss::fingerprint(path))}});
            return 0;
        }
        if (command == "convert") {
            if (args.size() != 3)
                throw ss::Error("convert requires input and output");
            auto im = ss::readImage(path);
            ss::writeImage(args[2].toStdString(), im, ss::parseFormat(parser.value("format").toStdString()),
                           parser.isSet("overwrite"));
            return 0;
        }
        if (command == "init") {
            if (ss::fs::exists(path))
                throw ss::Error("Project already exists");
            ss::Project project(path, true);
            event({{"event", "created"}, {"project", args[1]}});
            return 0;
        }
        ss::Project project(path);
        if (command == "import") {
            std::vector<ss::fs::path> paths;
            for (int i = 2; i < args.size(); ++i)
                paths.emplace_back(args[i].toStdString());
            if (paths.empty())
                throw ss::Error("import requires input files or folders");
            ss::importFiles(project, paths, progress);
        } else if (command == "analyze")
            ss::analyze(project, progress);
        else if (command == "stack" || command == "resume") {
            if (args.size() != 3)
                throw ss::Error("stack/resume requires an output directory");
            ss::stack(project, args[2].toStdString(), progress);
        } else if (command == "export") {
            if (args.size() != 3)
                throw ss::Error("export requires an output directory");
            ss::exportMasters(project, args[2].toStdString(),
                              ss::parseFormat(parser.value("format").toStdString()), progress);
        } else if (command == "masters") {
            if (args.size() != 3)
                throw ss::Error("masters requires a calibration output directory");
            std::vector<int64_t> ids;
            if (parser.isSet("ids"))
                for (const auto &item : parser.value("ids").split(',')) {
                    bool ok = false;
                    auto id = item.trimmed().toLongLong(&ok);
                    if (!ok || id <= 0)
                        throw ss::Error("Invalid calibration frame ID");
                    ids.push_back(id);
                }
            ss::createCalibrationMasters(project, args[2].toStdString(),
                                         parser.value("flat-calibration").toStdString(), ids,
                                         ss::parseFormat(parser.value("format").toStdString()), progress);
        } else if (command == "list") {
            QJsonArray a;
            for (const auto &f : project.frames())
                a.append(f.toJson(false));
            std::cout << QJsonDocument(a).toJson(QJsonDocument::Indented).constData();
            return 0;
        } else if (command == "calibration") {
            std::cout << QJsonDocument(ss::calibrationPlan(project.frames()))
                             .toJson(QJsonDocument::Indented)
                             .constData();
            return 0;
        } else if (command == "settings") {
            auto o = project.settings().toJson();
            if (parser.isSet("set")) {
                auto changes = ss::parseJson(parser.value("set").toUtf8());
                for (auto it = changes.begin(); it != changes.end(); ++it) {
                    if (!o.contains(it.key()))
                        throw ss::Error("Unknown setting: " + it.key().toStdString());
                    o[it.key()] = it.value();
                }
                project.settings(ss::Settings::fromJson(o));
            }
            event(o);
            return 0;
        } else if (command == "edit") {
            if (!parser.isSet("set") || !parser.isSet("ids"))
                throw ss::Error("edit requires --ids and --set");
            auto changes = ss::parseJson(parser.value("set").toUtf8());
            QSet<qint64> ids;
            for (const auto &item : parser.value("ids").split(',')) {
                bool ok = false;
                auto id = item.trimmed().toLongLong(&ok);
                if (!ok || id <= 0)
                    throw ss::Error("Invalid frame ID");
                ids.insert(id);
            }
            auto frames = project.frames();
            project.transaction([&] {
                for (auto &f : frames)
                    if (ids.remove(f.id)) {
                        ss::editFrame(f, changes);
                        project.save(f);
                    }
                if (!ids.empty())
                    throw ss::Error("One or more frame IDs are absent from the project");
            });
        } else
            throw ss::Error("Unknown command: " + command.toStdString());
        event({{"event", "complete"}});
        return 0;
    } catch (const std::exception &e) {
        if (events)
            event({{"event", "error"}, {"message", e.what()}});
        else
            std::cerr << "Error: " << e.what() << '\n';
        return std::string(e.what()) == "interrupted" ? 130 : 1;
    }
}

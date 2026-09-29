// One-shot offscreen capture. No app config, drivers, parameter restore, or control leases.
#include "edge_gateway/scada_project_loader.hpp"
#include "local_display_qt_scada_scene.hpp"

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QRegularExpression>
#include <QThread>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <cmath>
#include <cstdio>

namespace {
class CaptureSource final : public ScadaSceneRuntimeSource {
public:
    explicit CaptureSource(const edge_gateway::ScadaProject& project) {
        if (project.nodes.size() != 1) throw std::runtime_error("Capture requires a single local node");
        node_ = project.nodes.front().nodeId;
        for (const auto& tag : project.tags) tags_.emplace(tag.tagId, tag);
        for (const auto& route : project.runtimeMappings) {
            if (route.nodeId != node_) throw std::runtime_error("Non-local capture route rejected");
            if (!indexes_.emplace(route.index, route.tagId).second) throw std::runtime_error("Duplicate capture index");
            routes_.emplace(route.tagId, route);
        }
    }
    void sample(const QByteArray& bytes) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            throw std::runtime_error("Invalid sample JSON object");
        const auto root = document.object();
        const auto integer = [](const QJsonObject& object, const char* key, double minimum, double maximum) {
            const auto field = object.value(key);
            const double number = field.toDouble();
            if (!field.isDouble() || !std::isfinite(number) || std::floor(number) != number ||
                number < minimum || number > maximum)
                throw std::runtime_error(std::string("Invalid sample integer: ") + key);
            return static_cast<std::int64_t>(number);
        };
        const auto sampleTime = integer(root, "sampleTimestampMs", 1, 9007199254740991.0);
        if (!root.value("points").isArray()) throw std::runtime_error("Sample points array required");
        std::map<std::uint32_t, edge_gateway::StoredPointValue> values;
        std::set<std::uint32_t> seen;
        for (const auto& item : root.value("points").toArray()) {
            if (!item.isObject()) throw std::runtime_error("Sample point object required");
            const auto point = item.toObject();
            edge_gateway::StoredPointValue value;
            value.index = static_cast<std::uint32_t>(integer(point, "index", 0, 4294967295.0));
            if (!seen.insert(value.index).second) throw std::runtime_error("Duplicate sample index");
            if (!point.value("value").isDouble() || !std::isfinite(point.value("value").toDouble()) ||
                !point.value("stale").isBool()) throw std::runtime_error("Sample value/stale required");
            value.value = point.value("value").toDouble();
            value.quality = static_cast<int>(integer(point, "quality", -2147483648.0, 2147483647.0));
            value.ts = integer(point, "ts", 0, 9007199254740991.0);
            value.expireAt = integer(point, "expireAt", 0, 9007199254740991.0);
            value.stale = point.value("stale").toBool() ||
                          (value.expireAt > 0 && sampleTime >= value.expireAt);
            if (indexes_.count(value.index)) values.emplace(value.index, value);
        }
        values_ = std::move(values);
        timestamp = sampleTime;
        unavailable = QJsonArray();
        for (const auto& index : indexes_)
            if (!values_.count(index.first)) unavailable.append(static_cast<qint64>(index.first));
    }
    const std::string& nodeId() const override { return node_; }
    edge_gateway::Optional<ScadaSceneResolvedTag> resolveTag(const std::string& tag) const override {
        const auto t=tags_.find(tag); const auto r=routes_.find(tag);
        if(t==tags_.end() || r==routes_.end()) return edge_gateway::NullOpt;
        return ScadaSceneResolvedTag{t->second,r->second,edge_gateway::NullOpt,edge_gateway::NullOpt,0};
    }
    edge_gateway::Optional<edge_gateway::StoredPointValue> readTag(const std::string& tag,std::int64_t) const override {
        const auto r=routes_.find(tag);
        if(r==routes_.end()) return edge_gateway::NullOpt;
        const auto v=values_.find(r->second.index);
        return v==values_.end() ? edge_gateway::Optional<edge_gateway::StoredPointValue>(edge_gateway::NullOpt) : v->second;
    }
    std::vector<edge_gateway::StoredPointValue> readIndexes(const std::vector<std::uint32_t>& indexes,std::int64_t) const override {
        std::vector<edge_gateway::StoredPointValue> result;
        for(auto index:indexes) { const auto v=values_.find(index);if(v!=values_.end()) result.push_back(v->second); }
        return result;
    }
    ScadaSceneWriteResult submitWrite(const std::string&,edge_gateway::PendingWriteCommand) override {
        ++writeAttempts;return {false,"Capture is read-only; all control writes are prohibited"};
    }
    ScadaSceneWriteResult submitWriteGroup(const std::vector<ScadaSceneWriteTarget>&,edge_gateway::PendingWriteCommand) override {
        ++writeAttempts;return {false,"Capture is read-only; grouped writes are prohibited"};
    }
    edge_gateway::Optional<edge_gateway::WritebackResultRecord> getWritebackResult(const std::string&,const std::string&) const override {
        return edge_gateway::NullOpt;
    }
    std::size_t sampleCount() const { return values_.size(); }
    std::int64_t timestamp=0;
    int writeAttempts=0;
    QJsonArray unavailable;
private:
    std::string node_;
    std::map<std::string,edge_gateway::ScadaTag> tags_;
    std::map<std::string,edge_gateway::ScadaRuntimeMapping> routes_;
    std::map<std::uint32_t,std::string> indexes_;
    std::map<std::uint32_t,edge_gateway::StoredPointValue> values_;
};

QString digest(const QString& path) {
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot hash source/output file");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if(!hash.addData(&file)) throw std::runtime_error("Cannot hash capture file");
    return QString::fromLatin1(hash.result().toHex());
}
} // namespace

#ifndef SCADA_CAPTURE_NO_MAIN
int main(int argc,char** argv) {
    try {
        std::string projectPath,outputPath,samplePath;
        for(int i=1;i<argc;++i) {
            const std::string argument=argv[i];
            if(argument=="--help") {
                std::cout<<"scada_capture_readonly --project DIR --samples FILE_OR_MINUS --output NEW_DIR\n";
                return 0;
            }
            if(i+1>=argc) throw std::runtime_error("Missing argument value");
            if(argument=="--project") projectPath=argv[++i];
            else if(argument=="--output") outputPath=argv[++i];
            else if(argument=="--samples") samplePath=argv[++i];
            else throw std::runtime_error("Unknown argument");
        }
        if(projectPath.empty() || outputPath.empty() || samplePath.empty())
            throw std::runtime_error("Project, samples and output are required; no live data access supported");
        QFile samples;
        if(samplePath=="-") {
            if(!samples.open(stdin,QIODevice::ReadOnly)) throw std::runtime_error("Cannot read sample stdin");
        } else {
            samples.setFileName(QString::fromStdString(samplePath));
            if(!samples.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot open sample file");
        }
        const auto sampleBytes=samples.readAll();
        samples.close();
        QDir source(QString::fromStdString(projectPath));
        const auto sourcePath=source.canonicalPath();
        const auto targetPath=QFileInfo(QString::fromStdString(outputPath)).absoluteFilePath();
        if(sourcePath.isEmpty() || QFileInfo::exists(targetPath) || targetPath.startsWith(sourcePath+"/"))
            throw std::runtime_error("Output must be a new directory outside the source project");
        const auto project=edge_gateway::ScadaProjectLoader::loadFromDirectory(sourcePath.toStdString());
        qputenv("QT_QPA_PLATFORM","offscreen");
        QApplication app(argc,argv);
        CaptureSource runtime(project);
        runtime.sample(sampleBytes);
        if(!QDir().mkpath(targetPath)) throw std::runtime_error("Cannot create capture output");
        QJsonArray captures;
        const QRegularExpression safeName("^[A-Za-z0-9_-]+$");
        for(const auto& screen:project.screens) {
            const auto id=QString::fromStdString(screen.screenId);
            if(!safeName.match(id).hasMatch()) throw std::runtime_error("Unsafe capture screen id");
            ScadaSceneView view(screen,sourcePath.toStdString(),project.alarms,project.trends,runtime,
                                [](const std::string&) {});
            view.setFixedSize(screen.width,screen.height);
            view.show();
            app.processEvents();
            view.refresh(runtime.timestamp);
            app.processEvents();
            const auto path=QDir(targetPath).filePath(id+".png");
            const auto pixels=view.grab();
            if(pixels.width()!=screen.width || pixels.height()!=screen.height || !pixels.save(path))
                throw std::runtime_error("Capture size/save failure");
            QJsonObject item;
            item["screenId"]=id;item["width"]=pixels.width();item["height"]=pixels.height();
            item["pngSha256"]=digest(path);
            item["screenJsonSha256"]=digest(source.filePath("screens/"+id+".json"));
            captures.append(item);
        }
        QJsonObject report;
        report["projectDirectory"]=sourcePath;
        report["sourceManifestSha256"]=digest(source.filePath("manifest.json"));
        report["sampleTimestampMs"]=static_cast<qint64>(runtime.timestamp);
        report["sampledPoints"]=static_cast<qint64>(runtime.sampleCount());
        report["routeCount"]=static_cast<qint64>(project.runtimeMappings.size());
        report["missingSampleIndexes"]=runtime.unavailable;
        report["sampleInputSha256"]=QString::fromLatin1(QCryptographicHash::hash(sampleBytes,QCryptographicHash::Sha256).toHex());
        report["writeAttempts"]=runtime.writeAttempts;
        report["pages"]=captures;
        report["captureKind"]="offscreen-json-sample";
        report["liveDataAccess"]=false;
        report["trendHistoryAvailable"]=false;
        QFile output(QDir(targetPath).filePath("capture-report.json"));
        if(!output.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot create capture report");
        output.write(QJsonDocument(report).toJson());output.close();
        std::cout<<"Captured "<<captures.size()<<" pages; sampled "<<runtime.sampleCount()
                 <<" points; missing samples="<<runtime.unavailable.size()<<"; write attempts="<<runtime.writeAttempts<<'\n';
        return runtime.unavailable.isEmpty() && runtime.writeAttempts==0 ? 0 : 2;
    } catch(const std::exception& error) {
        std::cerr<<"Capture failed: "<<error.what()<<'\n';return 1;
    }
}
#endif

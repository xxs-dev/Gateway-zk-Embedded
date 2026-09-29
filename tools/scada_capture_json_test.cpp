#define SCADA_CAPTURE_NO_MAIN
#include "scada_capture_readonly.cpp"

static void check(bool ok) {
    if (!ok) throw std::runtime_error("Capture JSON behavior test failed");
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Pass project directory");
        const auto project = edge_gateway::ScadaProjectLoader::loadFromDirectory(argv[1]);
        CaptureSource source(project);
        const auto index = project.runtimeMappings.front().index;
        QJsonObject point{{"index", static_cast<qint64>(index)}, {"value", -2.5},
                          {"quality", 1}, {"ts", 900}, {"expireAt", 1100}, {"stale", false}};
        const auto encode = [](QJsonArray points) {
            return QJsonDocument(QJsonObject{{"sampleTimestampMs", 1000}, {"points", points}}).toJson();
        };
        source.sample(encode({point}));
        auto values = source.readIndexes({index}, 1000);
        check(values.size() == 1 && values[0].value == -2.5 && !values[0].stale && values[0].quality == 1);
        point["expireAt"] = 999;
        point["quality"] = 0;
        source.sample(encode({point}));
        values = source.readIndexes({index}, 1000);
        check(values[0].stale && values[0].quality == 0);
        int rejected = 0;
        for (const auto& field : {"index", "value", "quality", "ts", "expireAt", "stale"}) {
            auto invalid = point;
            invalid.remove(field);
            try { source.sample(encode({invalid})); }
            catch (const std::exception&) { ++rejected; }
        }
        try { source.sample(encode({point, point})); }
        catch (const std::exception&) { ++rejected; }
        try { source.sample("{}"); }
        catch (const std::exception&) { ++rejected; }
        check(rejected == 8);
        check(!source.submitWrite("unused", {}).accepted);
        check(!source.submitWriteGroup({}, {}).accepted);
        check(source.writeAttempts == 2 && source.readIndexes({index}, 1000)[0].value == -2.5);
        source.sample(encode({}));
        check(source.sampleCount() == 0 && source.unavailable.size() == static_cast<int>(project.runtimeMappings.size()));
        std::cout << "PASS: JSON values/quality/expiry, 8 invalid inputs, denied writes, missing samples\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

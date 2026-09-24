#define EMS_SHADOW_LIVE_HELPER_NO_MAIN
#include "ems_shadow_live_helper.cpp"
#include <sys/wait.h>

namespace {
using namespace ems_shadow;

template <typename F> void refused(F action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "operation should have been refused");
}

void pipeProtocol() {
    int input[2], output[2];
    require(pipe(input) == 0 && pipe(output) == 0, "create CLI pipes");
    const auto pid = fork();
    require(pid >= 0, "fork CLI");
    if (pid == 0) {
        dup2(input[0], STDIN_FILENO);
        dup2(output[1], STDOUT_FILENO);
        close(input[0]); close(input[1]); close(output[0]); close(output[1]);
        std::vector<std::string> args{"helper", "--shm", kName, "--exclusive-inputs", "--duration-seconds", "5"};
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(&arg[0]);
        try { _exit(ems_shadow::run(static_cast<int>(argv.size()), argv.data())); }
        catch (...) { _exit(2); }
    }
    close(input[0]); close(output[1]);
    struct Child {
        pid_t pid;
        int in, out;
        ~Child() {
            if (in >= 0) close(in);
            close(out);
            if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, nullptr, 0); }
        }
    } child{pid, input[1], output[0]};
    const auto reply = [&] {
        std::string line;
        while (line.size() <= 8192) {
            pollfd ready{child.out, POLLIN, 0};
            require(poll(&ready, 1, 2000) > 0, "bounded CLI reply");
            char ch;
            require(read(child.out, &ch, 1) == 1, "CLI exited before reply");
            if (ch == '\n') return edge_gateway::json::JsonParser(line).parse();
            line += ch;
        }
        throw std::runtime_error("oversized CLI reply");
    };
    const auto send = [&](const std::string& line) {
        const auto data = line + '\n';
        require(write(child.in, data.data(), data.size()) == static_cast<ssize_t>(data.size()), "CLI command write");
        return reply();
    };
    require(reply().find("op")->asString() == "ready", "CLI ready record");
    require(send(R"({"op":"set","values":[{"index":724021,"value":77}]})").find("ok")->asBool(), "CLI set ack");
    usleep(300000);
    const auto sample = send(R"({"op":"sample"})");
    bool refreshed = false;
    for (const auto& point : sample.find("inputs")->asArray().values)
        if (point->find("index")->asNumber() == 724021)
            refreshed = point->find("present")->asBool() && point->find("value")->asNumber() == 77;
    require(refreshed && sample.find("targetsPaused")->asBool(), "CLI timer refresh without target writes");
    close(child.in);
    child.in = -1;
    int status = 0;
    require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "EOF exits cleanly");
    child.pid = 0;
}

void test() {
    refused([] { Session helper("gateway_point_store"); });
    refused([] { Session helper(std::string("/") + kName); });
    refused([] { Session helper(""); });
    refused([] { Session helper(kName); });
    const auto path = std::string("/") + kName;
    require(shm_open(path.c_str(), O_RDONLY, 0) < 0 && errno == ENOENT,
            "OpenExisting must not create missing SHM");
    Descriptor fixture;
    fixture.fd = shm_open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    require(fixture.fd >= 0, "exclusive isolated fixture");
    struct Cleanup { std::string path; ~Cleanup() { shm_unlink(path.c_str()); } } cleanup{path};
    MemoryPointStore store(kName);
    pipeProtocol();
    const std::int64_t now = wallNow();
    {
        Session helper(kName);
        refused([] { Session duplicate(kName); });
        const auto original = store.getStats();
        for (const auto& command : {
                R"({"op":"set","values":[{"index":724001,"value":1}]})",
                R"({"op":"set","values":[{"index":725000,"value":1}]})",
                R"({"op":"set","values":[{"index":724053,"value":1}]})",
                R"({"op":"set","values":[{"index":724020.5,"value":1}]})",
                R"({"op":"set","values":[{"index":724020,"value":1e999}]})",
                R"({"op":"set","values":[{"index":724020,"value":101}]})",
                R"({"op":"set","values":[{"index":724026,"value":2}]})",
                R"({"op":"set","values":[{"index":724021,"value":-1}]})",
                R"({"op":"set","values":[{"index":724020,"value":50},{"index":725000,"value":1}]})",
                R"({"op":"set","values":[{"index":724020,"value":50},{"index":724020,"value":51}]})",
                R"({"op":"sample","op":"quit"})",
                R"({"op":"sample","shm":"gateway_point_store"})",
                R"({"op":"resume-targets"})",
                R"({"op":"write"})"}) {
            refused([&] { helper.command(command, now); });
        }
        helper.tick(now);
        require(store.getStats().latestCount == original.latestCount, "invalid batch must not update any input");
        helper.command(R"({"op":"set","values":[{"index":724000,"value":1},{"index":724020,"value":80},{"index":724060,"value":18},{"index":724061,"value":18},{"index":724062,"value":18},{"index":724063,"value":0},{"index":724064,"value":0},{"index":724065,"value":0}]})", now);
        helper.tick(now);
        require(!store.getLatestByIndex(724060, now), "targets start paused");
        helper.command(R"({"op":"resume-targets"})", now);
        helper.tick(now + 200);
        const auto targetTs = store.getLatestByIndex(724060, now + 200)->ts;
        helper.command(R"({"op":"pause-targets"})", now + 250);
        helper.command(R"({"op":"set","values":[{"index":724020,"value":20},{"index":724060,"value":-18}]})", now + 250);
        helper.tick(now + 400);
        helper.tick(now + 1400);
        const auto capability = store.getLatestByIndex(724020, now + 1400);
        const auto paused = store.getLatestByIndex(724060, now + 1400);
        require(capability && capability->value == 20 && capability->ts == now + 1400 && !capability->stale,
                "paused targets must not stop capability refresh or live set");
        require(paused && paused->ts == targetTs && paused->value == 18 && paused->stale,
                "paused target retains original timestamp and expires naturally");
        helper.command(R"({"op":"resume-targets"})", now + 1400);
        helper.tick(now + 1600);
        require(store.getLatestByIndex(724060, now + 1600)->value == -18, "resume uses updated target");

        edge_gateway::PointValue sink;
        sink.index = 725000;
        sink.value = std::nextafter(1.0, 2.0);
        sink.quality = 1;
        sink.ts = now + 1600;
        sink.expireAt = now + 2600;
        store.putLatest(sink);
        const auto text = helper.command(R"({"op":"sample"})", now + 1601);
        const auto sample = edge_gateway::json::JsonParser(text).parse();
        const auto& first = *sample.find("points")->asArray().values.front();
        require(first.find("value")->asNumber() == sink.value, "sink double must round-trip without precision loss");
        require(first.find("ts")->asNumber() == sink.ts && first.find("expireAt")->asNumber() == sink.expireAt &&
                first.find("quality")->asNumber() == 1 && !first.find("stale")->asBool(), "sink metadata preserved");
        require(sample.find("pendingWrites")->asNumber() == 0 && store.peekPendingWriteCommands().empty(),
                "helper must never enqueue control writes");
        require(!store.clusterAuthority(), "helper must not manufacture cluster authority");
        sink.index = 724020;
        sink.value = 90;
        store.putLatest(sink);
        refused([&] { helper.tick(now + 1800); });
        require(store.getLatestByIndex(724000, now + 1800)->ts == now + 1600,
                "external writer detection must abort whole refresh");
    }
    {
        Session helper(kName);
        helper.command(R"({"op":"set","values":[{"index":724020,"value":50}]})", now + 1700);
        refused([&] { helper.tick(now + 1700); });
    }
    {
        Session helper(kName);
        require(shm_unlink(path.c_str()) == 0, "remove test segment name");
        refused([&] { helper.tick(now + 3000); });
        require(shm_open(path.c_str(), O_RDONLY, 0) < 0 && errno == ENOENT,
                "lost name must not be recreated");
    }
}
}  // namespace

int main() {
    const auto path = std::string("/") + ems_shadow::kName;
    const int existing = shm_open(path.c_str(), O_RDONLY, 0);
    if (existing >= 0) { close(existing); std::cerr << "SKIP: shadow segment exists; use isolated namespace\n"; return 77; }
    try { test(); std::cout << "ems_shadow_live_helper_test passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

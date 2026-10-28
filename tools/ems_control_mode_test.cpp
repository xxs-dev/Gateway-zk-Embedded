#include "edge_gateway/ems_control_mode.hpp"
#include "edge_gateway/memory_point_store.hpp"
#include "edge_gateway/filesystem_compat.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifndef _WIN32
#include <unistd.h>
#endif

using namespace edge_gateway;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejected(F f) { bool failed=false; try { f(); } catch (const std::exception&) { failed=true; } require(failed,"operation should be rejected"); }
int main() {
#ifdef _WIN32
    return 0;
#else
    char name[]="/tmp/ems-mode-test-XXXXXX";
    const auto dir=mkdtemp(name);
    if (!dir) return 1;
    try {
        const std::string path=std::string(dir)+"/mode.json";
        EmsControlMode mode(path);
        require(mode.read().remote,"missing state must default to remote");
        mode.admitPlatform(100);
        PendingWriteCommand remote;
        remote.cmdId="remote"; remote.index=1; remote.acceptedAt=100; remote.platformControl=true;
        remote.source="local-display-scada"; remote.highPriority=true;
        {
            const auto permit=mode.dispatchPermit(remote);
            rejected([&]{mode.setRemote(false,101);});
            require(mode.read().remote,"busy transition must not change state");
        }
        mode.setRemote(false,200);
        require(!EmsControlMode(path).read().remote,"local choice must survive reopen");
        rejected([&]{mode.admitPlatform(201);});
        rejected([&]{mode.dispatchPermit(remote);});
        auto local=remote; local.platformControl=false;
        require(!mode.dispatchPermit(local),"local transport must remain available");
        mode.setRemote(true,300);
        rejected([&]{mode.dispatchPermit(remote);});
        rejected([&]{mode.admitPlatform(199);});
        remote.acceptedAt=301;
        require(bool(mode.dispatchPermit(remote)),"fresh remote command should be accepted");
        MemoryStoreConfig cfg;
        cfg.sharedMemoryName="ems_mode_fixture_"+std::to_string(getpid());
        MemoryPointStore::cleanupOrphanedSegment(cfg.sharedMemoryName);
        {
            MemoryPointStore store(cfg);
            store.submitWriteCommand(remote);
            const auto peek=store.peekPendingWriteCommands();
            require(peek.size()==1 && peek[0].platformControl,"peek lost trusted transport flag");
            const auto drained=store.drainPendingWriteCommands();
            require(drained.size()==1 && drained[0].platformControl && drained[0].acceptedAt==301,"drain lost trusted metadata");
        }
        MemoryPointStore::cleanupOrphanedSegment(cfg.sharedMemoryName);
        {std::ofstream out(path);out << "{bad";}
        rejected([&]{mode.admitPlatform(400);});
        rejected([&]{mode.setRemote(true,400);});
        require(!mode.dispatchPermit(local),"invalid state must not block local transport");
        filesystem::remove_all(dir);
        std::cout << "ems_control_mode_test passed\n";
        return 0;
    } catch (const std::exception& ex) {std::cerr << ex.what() << '\n';return 1;}
#endif
}

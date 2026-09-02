#ifndef _WIN32

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <iostream>
#include <stdexcept>
#include <string>

#include "edge_gateway/posix_serial_port.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    try {
        const int master = posix_openpt(O_RDWR | O_NOCTTY);
        require(master >= 0, "failed to create pseudo terminal");
        require(grantpt(master) == 0, "failed to grant pseudo terminal");
        require(unlockpt(master) == 0, "failed to unlock pseudo terminal");
        const char* slaveName = ptsname(master);
        require(slaveName != nullptr, "failed to resolve pseudo terminal path");

        edge_gateway::SerialPortOptions options;
        options.device = slaveName;
        options.baudRate = 9600;
        options.dataBits = 8;
        options.stopBits = 1;
        options.parity = "N";
        edge_gateway::PosixSerialPort port(options);
        port.open();
        require(port.isOpen(), "pseudo serial port should open");

        close(master);
        bool failed = false;
        try {
            (void)port.read(64, 200);
        } catch (const std::exception&) {
            failed = true;
        }
        require(failed, "serial hangup should be surfaced as a hard error");
        require(!port.isOpen(), "hard serial error must close the stale descriptor");

        std::cout << "posix serial port recovery test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "posix serial port recovery test failed: " << ex.what() << std::endl;
        return 1;
    }
}

#endif

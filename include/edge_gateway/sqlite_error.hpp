#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace edge_gateway {

// Snapshot SQLite diagnostics before reset/finalize/rollback can replace them.
class SqliteError : public std::runtime_error {
public:
    SqliteError(int extendedCode, std::string operation, bool transactionActive,
        std::string message)
        : std::runtime_error(message + " [sqlite primary=" + std::to_string(extendedCode & 255) +
              " extended=" + std::to_string(extendedCode) + " operation=" + operation +
              " transactionActive=" + (transactionActive ? "true" : "false") + "]"),
          extendedCode_(extendedCode), operation_(std::move(operation)),
          transactionActive_(transactionActive) {}

    int primaryCode() const noexcept { return extendedCode_ & 255; }
    int extendedCode() const noexcept { return extendedCode_; }
    const std::string& operation() const noexcept { return operation_; }
    bool transactionActive() const noexcept { return transactionActive_; }
    bool isBusy() const noexcept { return primaryCode() == 5; }

private:
    int extendedCode_;
    std::string operation_;
    bool transactionActive_;
};

}  // namespace edge_gateway

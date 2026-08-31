#pragma once

#include <cstdint>
#include <string>

#include "edge_gateway/scada_models.hpp"

class QWidget;

class ScadaLocalAccessSession final {
public:
    explicit ScadaLocalAccessSession(edge_gateway::ScadaLocalAccess configuration = {});

    bool enabled() const;
    bool requiresAuthentication(const std::string& screenId) const;
    bool authorizedForScreen(const std::string& screenId, std::int64_t nowMs) const;
    bool authenticate(
        const std::string& username,
        const std::string& password,
        std::int64_t nowMs,
        std::string* message = nullptr
    );
    void touch(std::int64_t nowMs);
    void logout();

    bool authenticated(std::int64_t nowMs) const;
    const std::string& username() const;
    std::int64_t expiresAtMs() const;

private:
    edge_gateway::ScadaLocalAccess configuration_;
    std::string username_;
    std::int64_t expiresAtMs_ = 0;
};

bool requestScadaLocalLogin(
    QWidget* parent,
    ScadaLocalAccessSession& session,
    std::int64_t nowMs
);

#include "local_display_qt_access_control.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

edge_gateway::ScadaLocalAccess testConfiguration() {
    edge_gateway::ScadaLocalAccess access;
    access.sessionTimeoutSeconds = 60;
    access.protectedScreenPrefixes = {"Strategy-", "Control-"};
    edge_gateway::ScadaLocalAccessUser user;
    user.username = "operator";
    user.salt = "00112233445566778899aabbccddeeff";
    user.passwordSha256 = "e818d3f9b975db3914dedfeb7f81d07324d445ba6bdacaccf4f2098fbb51c4a6";
    user.roles = {"operator"};
    access.users.push_back(user);
    return access;
}

}  // namespace

int main() {
    try {
        ScadaLocalAccessSession session(testConfiguration());
        require(session.enabled(), "local access should be enabled");
        require(!session.requiresAuthentication("Overview"), "overview should remain public");
        require(session.requiresAuthentication("Strategy-Overview"), "strategy should be protected");
        require(session.requiresAuthentication("Control-Pcs"), "control should be protected");
        require(session.authorizedForScreen("Overview", 1000), "public page should not require login");
        require(!session.authorizedForScreen("Control-Pcs", 1000), "protected page opened without login");

        std::string message;
        require(!session.authenticate("operator", "wrong", 1000, &message), "invalid password was accepted");
        require(!session.authenticate("missing", "secret", 1000, &message), "unknown account was accepted");
        require(session.authenticate("operator", "secret", 1000, &message), "valid credentials were rejected");
        require(session.authorizedForScreen("Strategy-Grid", 60000), "valid session expired too early");
        require(!session.authorizedForScreen("Strategy-Grid", 61001), "expired session remained authorized");

        require(session.authenticate("operator", "secret", 100000, &message), "re-login failed");
        session.touch(150000);
        require(session.authorizedForScreen("Control-Dido", 200000), "activity did not extend the session");
        session.logout();
        require(!session.authorizedForScreen("Control-Dido", 200000), "logout did not revoke access");
        std::cout << "local_display_qt_access_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "local_display_qt_access_test failed: " << ex.what() << std::endl;
        return 1;
    }
}

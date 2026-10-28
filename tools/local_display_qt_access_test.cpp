#include "local_display_qt_access_control.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <memory>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QDateTime>
#include <QTimer>

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

int main(int argc, char** argv) {
    QApplication app(argc, argv);
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

        for (int attempt = 0; attempt < 5; ++attempt) {
            require(!session.authenticate("operator", "wrong", 300000, &message), "invalid password was accepted");
        }
        require(!session.authenticate("operator", "secret", 300000, &message), "login was not locked after repeated failures");
        require(!session.authenticate("operator", "secret", 329999, &message), "lockout ended too early");
        require(session.authenticate("operator", "secret", 330000, &message), "login stayed locked after the lockout");
        edge_gateway::ScadaScreen protectedScreen;
        protectedScreen.screenId = "Control-Pcs";
        edge_gateway::ScadaWidget control;
        control.widgetId = "pcs-control";
        control.type = "pcsPhasePowerControl";
        control.action.type = "writeSetpoint";
        protectedScreen.widgets = {control};
        control.widgetId = "lanhu-untrusted-control";
        protectedScreen.widgets.push_back(control);
        const auto gate = makeLanhuAccessGate(protectedScreen);
        require(gate.widgets.size() == 4, "gate retained protected content");
        require(gate.widgets[1].widgetId == "access-gate-icon" &&
                gate.widgets[1].properties.at("qtImageFile") == "assets/lanhu-assets/access-lock.png",
                "gate artwork missing");
        for (const auto& widget : gate.widgets) {
            require(widget.bindings.empty(), "gate retained telemetry binding");
            require(widget.action.type == "none" || widget.action.type == "navigate", "gate contains executable writes");
        }
        require(gate.widgets.back().action.targetScreen == "__local_login__", "gate login target missing");

        ScadaLocalAccessSession uiSession(testConfiguration());
        std::unique_ptr<QDialog> dialog(createLanhuLocalLogin(nullptr, uiSession, argc > 1 ? argv[1] : "."));
        dialog->resize(1440, 810);
        dialog->show();
        app.processEvents();
        auto findWidget = [](const char* name) -> QWidget* {
            for (auto* widget : QApplication::allWidgets()) if (widget->objectName() == name) return widget;
            throw std::runtime_error(std::string("missing login control: ") + name);
        };
        auto* username = qobject_cast<QLineEdit*>(findWidget("lanhuUsername"));
        auto* password = qobject_cast<QLineEdit*>(findWidget("lanhuPassword"));
        auto* submit = qobject_cast<QPushButton*>(findWidget("lanhuSubmit"));
        auto* remember = qobject_cast<QCheckBox*>(findWidget("lanhuRememberPassword"));
        require(password->echoMode() == QLineEdit::Password, "login exposes password");
        require(!remember->isEnabled() && !remember->isChecked(), "password persistence is not reserved");
        if (argc > 2) {
            QDir().mkpath(argv[2]);
            require(dialog->grab().scaled(1440, 810, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(QString::fromLocal8Bit(argv[2]) + "/Login.png"), "login screenshot failed");
            dialog->resize(800, 600); app.processEvents();
            require(dialog->grab().scaled(800, 600, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(QString::fromLocal8Bit(argv[2]) + "/Login-800x600.png"), "scaled login screenshot failed");
            dialog->resize(1440, 810); app.processEvents();
        }
        username->setText("operator"); password->setText("wrong"); submit->click();
        require(!uiSession.authenticated(QDateTime::currentMSecsSinceEpoch()), "UI accepted wrong password");
        require(password->text().isEmpty(), "UI retained submitted password");
        require(!qobject_cast<QLabel*>(findWidget("lanhuLoginError"))->text().isEmpty(), "UI hid authentication error");
        password->setFocus(); app.processEvents();
        QTimer::singleShot(0, [&]() {
            auto* keyboard = QApplication::activeModalWidget();
            require(keyboard && keyboard->objectName() == "lanhuTouchKeyboard", "touch keyboard did not open");
            for (auto* key : keyboard->findChildren<QPushButton*>()) if (key->text() == "s") key->click();
            qobject_cast<QDialog*>(keyboard)->accept();
        });
        qobject_cast<QPushButton*>(findWidget("lanhuKeyboard"))->click();
        require(password->text() == "s", "touch keyboard typed into wrong input");
        password->setText("secret"); submit->click();
        require(dialog->result() == QDialog::Accepted && uiSession.authenticated(QDateTime::currentMSecsSinceEpoch()), "UI login failed");
        require(password->text().isEmpty(), "successful login retained password");
        std::cout << "local_display_qt_access_test passed" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "local_display_qt_access_test failed: " << ex.what() << std::endl;
        return 1;
    }
}

#include "local_display_qt_access_control.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include <QApplication>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

namespace {

bool constantTimeEqual(const QByteArray& left, const QByteArray& right) {
    if (left.size() != right.size()) return false;
    unsigned char difference = 0;
    for (int index = 0; index < left.size(); ++index) {
        difference |= static_cast<unsigned char>(left[index]) ^
            static_cast<unsigned char>(right[index]);
    }
    return difference == 0;
}

QByteArray passwordDigest(
    const std::string& salt,
    const std::string& password
) {
    QByteArray payload = QByteArray::fromStdString(salt);
    payload.append(':');
    payload.append(QByteArray::fromStdString(password));
    const auto digest = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
    payload.fill('\0');
    return digest;
}

class TouchLoginDialog final : public QDialog {
public:
    TouchLoginDialog(
        ScadaLocalAccessSession& session,
        std::int64_t nowMs,
        QWidget* parent
    ) : QDialog(parent), session_(session), nowMs_(nowMs) {
        setWindowTitle(QString::fromUtf8("操作员登录"));
        setModal(true);
        setMinimumSize(820, 680);
        setStyleSheet(R"CSS(
            QDialog { background:#071B26; color:#F3F8FA; }
            QLabel#title { color:#F3F8FA; font-size:30px; font-weight:700; }
            QLabel#hint { color:#8FB2BF; font-size:18px; }
            QLabel#error { color:#FF7A7A; font-size:18px; min-height:28px; }
            QLineEdit { background:#0B2633; border:2px solid #31596A; border-radius:6px;
                        padding:12px 16px; color:#F3F8FA; font-size:24px; min-height:42px; }
            QLineEdit:focus { border-color:#55D8E8; }
            QPushButton { background:#173847; border:1px solid #3D6473; border-radius:5px;
                          color:#F3F8FA; font-size:21px; min-height:54px; }
            QPushButton:pressed { background:#2B7184; }
            QPushButton#primary { background:#0E705F; border-color:#55E0AA; font-weight:700; }
            QPushButton#cancel { background:#3B2B30; border-color:#8B555C; }
        )CSS");

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(34, 28, 34, 28);
        root->setSpacing(14);

        auto* title = new QLabel(QString::fromUtf8("操作员登录"));
        title->setObjectName(QStringLiteral("title"));
        root->addWidget(title);
        auto* hint = new QLabel(QString::fromUtf8("策略和控制属于受保护操作，请输入本机账号与密码。"));
        hint->setObjectName(QStringLiteral("hint"));
        hint->setWordWrap(true);
        root->addWidget(hint);

        username_ = new QLineEdit();
        username_->setPlaceholderText(QString::fromUtf8("账号"));
        username_->setAccessibleName(QString::fromUtf8("账号"));
        root->addWidget(username_);
        password_ = new QLineEdit();
        password_->setPlaceholderText(QString::fromUtf8("密码"));
        password_->setAccessibleName(QString::fromUtf8("密码"));
        password_->setEchoMode(QLineEdit::Password);
        root->addWidget(password_);
        activeInput_ = username_;
        connect(username_, &QLineEdit::selectionChanged, this, [this]() { activeInput_ = username_; });
        connect(password_, &QLineEdit::selectionChanged, this, [this]() { activeInput_ = password_; });
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* current) {
            if (current == username_) activeInput_ = username_;
            else if (current == password_) activeInput_ = password_;
        });

        auto* keyboard = new QGridLayout();
        keyboard->setHorizontalSpacing(7);
        keyboard->setVerticalSpacing(7);
        const QStringList rows = {
            QStringLiteral("1234567890"),
            QStringLiteral("qwertyuiop"),
            QStringLiteral("asdfghjkl"),
            QStringLiteral("zxcvbnm")
        };
        for (int row = 0; row < rows.size(); ++row) {
            const auto keys = rows[row];
            const int offset = row >= 2 ? row - 1 : 0;
            for (int column = 0; column < keys.size(); ++column) {
                auto* button = new QPushButton(keys.mid(column, 1));
                keyButtons_.push_back(button);
                keyboard->addWidget(button, row, column + offset);
                connect(button, &QPushButton::clicked, this, [this, button]() {
                    if (activeInput_ == nullptr) return;
                    auto text = button->text();
                    if (shifted_) text = text.toUpper();
                    activeInput_->insert(text);
                    activeInput_->setFocus();
                });
            }
        }
        root->addLayout(keyboard);

        auto* utilityRow = new QHBoxLayout();
        auto* shift = new QPushButton(QString::fromUtf8("大写"));
        auto* backspace = new QPushButton(QString::fromUtf8("退格"));
        auto* clear = new QPushButton(QString::fromUtf8("清空"));
        auto* showPassword = new QPushButton(QString::fromUtf8("显示密码"));
        utilityRow->addWidget(shift);
        for (const auto& symbol : {
            QStringLiteral("!"),
            QStringLiteral("#"),
            QStringLiteral("-"),
            QStringLiteral("_"),
            QStringLiteral("."),
            QStringLiteral("@")
        }) {
            auto* button = new QPushButton(symbol);
            utilityRow->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, symbol]() {
                if (activeInput_ != nullptr) activeInput_->insert(symbol);
            });
        }
        utilityRow->addWidget(backspace);
        utilityRow->addWidget(clear);
        utilityRow->addWidget(showPassword);
        root->addLayout(utilityRow);

        connect(shift, &QPushButton::clicked, this, [this, shift]() {
            shifted_ = !shifted_;
            shift->setText(shifted_ ? QString::fromUtf8("小写") : QString::fromUtf8("大写"));
            for (auto* button : keyButtons_) {
                if (button->text().size() == 1 && button->text().front().isLetter()) {
                    button->setText(shifted_ ? button->text().toUpper() : button->text().toLower());
                }
            }
        });
        connect(backspace, &QPushButton::clicked, this, [this]() {
            if (activeInput_ != nullptr) activeInput_->backspace();
        });
        connect(clear, &QPushButton::clicked, this, [this]() {
            if (activeInput_ != nullptr) activeInput_->clear();
        });
        connect(showPassword, &QPushButton::clicked, this, [this, showPassword]() {
            const auto visible = password_->echoMode() == QLineEdit::Normal;
            password_->setEchoMode(visible ? QLineEdit::Password : QLineEdit::Normal);
            showPassword->setText(visible ? QString::fromUtf8("显示密码") : QString::fromUtf8("隐藏密码"));
        });

        error_ = new QLabel();
        error_->setObjectName(QStringLiteral("error"));
        root->addWidget(error_);
        auto* actionRow = new QHBoxLayout();
        auto* cancel = new QPushButton(QString::fromUtf8("取消"));
        cancel->setObjectName(QStringLiteral("cancel"));
        auto* login = new QPushButton(QString::fromUtf8("登录"));
        login->setObjectName(QStringLiteral("primary"));
        actionRow->addWidget(cancel);
        actionRow->addWidget(login, 2);
        root->addLayout(actionRow);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        connect(login, &QPushButton::clicked, this, [this]() { submit(); });
        connect(password_, &QLineEdit::returnPressed, this, [this]() { submit(); });
    }

private:
    void submit() {
        std::string message;
        const auto username = username_->text().trimmed().toStdString();
        auto passwordBytes = password_->text().toUtf8();
        const auto accepted = session_.authenticate(
            username,
            passwordBytes.toStdString(),
            nowMs_,
            &message
        );
        passwordBytes.fill('\0');
        password_->clear();
        if (accepted) {
            accept();
            return;
        }
        error_->setText(QString::fromStdString(message));
        password_->setFocus();
        activeInput_ = password_;
    }

    ScadaLocalAccessSession& session_;
    std::int64_t nowMs_ = 0;
    QLineEdit* username_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLineEdit* activeInput_ = nullptr;
    QLabel* error_ = nullptr;
    std::vector<QPushButton*> keyButtons_;
    bool shifted_ = false;
};

}  // namespace

ScadaLocalAccessSession::ScadaLocalAccessSession(
    edge_gateway::ScadaLocalAccess configuration
) : configuration_(std::move(configuration)) {
}

bool ScadaLocalAccessSession::enabled() const {
    return configuration_.enabled();
}

bool ScadaLocalAccessSession::requiresAuthentication(const std::string& screenId) const {
    if (!enabled()) return false;
    return std::any_of(
        configuration_.protectedScreenPrefixes.begin(),
        configuration_.protectedScreenPrefixes.end(),
        [&](const std::string& prefix) { return screenId.compare(0, prefix.size(), prefix) == 0; }
    );
}

bool ScadaLocalAccessSession::authorizedForScreen(
    const std::string& screenId,
    std::int64_t nowMs
) const {
    return !requiresAuthentication(screenId) || authenticated(nowMs);
}

bool ScadaLocalAccessSession::authenticate(
    const std::string& username,
    const std::string& password,
    std::int64_t nowMs,
    std::string* message
) {
    const auto user = std::find_if(
        configuration_.users.begin(),
        configuration_.users.end(),
        [&](const edge_gateway::ScadaLocalAccessUser& item) { return item.username == username; }
    );
    const auto actual = user == configuration_.users.end()
        ? passwordDigest("00000000000000000000000000000000", password)
        : passwordDigest(user->salt, password);
    const auto expected = user == configuration_.users.end()
        ? QByteArray(64, '0')
        : QByteArray::fromStdString(user->passwordSha256);
    if (user == configuration_.users.end() || !constantTimeEqual(actual, expected)) {
        if (message != nullptr) *message = "账号或密码错误";
        return false;
    }
    username_ = user->username;
    expiresAtMs_ = nowMs + static_cast<std::int64_t>(configuration_.sessionTimeoutSeconds) * 1000;
    if (message != nullptr) *message = "登录成功";
    return true;
}

void ScadaLocalAccessSession::touch(std::int64_t nowMs) {
    if (!authenticated(nowMs)) return;
    expiresAtMs_ = nowMs + static_cast<std::int64_t>(configuration_.sessionTimeoutSeconds) * 1000;
}

void ScadaLocalAccessSession::logout() {
    username_.clear();
    expiresAtMs_ = 0;
}

bool ScadaLocalAccessSession::authenticated(std::int64_t nowMs) const {
    return !username_.empty() && nowMs <= expiresAtMs_;
}

const std::string& ScadaLocalAccessSession::username() const {
    return username_;
}

std::int64_t ScadaLocalAccessSession::expiresAtMs() const {
    return expiresAtMs_;
}

bool requestScadaLocalLogin(
    QWidget* parent,
    ScadaLocalAccessSession& session,
    std::int64_t nowMs
) {
    TouchLoginDialog dialog(session, nowMs, parent);
    return dialog.exec() == QDialog::Accepted;
}

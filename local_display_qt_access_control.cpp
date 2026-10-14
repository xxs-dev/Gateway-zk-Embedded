#include "local_display_qt_access_control.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include <QApplication>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGraphicsProxyWidget>
#include <QIcon>
#include <QResizeEvent>
#include <QStyle>
#include <QPainter>
#include <QPainterPath>

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
        // Use the submit time, not the dialog-open time, for lockout and session expiry.
        const auto accepted = session_.authenticate(
            username,
            passwordBytes.toStdString(),
            std::max<std::int64_t>(nowMs_, QDateTime::currentMSecsSinceEpoch()),
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

namespace {
class LanhuLoginDialog final : public QDialog {
public:
    LanhuLoginDialog(QWidget* parent, ScadaLocalAccessSession& session, const std::string& assets)
        : QDialog(parent), session_(session) {
        setObjectName(QStringLiteral("lanhuLoginDialog"));
        setWindowTitle(QString::fromUtf8("操作员登录"));
        setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
        resize(parent ? parent->size() : QSize(1440, 810));
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        view_ = new QGraphicsView(this);
        view_->setFrameShape(QFrame::NoFrame);
        view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        view_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        view_->viewport()->installEventFilter(this);
        view_->setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        auto* scene = new QGraphicsScene(0, 0, 1440, 810, view_);
        view_->setScene(scene);
        layout->addWidget(view_);
        const auto directory = QString::fromStdString(assets) + '/';
        scene->setBackgroundBrush(QColor("#E5EFF8"));
        const QPixmap background(directory + "login-background.png");
        if (!background.isNull()) scene->addPixmap(background.scaled(1440, 810, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        auto* logo = new QLabel;
        logo->setStyleSheet("background:transparent;");
        logo->setFixedSize(174, 36);
        logo->setPixmap(QPixmap(directory + "login-logo.png").scaled(174, 36, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        scene->addWidget(logo)->setPos(51, 60);
        auto* brand = new QLabel(QString::fromUtf8("移动储能车监控平台"));
        brand->setStyleSheet("color:#303438;background:transparent;font-size:30px;font-weight:700;");
        brand->setFixedSize(560, 50);
        scene->addWidget(brand)->setPos(255, 55);
        auto* form = new QWidget;
        form->setObjectName("loginForm");
        form->setAttribute(Qt::WA_StyledBackground, true);
        form->setAttribute(Qt::WA_TranslucentBackground, true);
        form->setFont(QFont(QStringLiteral("Microsoft YaHei")));
        form->setFixedSize(447, 450);
        QPainterPath formBackground;
        formBackground.addRoundedRect(QRectF(883.5, 180, 447, 450), 12, 12);
        scene->addPath(formBackground, QPen(Qt::NoPen), QBrush(Qt::white));
        form->setStyleSheet(R"CSS(
            QWidget#loginForm {background:transparent;}
            QLabel {color:#404040;background:transparent;}
            QLineEdit {border:1px solid #C8C8C8;border-radius:5px;background:white;
                       color:#404040;font-size:16px;padding:0 14px;}
            QLineEdit:focus {border-color:#129AF5;}
            QCheckBox {color:#404040;font-size:15px;}
            QPushButton {background:#129AF5;color:white;border:0;border-radius:5px;font-size:18px;font-weight:700;}
            QPushButton:pressed {background:#168ACD;}
        )CSS");
        auto* title = new QLabel(QString::fromUtf8("欢迎登录"), form);
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet("font-size:24px;font-weight:700;");
        title->setGeometry(24, 30, 399, 42);
        username_ = new QLineEdit(form);
        username_->setObjectName("lanhuUsername");
        username_->setAccessibleName(QString::fromUtf8("用户名"));
        username_->setPlaceholderText(QString::fromUtf8("请输入用户名"));
        username_->setGeometry(24, 102, 399, 51);
        username_->addAction(QIcon(directory + "login-user.png"), QLineEdit::LeadingPosition);
        password_ = new QLineEdit(form);
        password_->setObjectName("lanhuPassword");
        password_->setAccessibleName(QString::fromUtf8("密码"));
        password_->setPlaceholderText(QString::fromUtf8("请输入密码"));
        password_->setEchoMode(QLineEdit::Password);
        password_->setGeometry(24, 180, 399, 51);
        password_->addAction(QIcon(directory + "login-password.png"), QLineEdit::LeadingPosition);
        username_->installEventFilter(this);
        password_->installEventFilter(this);
        auto* remember = new QCheckBox(QString::fromUtf8("记住密码"), form);
        remember->setObjectName("lanhuRememberPassword");
        remember->setGeometry(24, 254, 180, 26);
        remember->setEnabled(false);
        remember->setToolTip(QString::fromUtf8("尚未接入安全凭据存储，不保存密码"));
        auto* keyboard = new QPushButton(form);
        keyboard->setObjectName("lanhuKeyboard");
        keyboard->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
        keyboard->setToolTip(QString::fromUtf8("触摸键盘"));
        keyboard->setAccessibleName(QString::fromUtf8("触摸键盘"));
        keyboard->setGeometry(387, 250, 36, 32);
        keyboard->setStyleSheet("background:#F0F7FB;border:1px solid #D5E7F2;");
        activeInput_ = username_;
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* current) {
            if (current == username_ || current == password_) activeInput_ = static_cast<QLineEdit*>(current);
        });
        connect(keyboard, &QPushButton::clicked, this, [this]() { showKeyboard(); });
        error_ = new QLabel(form);
        error_->setObjectName("lanhuLoginError");
        error_->setGeometry(24, 287, 399, 38);
        error_->setWordWrap(true);
        error_->setStyleSheet("color:#D93550;font-size:14px;");
        auto* login = new QPushButton(QString::fromUtf8("登录"), form);
        login->setObjectName("lanhuSubmit");
        login->setGeometry(24, 334, 399, 59);
        connect(login, &QPushButton::clicked, this, [this]() { submit(); });
        connect(password_, &QLineEdit::returnPressed, this, [this]() { submit(); });
        connect(username_, &QLineEdit::returnPressed, password_, [this]() { password_->setFocus(); });
        auto* back = new QPushButton;
        back->setObjectName("lanhuLoginBack");
        back->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
        back->setToolTip(QString::fromUtf8("返回"));
        back->setAccessibleName(QString::fromUtf8("返回"));
        back->setFixedSize(44, 36);
        back->setStyleSheet("background:white;border:1px solid #D5E7F2;border-radius:5px;");
        scene->addWidget(back)->setPos(1372, 18);
        connect(back, &QPushButton::clicked, this, &QDialog::reject);
        scene->addWidget(form)->setPos(883.5, 180);
    }
protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (view_ && watched == view_->viewport() && event->type() == QEvent::Resize)
            view_->fitInView(QRectF(0, 0, 1440, 810), Qt::KeepAspectRatio);
        if ((event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonPress) &&
            (watched == username_ || watched == password_))
            activeInput_ = static_cast<QLineEdit*>(watched);
        return QDialog::eventFilter(watched, event);
    }
    void resizeEvent(QResizeEvent* event) override {
        QDialog::resizeEvent(event);
        view_->fitInView(QRectF(0, 0, 1440, 810), Qt::KeepAspectRatio);
    }
private:
    void submit() {
        std::string message;
        auto bytes = password_->text().toUtf8();
        const bool accepted = session_.authenticate(username_->text().trimmed().toStdString(),
            bytes.toStdString(), QDateTime::currentMSecsSinceEpoch(), &message);
        bytes.fill('\0');
        password_->clear();
        if (accepted) accept();
        else { error_->setText(QString::fromStdString(message)); activeInput_ = password_; password_->setFocus(); }
    }
    void showKeyboard() {
        QDialog keyboard(this);
        keyboard.setObjectName("lanhuTouchKeyboard");
        keyboard.setWindowTitle(QString::fromUtf8("触摸键盘"));
        keyboard.setStyleSheet("QDialog{background:#EDF2F6;} QPushButton{background:white;color:#303438;"
                              "border:1px solid #CAD5DF;border-radius:5px;min-width:44px;min-height:40px;font-size:18px;}");
        auto* grid = new QGridLayout(&keyboard);
        const QStringList rows = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm", "!#-_.@"};
        std::vector<QPushButton*> keys;
        for (int r = 0; r < rows.size(); ++r) for (int c = 0; c < rows[r].size(); ++c) {
            auto* key = new QPushButton(rows[r].mid(c, 1), &keyboard);
            keys.push_back(key);
            grid->addWidget(key, r, c);
            connect(key, &QPushButton::clicked, &keyboard, [this, key]() { activeInput_->insert(key->text()); });
        }
        auto* shift = new QPushButton("Shift", &keyboard);
        shift->setCheckable(true);
        grid->addWidget(shift, 5, 0, 1, 2);
        connect(shift, &QPushButton::toggled, &keyboard, [keys](bool upper) {
            for (auto* key : keys) key->setText(upper ? key->text().toUpper() : key->text().toLower());
        });
        auto* erase = new QPushButton(QString::fromUtf8("退格"), &keyboard);
        grid->addWidget(erase, 5, 2, 1, 2);
        connect(erase, &QPushButton::clicked, &keyboard, [this]() { activeInput_->backspace(); });
        auto* field = new QPushButton(QString::fromUtf8("切换账号/密码"), &keyboard);
        grid->addWidget(field, 5, 4, 1, 4);
        connect(field, &QPushButton::clicked, &keyboard, [this, &keyboard]() {
            activeInput_ = activeInput_ == username_ ? password_ : username_;
            keyboard.setWindowTitle(activeInput_->accessibleName());
        });
        auto* done = new QPushButton(QString::fromUtf8("完成"), &keyboard);
        grid->addWidget(done, 5, 8, 1, 2);
        connect(done, &QPushButton::clicked, &keyboard, &QDialog::accept);
        keyboard.exec();
        activeInput_->setFocus();
    }
    ScadaLocalAccessSession& session_;
    QGraphicsView* view_ = nullptr;
    QLineEdit* username_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLineEdit* activeInput_ = nullptr;
    QLabel* error_ = nullptr;
};
}

QDialog* createLanhuLocalLogin(QWidget* parent, ScadaLocalAccessSession& session, const std::string& assets) {
    return new LanhuLoginDialog(parent, session, assets);
}

edge_gateway::ScadaScreen makeLanhuAccessGate(const edge_gateway::ScadaScreen& protectedScreen) {
    auto gate = protectedScreen;
    gate.background.clear();
    gate.widgets.clear();
    // Rebuild only the allowlisted chrome. Never instantiate protected controls behind an overlay.
    for (const auto& widget : protectedScreen.widgets) {
        const auto& id = widget.widgetId;
        if (id == "lanhu-background" || id == "lanhu-clock" || id == "lanhu-logo" ||
            id == "lanhu-brand" || id == "lanhu-brand-separator" || id == "lanhu-machine" ||
            id == "lanhu-header-rule" || id == "lanhu-login" || id.compare(0, 10, "lanhu-nav-") == 0) {
            auto chrome = widget;
            chrome.bindings.clear();
            chrome.stateRules.clear();
            if (chrome.action.type != "navigate") chrome.action = {};
            gate.widgets.push_back(std::move(chrome));
        }
    }
    edge_gateway::ScadaWidget body;
    body.widgetId = "access-gate-body"; body.type = "qtFrame"; body.visible = true;
    body.geometry = {132, 155, 1271, 614};
    body.properties = {{"qtBackgroundColor", "#FFFFFF"}, {"qtCornerRadius", "8"}};
    gate.widgets.push_back(body);
    edge_gateway::ScadaWidget label;
    label.widgetId = "access-gate-label"; label.type = "qtLabel"; label.visible = true;
    label.geometry = {568, 470, 399, 26}; label.zIndex = 5;
    label.properties = {{"qtText", "登录后可查看此界面"}, {"qtFontSize", "16"},
        {"qtTextColor", "#303438"}, {"qtTransparent", "true"},
        {"qtTextAlignment", "Center"}, {"qtVerticalAlignment", "Center"}};
    gate.widgets.push_back(label);
    auto button = label;
    button.widgetId = "access-gate-login"; button.type = "qtButton";
    button.geometry = {568, 543, 399, 59};
    button.properties = {{"qtText", "去登录"}, {"qtTheme", "light"}, {"qtFontSize", "18"},
        {"qtFontWeight", "Bold"}, {"qtBackgroundColor", "#129AF5"},
        {"qtTextColor", "#FFFFFF"}, {"qtCornerRadius", "5"}, {"qtBorderColor", "#129AF5"}};
    button.action.type = "navigate"; button.action.targetScreen = "__local_login__";
    gate.widgets.push_back(button);
    return gate;
}

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
    if (nowMs < lockedUntilMs_) {
        if (message != nullptr) *message = "登录失败次数过多，请稍后再试";
        return false;
    }
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
        if (++failedAttempts_ >= kMaxFailedAttempts) {
            failedAttempts_ = 0;
            lockoutRound_ = std::min(lockoutRound_ + 1, 7);
            lockedUntilMs_ = nowMs + (static_cast<std::int64_t>(30000) << (lockoutRound_ - 1));
        }
        if (message != nullptr) *message = "账号或密码错误";
        return false;
    }
    failedAttempts_ = 0;
    lockoutRound_ = 0;
    lockedUntilMs_ = 0;
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

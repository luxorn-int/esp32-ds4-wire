#include "main_window.h"

#include <QColorDialog>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSerialPortInfo>
#include <QSlider>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>
#include <QTime>
#include <QVBoxLayout>
#include <QWidget>
#include <QtEndian>

namespace {
QString stateName(quint8 state)
{
    switch (state) {
    case BridgeProtocol::Scanning: return QStringLiteral("Поиск геймпада");
    case BridgeProtocol::Connecting: return QStringLiteral("Подключение");
    case BridgeProtocol::Connected: return QStringLiteral("Геймпад подключён");
    case BridgeProtocol::Pairing: return QStringLiteral("Режим сопряжения");
    case BridgeProtocol::Disconnected: return QStringLiteral("Геймпад не подключён");
    default: return QStringLiteral("Ожидание платы");
    }
}

quint16 readLe16(const char *data)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(data));
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("ESP32 Gamepad Bridge"));
    resize(1050, 760);
    setMinimumSize(850, 620);
    buildUi();

    connect(&m_serial, &QSerialPort::readyRead, this, [this] { handleSerialData(); });
    connect(&m_serial, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError error) {
        if (error == QSerialPort::NoError || !m_serial.isOpen())
            return;
        appendLog(QStringLiteral("Ошибка последовательного порта: %1").arg(m_serial.errorString()),
                  true);
        m_serial.close();
        m_connectButton->setText(QStringLiteral("Подключить"));
        updateOutputControls();
    });

    refreshPorts();
    const QString defaultProfile = QCoreApplication::applicationDirPath()
        + QStringLiteral("/profiles/ds4_bluetooth.json");
    QString error;
    const auto profile = ReportProfile::load(defaultProfile, &error);
    if (profile) {
        for (int slot = 0; slot < BridgeProtocol::GamepadSlots; ++slot)
            setProfile(slot, *profile);
    } else {
        appendLog(QStringLiteral("Профиль DS4 по умолчанию не загружен: %1").arg(error), true);
    }
    m_virtualLabel->setText(m_virtualDs4.statusText());
    appendLog(m_virtualDs4.statusText());
    updateOutputControls();

    for (int slot = 0; slot < BridgeProtocol::GamepadSlots; ++slot) {
        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setTimerType(Qt::PreciseTimer);
        connect(timer, &QTimer::timeout, this, [this, slot] { flushOutput(slot); });
        m_outputTimer[slot] = timer;
    }
    // Called on a ViGEm worker thread: hop to the GUI thread before touching anything.
    m_virtualDs4.setOutputCallback([this](int slot, quint8 largeMotor, quint8 smallMotor,
                                          quint8 red, quint8 green, quint8 blue) {
        QMetaObject::invokeMethod(this, [this, slot, largeMotor, smallMotor, red, green, blue] {
            onVirtualOutput(slot, largeMotor, smallMotor, red, green, blue);
        }, Qt::QueuedConnection);
    });
}

MainWindow::~MainWindow()
{
    m_virtualDs4.close(); // no more callbacks from the ViGEm threads after this
    if (m_captureFile.isOpen())
        m_captureFile.close();
    m_serial.close();
}

QWidget *MainWindow::makePanel(const QString &title, QVBoxLayout **body)
{
    auto *panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("panel"));
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(16, 14, 16, 16);
    layout->setSpacing(10);
    auto *heading = new QLabel(title, panel);
    heading->setObjectName(QStringLiteral("panelTitle"));
    layout->addWidget(heading);
    if (body)
        *body = layout;
    return panel;
}

void MainWindow::buildUi()
{
    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget { background: #101722; color: #e7edf5; font-family: "Segoe UI"; font-size: 10pt; }
        QWidget#panel { background: #182331; border: 1px solid #263649; border-radius: 12px; }
        QLabel#appTitle { font-size: 22pt; font-weight: 650; }
        QLabel#panelTitle { background: transparent; font-size: 12pt; font-weight: 600; color: #f2f6fb; }
        QLabel#muted { color: #93a3b7; }
        QLabel#status { color: #58c7b7; font-size: 11pt; font-weight: 600; }
        QPushButton { background: #243245; border: 1px solid #30445d; border-radius: 7px; padding: 8px 12px; }
        QPushButton:hover { background: #2b3c52; }
        QPushButton:disabled { color: #748396; background: #1e2b3b; border-color: #263649; }
        QPushButton#primary { background: #286f6a; border-color: #328b83; font-weight: 600; }
        QPushButton#primary:hover { background: #328b83; }
        QComboBox, QLineEdit, QSpinBox { background: #1e2b3b; border: 1px solid #30445d; border-radius: 6px; padding: 7px; }
        QComboBox QAbstractItemView { background: #1e2b3b; selection-background-color: #286f6a; }
        QSlider::groove:horizontal { height: 6px; background: #253449; border-radius: 3px; }
        QSlider::sub-page:horizontal { background: #58c7b7; border-radius: 3px; }
        QSlider::handle:horizontal { width: 14px; margin: -5px 0; border-radius: 7px; background: #e7edf5; }
        QPlainTextEdit { background: #0c131c; color: #c6d7e7; border: none; border-radius: 7px; padding: 10px; font-family: "Cascadia Mono", monospace; font-size: 9pt; }
    )"));

    auto *central = new QWidget(this);
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(22, 18, 22, 22);
    outer->setSpacing(12);

    auto *header = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("ESP32 Gamepad Bridge"), central);
    title->setObjectName(QStringLiteral("appTitle"));
    auto *subtitle = new QLabel(QStringLiteral("Bluetooth HID  →  USB/UART  →  виртуальный DualShock 4"), central);
    subtitle->setObjectName(QStringLiteral("muted"));
    header->addWidget(title);
    header->addSpacing(12);
    header->addWidget(subtitle);
    header->addStretch();
    outer->addLayout(header);

    QVBoxLayout *connectionBody = nullptr;
    auto *connectionPanel = makePanel(QStringLiteral("Подключение к ESP32"), &connectionBody);
    auto *connectionRow = new QHBoxLayout;
    m_portCombo = new QComboBox(connectionPanel);
    m_portCombo->setMinimumWidth(250);
    auto *refresh = new QPushButton(QStringLiteral("Обновить список"), connectionPanel);
    m_baudSpin = new QSpinBox(connectionPanel);
    m_baudSpin->setRange(1200, 4000000);
    m_baudSpin->setValue(921600);
    m_baudSpin->setMinimumWidth(125);
    m_connectButton = new QPushButton(QStringLiteral("Подключить"), connectionPanel);
    m_connectButton->setObjectName(QStringLiteral("primary"));
    connectionRow->addWidget(new QLabel(QStringLiteral("COM-порт"), connectionPanel));
    connectionRow->addWidget(m_portCombo, 1);
    connectionRow->addWidget(refresh);
    connectionRow->addSpacing(10);
    connectionRow->addWidget(new QLabel(QStringLiteral("Скорость"), connectionPanel));
    connectionRow->addWidget(m_baudSpin);
    connectionRow->addWidget(m_connectButton);
    connectionBody->addLayout(connectionRow);
    outer->addWidget(connectionPanel);

    auto *columns = new QHBoxLayout;
    columns->setSpacing(12);
    QVBoxLayout *controllerBody = nullptr;
    auto *controllerPanel = makePanel(QStringLiteral("Контроллеры и профили"), &controllerBody);
    m_connectionLabel = new QLabel(QStringLiteral("Ожидание платы"), controllerPanel);
    m_connectionLabel->setObjectName(QStringLiteral("status"));
    m_controllerLabel = new QLabel(QStringLiteral("Контроллеры: не обнаружены"), controllerPanel);
    m_controllerLabel->setObjectName(QStringLiteral("muted"));
    m_bondsLabel = new QLabel(QStringLiteral("Сохранённых сопряжений: —"), controllerPanel);
    m_bondsLabel->setObjectName(QStringLiteral("muted"));
    controllerBody->addWidget(m_connectionLabel);
    controllerBody->addWidget(m_controllerLabel);
    controllerBody->addWidget(m_bondsLabel);
    m_pairButton = new QPushButton(QStringLiteral("Начать поиск и сопряжение"), controllerPanel);
    controllerBody->addWidget(m_pairButton, 0, Qt::AlignLeft);

    auto *separator = new QWidget(controllerPanel);
    separator->setFixedHeight(1);
    separator->setStyleSheet(QStringLiteral("background:#304154"));
    controllerBody->addWidget(separator);
    for (int slot = 0; slot < BridgeProtocol::GamepadSlots; ++slot) {
        auto *row = new QHBoxLayout;
        auto *loadButton = new QPushButton(QStringLiteral("Профиль геймпада %1").arg(slot + 1),
                                           controllerPanel);
        m_profileLabels[slot] = new QLabel(QStringLiteral("не загружен"), controllerPanel);
        m_profileLabels[slot]->setObjectName(QStringLiteral("muted"));
        row->addWidget(loadButton);
        row->addWidget(m_profileLabels[slot], 1);
        controllerBody->addLayout(row);
        connect(loadButton, &QPushButton::clicked, this, [this, slot] { loadProfile(slot); });
    }
    m_virtualLabel = new QLabel(controllerPanel);
    m_virtualLabel->setWordWrap(true);
    m_virtualLabel->setObjectName(QStringLiteral("muted"));
    controllerBody->addWidget(m_virtualLabel);

    QVBoxLayout *outputBody = nullptr;
    auto *outputPanel = makePanel(QStringLiteral("Команды геймпаду"), &outputBody);
    auto *targetRow = new QHBoxLayout;
    targetRow->addWidget(new QLabel(QStringLiteral("Целевой слот"), outputPanel));
    m_slotCombo = new QComboBox(outputPanel);
    m_slotCombo->addItems({QStringLiteral("Геймпад 1"), QStringLiteral("Геймпад 2")});
    targetRow->addWidget(m_slotCombo);
    targetRow->addStretch();
    outputBody->addLayout(targetRow);

    auto *colorRow = new QHBoxLayout;
    colorRow->addWidget(new QLabel(QStringLiteral("Подсветка"), outputPanel));
    m_colorSwatch = new QLabel(QStringLiteral("     "), outputPanel);
    m_colorSwatch->setFixedSize(36, 24);
    m_colorSwatch->setStyleSheet(QStringLiteral("border:1px solid #52657b; border-radius:4px;"));
    m_colorLabel = new QLabel(outputPanel);
    auto *chooseColor = new QPushButton(QStringLiteral("Выбрать цвет"), outputPanel);
    colorRow->addWidget(m_colorSwatch);
    colorRow->addWidget(m_colorLabel);
    colorRow->addStretch();
    colorRow->addWidget(chooseColor);
    outputBody->addLayout(colorRow);

    auto *brightnessLabel = new QLabel(QStringLiteral("Яркость 100%"), outputPanel);
    m_brightnessSlider = new QSlider(Qt::Horizontal, outputPanel);
    m_brightnessSlider->setRange(0, 100);
    m_brightnessSlider->setValue(100);
    outputBody->addWidget(brightnessLabel);
    outputBody->addWidget(m_brightnessSlider);
    m_sendColorButton = new QPushButton(QStringLiteral("Отправить цвет"), outputPanel);
    outputBody->addWidget(m_sendColorButton, 0, Qt::AlignLeft);

    auto *rumbleLabel = new QLabel(QStringLiteral("Вибрация: 0 / 255"), outputPanel);
    m_rumbleSlider = new QSlider(Qt::Horizontal, outputPanel);
    m_rumbleSlider->setRange(0, 255);
    m_rumbleSlider->setValue(0);
    m_sendRumbleButton = new QPushButton(QStringLiteral("Отправить вибрацию"), outputPanel);
    outputBody->addWidget(rumbleLabel);
    outputBody->addWidget(m_rumbleSlider);
    outputBody->addWidget(m_sendRumbleButton, 0, Qt::AlignLeft);
    auto *profileHint = new QLabel(
        QStringLiteral("Формат команд HID зависит от модели контроллера и выбранного профиля."),
        outputPanel);
    profileHint->setObjectName(QStringLiteral("muted"));
    profileHint->setWordWrap(true);
    outputBody->addWidget(profileHint);

    columns->addWidget(controllerPanel, 1);
    columns->addWidget(outputPanel, 1);
    outer->addLayout(columns);

    QVBoxLayout *reportsBody = nullptr;
    auto *reportsPanel = makePanel(QStringLiteral("HID-отчёты"), &reportsBody);
    auto *reportHeader = new QHBoxLayout;
    m_reportSummary = new QLabel(QStringLiteral("Пока нет данных"), reportsPanel);
    m_reportSummary->setObjectName(QStringLiteral("muted"));
    m_captureLabel = new QLabel(QStringLiteral("Запись выключена"), reportsPanel);
    m_captureLabel->setObjectName(QStringLiteral("muted"));
    m_captureButton = new QPushButton(QStringLiteral("Начать запись"), reportsPanel);
    reportHeader->addWidget(m_reportSummary, 1);
    reportHeader->addWidget(m_captureLabel);
    reportHeader->addWidget(m_captureButton);
    reportsBody->addLayout(reportHeader);
    m_log = new QPlainTextEdit(reportsPanel);
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(100);
    m_log->setMinimumHeight(130);
    reportsBody->addWidget(m_log, 1);
    outer->addWidget(reportsPanel, 1);
    setCentralWidget(central);

    connect(refresh, &QPushButton::clicked, this, [this] { refreshPorts(); });
    connect(m_connectButton, &QPushButton::clicked, this, [this] { toggleConnection(); });
    connect(m_pairButton, &QPushButton::clicked, this, [this] { togglePairing(); });
    connect(m_slotCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        m_selectedSlot = qBound(0, index, BridgeProtocol::GamepadSlots - 1);
        showColor();
        updateOutputControls();
    });

    connect(chooseColor, &QPushButton::clicked, this, [this] {
        const QColor chosen = QColorDialog::getColor(m_colors[m_selectedSlot], this,
                                                      QStringLiteral("Цвет подсветки"));
        if (chosen.isValid()) {
            m_colors[m_selectedSlot] = chosen;
            showColor();
        }
    });
    connect(m_brightnessSlider, &QSlider::valueChanged, this,
            [brightnessLabel](int value) {
                brightnessLabel->setText(QStringLiteral("Яркость %1%").arg(value));
            });
    connect(m_rumbleSlider, &QSlider::valueChanged, this, [rumbleLabel](int value) {
        rumbleLabel->setText(QStringLiteral("Вибрация: %1 / 255").arg(value));
    });
    connect(m_sendColorButton, &QPushButton::clicked, this, [this] { sendColor(); });
    connect(m_sendRumbleButton, &QPushButton::clicked, this, [this] { sendRumble(); });
    connect(m_captureButton, &QPushButton::clicked, this, [this] { toggleCapture(); });
    showColor();

    m_lagClock.start();
    auto *statsTimer = new QTimer(this);
    statsTimer->setTimerType(Qt::PreciseTimer);
    statsTimer->setInterval(10);
    connect(statsTimer, &QTimer::timeout, this, [this] { tickStats(); });
    statsTimer->start();
}

void MainWindow::refreshPorts()
{
    const QString previous = m_portCombo->currentData().toString();
    m_portCombo->clear();
    const auto ports = QSerialPortInfo::availablePorts();
    for (const auto &port : ports) {
        const QString description = port.description().isEmpty()
            ? port.portName() : port.description();
        m_portCombo->addItem(QStringLiteral("%1 — %2").arg(port.portName(), description),
                             port.portName());
    }
    const int previousIndex = m_portCombo->findData(previous);
    if (previousIndex >= 0)
        m_portCombo->setCurrentIndex(previousIndex);
    if (ports.isEmpty())
        appendLog(QStringLiteral("COM-порты не найдены."));
}

void MainWindow::toggleConnection()
{
    if (m_serial.isOpen()) {
        m_serial.close();
        m_connectButton->setText(QStringLiteral("Подключить"));
        m_connectionLabel->setText(QStringLiteral("Отключено"));
        m_pairingEnabled = false;
        m_pairButton->setText(QStringLiteral("Начать поиск и сопряжение"));
        updateOutputControls();
        appendLog(QStringLiteral("Соединение с ESP32 закрыто."));
        return;
    }
    if (m_portCombo->currentData().toString().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Нет COM-порта"),
                             QStringLiteral("Выберите COM-порт ESP32."));
        return;
    }

    m_serial.setPortName(m_portCombo->currentData().toString());
    const int baudRate = m_baudSpin->value();
    m_serial.setBaudRate(baudRate);
    m_serial.setDataBits(QSerialPort::Data8);
    m_serial.setParity(QSerialPort::NoParity);
    m_serial.setStopBits(QSerialPort::OneStop);
    m_serial.setFlowControl(QSerialPort::NoFlowControl);
    if (!m_serial.open(QIODevice::ReadWrite)) {
        QMessageBox::critical(this, QStringLiteral("Ошибка подключения"),
                              QStringLiteral("Не удалось открыть %1: %2")
                                  .arg(m_serial.portName(), m_serial.errorString()));
        return;
    }
    // Many ESP32 DevKit boards reset when DTR/RTS toggle on port open.
    // Release both lines so the board is not held in reset/bootloader.
    m_serial.setDataTerminalReady(false);
    m_serial.setRequestToSend(false);
    m_parser = BridgeProtocol::FrameParser{};
    m_connectButton->setText(QStringLiteral("Отключить"));
    m_connectionLabel->setText(QStringLiteral("Соединено с платой"));
    sendFrame(BridgeProtocol::Hello);
    // If the board did reboot on open, the first Hello is lost: repeat it.
    QTimer::singleShot(1500, this, [this] {
        if (m_serial.isOpen())
            sendFrame(BridgeProtocol::Hello);
    });
    appendLog(QStringLiteral("Открыт %1 на скорости %2.").arg(m_serial.portName()).arg(baudRate));
    updateOutputControls();
}

void MainWindow::togglePairing()
{
    if (!m_serial.isOpen()) {
        QMessageBox::information(this, QStringLiteral("Нет соединения"),
                                 QStringLiteral("Сначала подключите ESP32 по USB."));
        return;
    }
    const bool enable = !m_pairingEnabled;
    if (sendFrame(BridgeProtocol::PairMode, QByteArray(1, char(enable ? 1 : 0)))) {
        m_pairingEnabled = enable;
        m_pairButton->setText(enable ? QStringLiteral("Остановить поиск")
                                     : QStringLiteral("Начать поиск и сопряжение"));
        appendLog(enable ? QStringLiteral("Включён поиск геймпада.")
                         : QStringLiteral("Режим поиска выключен."));
    }
}

void MainWindow::loadProfile(int slot)
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Выберите JSON-профиль геймпада"), {},
        QStringLiteral("Профили JSON (*.json);;Все файлы (*)"));
    if (path.isEmpty())
        return;
    QString error;
    const auto profile = ReportProfile::load(path, &error);
    if (!profile) {
        QMessageBox::critical(this, QStringLiteral("Некорректный профиль"), error);
        return;
    }
    setProfile(slot, *profile);
}

void MainWindow::setProfile(int slot, const ReportProfile &profile)
{
    if (slot < 0 || slot >= m_profiles.size())
        return;
    m_profiles[slot] = profile;
    m_profileLabels[slot]->setText(profile.name);
    updateOutputControls();
    appendLog(QStringLiteral("Для геймпада %1 загружен профиль «%2».")
                  .arg(slot + 1).arg(profile.name));
}

void MainWindow::sendColor()
{
    const auto &profile = m_profiles[m_selectedSlot];
    if (!profile) {
        QMessageBox::information(this, QStringLiteral("Нужен профиль"),
                                 QStringLiteral("Сначала загрузите профиль этого геймпада."));
        return;
    }
    const int scale = m_brightnessSlider->value();
    QString error;
    const auto report = profile->buildLightbarReport(
        qRound(m_colors[m_selectedSlot].red() * scale / 100.0),
        qRound(m_colors[m_selectedSlot].green() * scale / 100.0),
        qRound(m_colors[m_selectedSlot].blue() * scale / 100.0), &error);
    if (!report) {
        QMessageBox::information(this, QStringLiteral("Подсветка недоступна"),
                                 error.isEmpty() ? QStringLiteral("В профиле нет HID-команды света.")
                                                 : error);
        return;
    }
    sendOutput(m_selectedSlot, *report);
}

void MainWindow::sendRumble()
{
    const auto &profile = m_profiles[m_selectedSlot];
    if (!profile) {
        QMessageBox::information(this, QStringLiteral("Нужен профиль"),
                                 QStringLiteral("Сначала загрузите профиль этого геймпада."));
        return;
    }
    QString error;
    const int value = m_rumbleSlider->value();
    const auto report = profile->buildRumbleReport(value, value, &error);
    if (!report) {
        QMessageBox::information(this, QStringLiteral("Вибрация недоступна"),
                                 error.isEmpty() ? QStringLiteral("В профиле нет HID-команды вибрации.")
                                                 : error);
        return;
    }
    sendOutput(m_selectedSlot, *report);
}

bool MainWindow::sendOutput(int slot, const OutputReport &report, bool logIt,
                            quint16 *usedSequence)
{
    QByteArray payload;
    payload.append(char(slot));
    payload.append(char(report.mapIndex));
    char word[2];
    qToLittleEndian(report.reportId, reinterpret_cast<uchar *>(word));
    payload.append(word, 2);
    qToLittleEndian(quint16(report.data.size()), reinterpret_cast<uchar *>(word));
    payload.append(word, 2);
    payload.append(report.data);
    if (!sendFrame(BridgeProtocol::HidOutput, payload, usedSequence))
        return false;
    if (logIt)
        appendLog(QStringLiteral("Команда HID отправлена слоту %1: report %2, %3 байт.")
                      .arg(slot + 1).arg(report.reportId).arg(report.data.size()));
    return true;
}

// ---- Rumble / lightbar from the host (Steam) to the real pad ----------------

void MainWindow::onVirtualOutput(int slot, quint8 largeMotor, quint8 smallMotor, quint8 red,
                                 quint8 green, quint8 blue)
{
    if (slot < 0 || slot >= BridgeProtocol::GamepadSlots)
        return;
    WantedOutput &wanted = m_wanted[slot];
    if (wanted.valid && wanted.strong == largeMotor && wanted.weak == smallMotor
        && wanted.red == red && wanted.green == green && wanted.blue == blue)
        return; // hosts repeat the same state all the time
    wanted.weak = smallMotor;     // DS4 "small" motor = right/light = profile "weak"
    wanted.strong = largeMotor;   // DS4 "large" motor = left/heavy  = profile "strong"
    wanted.red = red;
    wanted.green = green;
    wanted.blue = blue;
    wanted.valid = true;
    scheduleOutputFlush(slot);
}

// Leading edge: send at once when the last send is old enough. Otherwise one
// timer sends the LATEST wanted state at the end of the interval, so a "rumble
// off" is never lost and the Bluetooth link is not flooded.
void MainWindow::scheduleOutputFlush(int slot)
{
    constexpr qint64 minIntervalMs = 10;
    m_outputDirty[slot] = true;
    if (m_outputTimer[slot] && m_outputTimer[slot]->isActive())
        return;
    const qint64 sinceLast = m_outputClock[slot].isValid() ? m_outputClock[slot].elapsed()
                                                           : minIntervalMs;
    if (sinceLast >= minIntervalMs || !m_outputTimer[slot])
        flushOutput(slot);
    else
        m_outputTimer[slot]->start(int(minIntervalMs - sinceLast));
}

void MainWindow::flushOutput(int slot)
{
    if (!m_outputDirty[slot])
        return;
    const WantedOutput &wanted = m_wanted[slot];
    const auto &profile = m_profiles[slot];
    if (!wanted.valid || !profile || !m_serial.isOpen() || !m_controllers[slot].connected)
        return; // kept in m_wanted: re-sent when the pad (re)connects
    m_outputDirty[slot] = false;

    const auto sendSilently = [&](const std::optional<OutputReport> &report) {
        if (!report)
            return;
        quint16 sequence = 0;
        if (sendOutput(slot, *report, false, &sequence)) {
            if (m_silentAckSeq.size() > 256)
                m_silentAckSeq.clear();
            m_silentAckSeq.insert(sequence);
        }
    };

    const auto combined = profile->buildCombinedReport(wanted.weak, wanted.strong, wanted.red,
                                                       wanted.green, wanted.blue);
    if (combined) {
        sendSilently(combined);
    } else {
        // Templates cannot be merged: send separately (each may reset the other).
        if (profile->hasRumble())
            sendSilently(profile->buildRumbleReport(wanted.weak, wanted.strong));
        if (profile->hasLightbar())
            sendSilently(profile->buildLightbarReport(wanted.red, wanted.green, wanted.blue));
    }
    m_outputClock[slot].restart();
}

bool MainWindow::sendFrame(quint8 type, const QByteArray &payload, quint16 *usedSequence)
{
    if (!m_serial.isOpen()) {
        appendLog(QStringLiteral("Последовательный порт не подключён."), true);
        return false;
    }
    const quint16 sequence = m_sequence++;
    if (usedSequence)
        *usedSequence = sequence;
    const QByteArray packet = BridgeProtocol::encodeFrame(type, sequence, payload);
    if (packet.isEmpty()) {
        appendLog(QStringLiteral("Не удалось сформировать пакет: превышена длина данных."), true);
        return false;
    }
    if (m_serial.write(packet) != packet.size()) {
        appendLog(QStringLiteral("Ошибка передачи по COM-порту: %1").arg(m_serial.errorString()),
                  true);
        return false;
    }
    return true;
}

void MainWindow::handleSerialData()
{
    const auto frames = m_parser.feed(m_serial.readAll());
    for (const auto &frame : frames)
        handleFrame(frame);
}

void MainWindow::handleFrame(const BridgeProtocol::Frame &frame)
{
    switch (frame.type) {
    case BridgeProtocol::Status:
        handleStatus(frame.payload);
        break;
    case BridgeProtocol::HidInput:
        handleInputReport(frame.payload);
        break;
    case BridgeProtocol::Log:
        appendLog(QStringLiteral("ESP32: %1").arg(QString::fromUtf8(frame.payload)));
        break;
    case BridgeProtocol::Ack:
        if (frame.payload.size() >= 2) {
            const auto command = quint8(frame.payload[0]);
            const auto result = quint8(frame.payload[1]);
            if (command == BridgeProtocol::HidOutput && m_silentAckSeq.remove(frame.sequence)) {
                if (result != 0)
                    ++m_outputRejects; // summarized in tickStats, not one log line each
                break;
            }
            appendLog(result == 0
                ? QStringLiteral("ESP32 подтвердила команду 0x%1.")
                      .arg(command, 2, 16, QLatin1Char('0'))
                : QStringLiteral("ESP32 отклонила команду 0x%1 (код %2).")
                      .arg(command, 2, 16, QLatin1Char('0')).arg(result), result != 0);
        }
        break;
    default:
        break;
    }
}

void MainWindow::handleStatus(const QByteArray &payload)
{
    QString error;
    const auto status = BridgeProtocol::decodeStatus(payload, &error);
    if (!status) {
        appendLog(QStringLiteral("Некорректный статус ESP32: %1").arg(error), true);
        return;
    }
    m_bondsLabel->setText(QStringLiteral("Сохранённых сопряжений: %1").arg(status->bondCount));

    QStringList names;
    for (int slot = 0; slot < BridgeProtocol::GamepadSlots; ++slot) {
        if (!status->controllers[slot]) {
            m_controllers[slot] = {};
            m_virtualDs4.neutral(slot);
            continue;
        }
        m_controllers[slot] = *status->controllers[slot];
        if (m_controllers[slot].connected) {
            const QString name = m_controllers[slot].name.isEmpty()
                ? QStringLiteral("подключён") : m_controllers[slot].name;
            names.append(QStringLiteral("%1: %2").arg(slot + 1).arg(name));
        } else {
            m_virtualDs4.neutral(slot);
        }
    }
    for (int slot = 0; slot < BridgeProtocol::GamepadSlots; ++slot) {
        const bool nowConnected = m_controllers[slot].connected;
        if (nowConnected && !m_wasConnected[slot] && m_wanted[slot].valid) {
            // The firmware sets its default lightbar colour on connect; re-apply the
            // colour/rumble the host wants once that has happened.
            QTimer::singleShot(1500, this, [this, slot] {
                m_outputDirty[slot] = true;
                flushOutput(slot);
            });
        }
        m_wasConnected[slot] = nowConnected;
    }
    m_connectionLabel->setText(names.isEmpty()
        ? stateName(status->state)
        : QStringLiteral("Подключено геймпадов: %1/%2")
              .arg(names.size()).arg(BridgeProtocol::GamepadSlots));
    m_controllerLabel->setText(QStringLiteral("Контроллеры: %1")
        .arg(names.isEmpty() ? QStringLiteral("не обнаружены") : names.join(QStringLiteral("; "))));
    if (status->state == BridgeProtocol::Pairing)
        m_pairingEnabled = true;
    else if (status->state == BridgeProtocol::Scanning)
        m_pairingEnabled = false;
    m_pairButton->setText(m_pairingEnabled ? QStringLiteral("Остановить поиск")
                                           : QStringLiteral("Начать поиск и сопряжение"));
    updateOutputControls();
}

void MainWindow::handleInputReport(const QByteArray &payload)
{
    if (payload.size() < 6) {
        appendLog(QStringLiteral("Получен некорректный HID-отчёт."), true);
        return;
    }
    const int slot = quint8(payload[0]);
    if (slot >= BridgeProtocol::GamepadSlots) {
        appendLog(QStringLiteral("Получен HID-отчёт с неизвестного слота %1.").arg(slot), true);
        return;
    }
    const quint16 reportId = readLe16(payload.constData() + 1);
    const quint8 mapIndex = quint8(payload[3]);
    const quint16 usage = readLe16(payload.constData() + 4);
    const QByteArray report = payload.mid(6);
    ++m_rawReportCount;
    // Updating a label for every report (hundreds per second) loads the GUI
    // thread, which also reads the serial port and feeds ViGEm: update ~4x/s.
    if (!m_summaryClock.isValid() || m_summaryClock.elapsed() >= 250) {
        m_summaryClock.restart();
        m_reportSummary->setText(QStringLiteral("Геймпад %1 · ID %2 · map %3 · usage 0x%4 · %5 байт")
            .arg(slot + 1).arg(reportId).arg(mapIndex).arg(usage, 4, 16, QLatin1Char('0'))
            .arg(report.size()));
    }

    // Timing diagnostics: pause between consecutive reports of this pad.
    if (m_reportClockValid[slot]) {
        const qint64 gapMs = m_reportClock[slot].elapsed();
        if (gapMs < 3000 && gapMs > m_pcMaxGapMs[slot])
            m_pcMaxGapMs[slot] = gapMs;
    }
    m_reportClock[slot].restart();
    m_reportClockValid[slot] = true;
    ++m_pcCount[slot];

    if (m_captureFile.isOpen()) {
        QJsonObject entry;
        entry.insert(QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        entry.insert(QStringLiteral("slot"), slot);
        entry.insert(QStringLiteral("report_id"), reportId);
        entry.insert(QStringLiteral("map_index"), mapIndex);
        entry.insert(QStringLiteral("usage"), usage);
        entry.insert(QStringLiteral("data_hex"), QString::fromLatin1(report.toHex()));
        m_captureFile.write(QJsonDocument(entry).toJson(QJsonDocument::Compact));
        m_captureFile.write("\n");
    }

    const auto &profile = m_profiles[slot];
    if (!profile)
        return;
    const auto state = profile->mapInput(reportId, report);
    if (state && state->extendedReport.size() == 63) {
        // Touchpad coordinates as sent by the pad: X 0..1919, Y 0..941 over the full pad.
        const QByteArray &ext = state->extendedReport;
        const int touchPackets = qMin(int(quint8(ext[32])), 3);
        for (int i = 0; i < touchPackets; ++i) {
            for (int finger = 0; finger < 2; ++finger) {
                const int base = 33 + i * 9 + 1 + finger * 4;
                if (quint8(ext[base]) & 0x80)
                    continue; // finger not touching
                const int x = quint8(ext[base + 1]) | ((quint8(ext[base + 2]) & 0x0F) << 8);
                const int y = (quint8(ext[base + 2]) >> 4) | (quint8(ext[base + 3]) << 4);
                if (!m_touchSeen[slot]) {
                    m_touchSeen[slot] = true;
                    m_touchMinX[slot] = m_touchMaxX[slot] = x;
                    m_touchMinY[slot] = m_touchMaxY[slot] = y;
                } else {
                    m_touchMinX[slot] = qMin(m_touchMinX[slot], x);
                    m_touchMaxX[slot] = qMax(m_touchMaxX[slot], x);
                    m_touchMinY[slot] = qMin(m_touchMinY[slot], y);
                    m_touchMaxY[slot] = qMax(m_touchMaxY[slot], y);
                }
            }
        }
    }
    if (state) {
        QElapsedTimer applyTimer;
        applyTimer.start();
        m_virtualDs4.apply(slot, *state);
        const qint64 applyMs = applyTimer.elapsed();
        if (applyMs > m_applyMaxMs)
            m_applyMaxMs = applyMs;
    }
}

// Runs every 10 ms on the GUI thread: measures how late the event loop is
// (a long gap means the GUI thread was busy) and prints one summary per 2 s.
void MainWindow::tickStats()
{
    const qint64 now = m_lagClock.elapsed();
    const qint64 lag = now - m_lastTickMs - 10;
    m_lastTickMs = now;
    if (lag > m_loopMaxLagMs)
        m_loopMaxLagMs = lag;

    if (now - m_lastStatsMs < 2000)
        return;
    m_lastStatsMs = now;
    if (m_pcCount[0] || m_pcCount[1]) {
        appendLog(QStringLiteral("PC/2с: g1 n=%1 gap=%2мс | g2 n=%3 gap=%4мс | gui_lag=%5мс vigem=%6мс")
                      .arg(m_pcCount[0]).arg(m_pcMaxGapMs[0])
                      .arg(m_pcCount[1]).arg(m_pcMaxGapMs[1])
                      .arg(m_loopMaxLagMs).arg(m_applyMaxMs));
    }
    for (int i = 0; i < BridgeProtocol::GamepadSlots; ++i) {
        if (m_touchSeen[i]) {
            appendLog(QStringLiteral("Тачпад g%1: X %2…%3, Y %4…%5 (полный тачпад: X 0…1919, Y 0…941)")
                          .arg(i + 1).arg(m_touchMinX[i]).arg(m_touchMaxX[i])
                          .arg(m_touchMinY[i]).arg(m_touchMaxY[i]));
            m_touchSeen[i] = false;
        }
        m_pcCount[i] = 0;
        m_pcMaxGapMs[i] = 0;
    }
    if (m_outputRejects) {
        appendLog(QStringLiteral("Свет/вибрация от хоста: ESP32 отклонила %1 раз (пад отключён?)")
                      .arg(m_outputRejects));
        m_outputRejects = 0;
    }
    m_loopMaxLagMs = 0;
    m_applyMaxMs = 0;
}

void MainWindow::toggleCapture()
{
    if (m_captureFile.isOpen()) {
        m_captureFile.close();
        m_captureLabel->setText(QStringLiteral("Запись выключена"));
        m_captureButton->setText(QStringLiteral("Начать запись"));
        appendLog(QStringLiteral("Записано HID-отчётов: %1.").arg(m_rawReportCount));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Сохранить запись HID"), QStringLiteral("gamepad-reports.jsonl"),
        QStringLiteral("JSON Lines (*.jsonl);;Все файлы (*)"));
    if (path.isEmpty())
        return;
    m_captureFile.setFileName(path);
    if (!m_captureFile.open(QIODevice::WriteOnly | QIODevice::Append)) {
        QMessageBox::critical(this, QStringLiteral("Ошибка записи"), m_captureFile.errorString());
        return;
    }
    m_rawReportCount = 0;
    m_captureLabel->setText(QStringLiteral("Идёт запись"));
    m_captureButton->setText(QStringLiteral("Остановить запись"));
    appendLog(QStringLiteral("Запись HID-отчётов: %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::updateOutputControls()
{
    const bool canSend = m_serial.isOpen()
        && m_controllers.value(m_selectedSlot).connected
        && m_profiles.value(m_selectedSlot).has_value();
    const auto &profile = m_profiles[m_selectedSlot];
    m_sendColorButton->setEnabled(canSend && profile->hasLightbar());
    m_sendRumbleButton->setEnabled(canSend && profile->hasRumble());
}

void MainWindow::appendLog(const QString &message, bool isError)
{
    const QString line = QStringLiteral("[%1] %2")
        .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), message);
    if (isError)
        m_log->appendHtml(QStringLiteral("<span style='color:#ff8d88'>%1</span>")
                              .arg(line.toHtmlEscaped()));
    else
        m_log->appendPlainText(line);
}

void MainWindow::showColor()
{
    const QColor color = m_colors.value(m_selectedSlot, QColor(Qt::green));
    m_colorSwatch->setStyleSheet(QStringLiteral("background:%1; border:1px solid #52657b; border-radius:4px;")
                                     .arg(color.name()));
    m_colorLabel->setText(colorHex(m_selectedSlot));
}

QString MainWindow::colorHex(int slot) const
{
    return m_colors.value(slot).name().toUpper();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_captureFile.isOpen())
        m_captureFile.close();
    m_serial.close();
    m_virtualDs4.close();
    event->accept();
}
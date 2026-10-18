#pragma once

#include "protocol.h"
#include "profile.h"
#include "virtual_ds4.h"

#include <QColor>
#include <QElapsedTimer>
#include <QFile>
#include <QMainWindow>
#include <QMap>
#include <QSet>
#include <QSerialPort>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QSlider;
class QTimer;
class QCloseEvent;
class QVBoxLayout;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void buildUi();
    QWidget *makePanel(const QString &title, QVBoxLayout **body = nullptr);
    void refreshPorts();
    void toggleConnection();
    void togglePairing();
    void loadProfile(int slot);
    void setProfile(int slot, const ReportProfile &profile);
    void sendColor();
    void sendRumble();
    bool sendOutput(int slot, const OutputReport &report, bool logIt = true,
                    quint16 *usedSequence = nullptr);
    // Rumble / lightbar requests from the virtual DS4 (Steam etc.) -> real pad.
    void onVirtualOutput(int slot, quint8 largeMotor, quint8 smallMotor, quint8 red,
                         quint8 green, quint8 blue);
    void scheduleOutputFlush(int slot);
    void flushOutput(int slot);
    bool sendFrame(quint8 type, const QByteArray &payload = {}, quint16 *usedSequence = nullptr);
    void handleSerialData();
    void handleFrame(const BridgeProtocol::Frame &frame);
    void handleStatus(const QByteArray &payload);
    void handleInputReport(const QByteArray &payload);
    void toggleCapture();
    void updateOutputControls();
    void appendLog(const QString &message, bool isError = false);
    void showColor();
    void tickStats();
    QString colorHex(int slot) const;

    QSerialPort m_serial;
    BridgeProtocol::FrameParser m_parser;
    VirtualDS4 m_virtualDs4;
    QVector<std::optional<ReportProfile>> m_profiles =
        QVector<std::optional<ReportProfile>>(BridgeProtocol::GamepadSlots);
    QVector<BridgeProtocol::ControllerInfo> m_controllers =
        QVector<BridgeProtocol::ControllerInfo>(BridgeProtocol::GamepadSlots);
    QVector<QColor> m_colors{QColor(0, 255, 0), QColor(0, 96, 255)};
    quint16 m_sequence = 0;
    int m_selectedSlot = 0;
    quint64 m_rawReportCount = 0;
    bool m_pairingEnabled = false;
    QFile m_captureFile;

    // Timing diagnostics (PC side).
    QElapsedTimer m_reportClock[BridgeProtocol::GamepadSlots];
    bool m_reportClockValid[BridgeProtocol::GamepadSlots]{};
    quint32 m_pcCount[BridgeProtocol::GamepadSlots]{};
    qint64 m_pcMaxGapMs[BridgeProtocol::GamepadSlots]{};
    qint64 m_applyMaxMs = 0;
    qint64 m_loopMaxLagMs = 0;
    qint64 m_lastTickMs = 0;
    qint64 m_lastStatsMs = 0;
    QElapsedTimer m_lagClock;
    QElapsedTimer m_summaryClock;

    // Touchpad range seen in the last window (diagnostics for a dead touchpad area).
    bool m_touchSeen[BridgeProtocol::GamepadSlots]{};
    int m_touchMinX[BridgeProtocol::GamepadSlots]{};
    int m_touchMaxX[BridgeProtocol::GamepadSlots]{};
    int m_touchMinY[BridgeProtocol::GamepadSlots]{};
    int m_touchMaxY[BridgeProtocol::GamepadSlots]{};

    // Output (rumble + lightbar) requested by the host for each virtual pad.
    struct WantedOutput {
        quint8 weak = 0;
        quint8 strong = 0;
        quint8 red = 0;
        quint8 green = 0;
        quint8 blue = 0;
        bool valid = false;
    };
    WantedOutput m_wanted[BridgeProtocol::GamepadSlots];
    bool m_outputDirty[BridgeProtocol::GamepadSlots]{};
    QElapsedTimer m_outputClock[BridgeProtocol::GamepadSlots];
    QTimer *m_outputTimer[BridgeProtocol::GamepadSlots]{};
    bool m_wasConnected[BridgeProtocol::GamepadSlots]{};
    QSet<quint16> m_silentAckSeq;   // sequences of forwarded outputs whose ack is not logged
    quint32 m_outputRejects = 0;

    QComboBox *m_portCombo = nullptr;
    QComboBox *m_slotCombo = nullptr;
    QSpinBox *m_baudSpin = nullptr;
    QLabel *m_connectionLabel = nullptr;
    QLabel *m_controllerLabel = nullptr;
    QLabel *m_bondsLabel = nullptr;
    QLabel *m_virtualLabel = nullptr;
    QLabel *m_profileLabels[BridgeProtocol::GamepadSlots]{};
    QLabel *m_colorSwatch = nullptr;
    QLabel *m_colorLabel = nullptr;
    QLabel *m_reportSummary = nullptr;
    QLabel *m_captureLabel = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_pairButton = nullptr;
    QPushButton *m_sendColorButton = nullptr;
    QPushButton *m_sendRumbleButton = nullptr;
    QPushButton *m_captureButton = nullptr;
    QSlider *m_brightnessSlider = nullptr;
    QSlider *m_rumbleSlider = nullptr;
    QPlainTextEdit *m_log = nullptr;
};
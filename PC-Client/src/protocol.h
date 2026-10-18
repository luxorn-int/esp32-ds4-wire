#pragma once

#include <QByteArray>
#include <QVector>
#include <QtGlobal>

#include <optional>

namespace BridgeProtocol {

    constexpr quint8 Version = 2;
    constexpr qsizetype MaxPayload = 1024;
    constexpr int GamepadSlots = 2;

    enum FrameType : quint8 {
        Hello = 0x01,
        Status = 0x02,
        Ack = 0x03,
        HidInput = 0x10,
        PairMode = 0x20,
        HidOutput = 0x21,
        Log = 0x30
    };

    enum LinkState : quint8 {
        Unknown = 0,
        Scanning = 1,
        Connecting = 2,
        Connected = 3,
        Pairing = 4,
        Disconnected = 5
    };

    struct Frame {
        quint8 type = 0;
        quint16 sequence = 0;
        QByteArray payload;
    };

    struct ControllerInfo {
        bool connected = false;
        QByteArray address;
        QString name;
    };

    struct StatusInfo {
        quint8 state = Unknown;
        quint8 bondCount = 0;
        QVector<std::optional<ControllerInfo>> controllers =
            QVector<std::optional<ControllerInfo>>(GamepadSlots);
    };

    quint16 crc16CcittFalse(const QByteArray &data);
    QByteArray encodeFrame(quint8 type, quint16 sequence, const QByteArray &payload = {});
    std::optional<StatusInfo> decodeStatus(const QByteArray &payload, QString *error = nullptr);

    class FrameParser {
    public:
        QVector<Frame> feed(const QByteArray &chunk);
        quint64 crcErrors() const { return m_crcErrors; }
        quint64 discardedBytes() const { return m_discardedBytes; }

    private:
        QByteArray m_buffer;
        quint64 m_crcErrors = 0;
        quint64 m_discardedBytes = 0;
    };

} // namespace BridgeProtocol
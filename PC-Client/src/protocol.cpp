#include "protocol.h"

#include <QtEndian>

namespace BridgeProtocol {
namespace {
constexpr char Magic[] = {char(0xA5), char(0x5A), char(0xA5), char(0x5A)};
constexpr qsizetype HeaderSize = 10;
constexpr qsizetype FrameOverhead = 12;

quint16 readLe16(const char *p)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(p));
}

void setError(QString *out, const QString &message)
{
    if (out)
        *out = message;
}
} // namespace

quint16 crc16CcittFalse(const QByteArray &data)
{
    quint16 crc = 0xFFFF;
    for (const auto byte : data) {
        crc ^= quint16(quint8(byte)) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? quint16((crc << 1) ^ 0x1021) : quint16(crc << 1);
    }
    return crc;
}

QByteArray encodeFrame(quint8 type, quint16 sequence, const QByteArray &payload)
{
    if (payload.size() > MaxPayload)
        return {};

    QByteArray frame(Magic, 4);
    frame.append(char(Version));
    frame.append(char(type));
    char word[2];
    qToLittleEndian(sequence, reinterpret_cast<uchar *>(word));
    frame.append(word, 2);
    qToLittleEndian(quint16(payload.size()), reinterpret_cast<uchar *>(word));
    frame.append(word, 2);
    frame.append(payload);

    const quint16 crc = crc16CcittFalse(frame.mid(4));
    qToLittleEndian(crc, reinterpret_cast<uchar *>(word));
    frame.append(word, 2);
    return frame;
}

std::optional<StatusInfo> decodeStatus(const QByteArray &payload, QString *error)
{
    if (payload.size() < 3) {
        setError(error, QStringLiteral("Укороченный статус ESP32"));
        return std::nullopt;
    }

    StatusInfo result;
    result.state = quint8(payload[0]);
    result.bondCount = quint8(payload[1]);
    const int count = quint8(payload[2]);
    qsizetype offset = 3;

    for (int i = 0; i < count; ++i) {
        if (payload.size() - offset < 9) {
            setError(error, QStringLiteral("Обрезана запись контроллера в статусе ESP32"));
            return std::nullopt;
        }
        const int slot = quint8(payload[int(offset)]);
        ControllerInfo info;
        info.connected = quint8(payload[int(offset + 1)]) != 0;
        info.address = payload.mid(offset + 2, 6);
        const int nameLength = quint8(payload[int(offset + 8)]);
        offset += 9;
        if (payload.size() - offset < nameLength) {
            setError(error, QStringLiteral("Обрезано имя контроллера в статусе ESP32"));
            return std::nullopt;
        }
        info.name = QString::fromUtf8(payload.constData() + offset, nameLength);
        offset += nameLength;
        if (slot < GamepadSlots)
            result.controllers[slot] = info;
    }

    if (offset != payload.size()) {
        setError(error, QStringLiteral("В статусе ESP32 есть лишние байты"));
        return std::nullopt;
    }
    return result;
}

QVector<Frame> FrameParser::feed(const QByteArray &chunk)
{
    m_buffer.append(chunk);
    QVector<Frame> frames;

    while (true) {
        const qsizetype markerAt = m_buffer.indexOf(QByteArray(Magic, 4));
        if (markerAt < 0) {
            const qsizetype keep = qMin<qsizetype>(m_buffer.size(), 3);
            const qsizetype discard = m_buffer.size() - keep;
            if (discard > 0) {
                m_buffer.remove(0, discard);
                m_discardedBytes += quint64(discard);
            }
            break;
        }
        if (markerAt > 0) {
            m_buffer.remove(0, markerAt);
            m_discardedBytes += quint64(markerAt);
        }
        if (m_buffer.size() < HeaderSize)
            break;

        const quint8 version = quint8(m_buffer[4]);
        const quint8 type = quint8(m_buffer[5]);
        const quint16 sequence = readLe16(m_buffer.constData() + 6);
        const quint16 payloadLength = readLe16(m_buffer.constData() + 8);
        if (version != Version || payloadLength > MaxPayload) {
            m_buffer.remove(0, 1);
            ++m_discardedBytes;
            continue;
        }

        const qsizetype frameLength = FrameOverhead + payloadLength;
        if (m_buffer.size() < frameLength)
            break;

        const qsizetype crcEnd = HeaderSize + payloadLength;
        const quint16 expectedCrc = readLe16(m_buffer.constData() + crcEnd);
        const quint16 actualCrc = crc16CcittFalse(m_buffer.mid(4, crcEnd - 4));
        if (expectedCrc != actualCrc) {
            m_buffer.remove(0, 1);
            ++m_crcErrors;
            ++m_discardedBytes;
            continue;
        }

        frames.append(Frame{type, sequence, m_buffer.mid(HeaderSize, payloadLength)});
        m_buffer.remove(0, frameLength);
    }
    return frames;
}
} // namespace BridgeProtocol

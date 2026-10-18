#include "profile.h"
#include "protocol.h"
#include "virtual_ds4.h"

#include <QCoreApplication>
#include <QDebug>

#include <cstdlib>

namespace {
void check(bool ok, const char *message)
{
    if (!ok) {
        qCritical() << "FAILED:" << message;
        std::exit(1);
    }
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    using namespace BridgeProtocol;

    check(crc16CcittFalse("123456789") == 0x29B1, "CRC-16 reference vector");

    const QByteArray first = encodeFrame(HidInput, 0x1234, QByteArray::fromHex("00aaff"));
    FrameParser parser;
    check(parser.feed(first.left(7)).isEmpty(), "fragmented frame prefix");
    check(parser.feed(first.mid(7, 5)).isEmpty(), "fragmented frame header");
    const auto parsed = parser.feed(first.mid(12));
    check(parsed.size() == 1 && parsed[0].type == HidInput
              && parsed[0].sequence == 0x1234
              && parsed[0].payload == QByteArray::fromHex("00aaff"),
          "frame round trip");

    const QString profilePath = QStringLiteral("profiles/ds4_bluetooth.json");
    QString error;
    const auto profile = ReportProfile::load(profilePath, &error);
    check(profile.has_value(), "load bundled DS4 profile");
    check(profile->hasLightbar() && profile->hasRumble(), "profile output commands");

    QByteArray input(53, char(0));
    input[2] = char(128);
    input[3] = char(64);
    input[4] = char(192);
    input[5] = char(255);
    input[6] = char(0x28); // D-pad up + cross.
    input[8] = char(0x02); // Touchpad button.
    input[34] = char(1);   // One active touch sample.
    input[35] = char(7);   // Packet counter.
    input[36] = char(0x00); // Finger down, tracking id 0.
    input[37] = char(0x34);
    input[38] = char(0x12);
    const auto mapped = profile->mapInput(17, input);
    check(mapped.has_value(), "map configured input report");
    check(mapped->axes.value(QStringLiteral("lx")) == 128, "map stick axis");
    check(mapped->buttons.value(QStringLiteral("cross")), "map face button");
    check(mapped->extendedReport.size() == 63, "build ViGEm extended report");
    check(quint8(mapped->extendedReport[32]) == 1, "preserve touch count");
    check(mapped->extendedReport.mid(33, 9) == input.mid(35, 9), "preserve touch packet bytes");
    const QByteArray nativeReport = makeDs4Report(*mapped);
    check(nativeReport.size() == 63, "serialize native ViGEm DS4_REPORT_EX");
    check(nativeReport.mid(33, 9) == input.mid(35, 9),
          "keep touch packet in report sent to ViGEm");
    check(quint8(nativeReport[0]) == 128, "write mapped stick axes to extended report");

    const auto color = profile->buildLightbarReport(12, 34, 56, &error);
    check(color.has_value(), "build lightbar output");
    check(quint8(color->data[7]) == 12 && quint8(color->data[8]) == 34
              && quint8(color->data[9]) == 56,
          "place RGB values in HID report");

    const QByteArray noisy = QByteArray("noise") + encodeFrame(Status, 4, "ok");
    FrameParser recovery;
    const auto recovered = recovery.feed(noisy);
    check(recovered.size() == 1 && recovered[0].sequence == 4, "resynchronize after noise");

    qInfo() << "All bridge core checks passed.";
    return 0;
}

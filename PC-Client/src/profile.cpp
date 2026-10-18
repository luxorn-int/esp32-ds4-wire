#include "profile.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QtEndian>

#include <algorithm>

namespace {
constexpr const char *AxisNames[] = {"lx", "ly", "rx", "ry", "lt", "rt"};
constexpr const char *ButtonNames[] = {
    "cross", "circle", "square", "triangle", "l1", "r1", "l3", "r3",
    "share", "options", "ps", "touchpad", "dpad_up", "dpad_down",
    "dpad_left", "dpad_right"
};

void fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

bool isKnown(const QString &name, const char *const *names, qsizetype count)
{
    for (qsizetype i = 0; i < count; ++i) {
        if (name == QLatin1String(names[i]))
            return true;
    }
    return false;
}

bool readOffset(const QJsonObject &object, const QString &key, int *out)
{
    const auto value = object.value(key);
    if (!value.isDouble() || value.toInt(-1) < 0)
        return false;
    *out = value.toInt();
    return true;
}

quint32 crc32(const QByteArray &data)
{
    quint32 crc = 0xFFFFFFFFu;
    for (const auto byte : data) {
        crc ^= quint8(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return crc ^ 0xFFFFFFFFu;
}

int clampedByte(int value)
{
    return std::clamp(value, 0, 255);
}
} // namespace

std::optional<ReportProfile> ReportProfile::load(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(error, QStringLiteral("Не удалось открыть профиль: %1").arg(file.errorString()));
        return std::nullopt;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        fail(error, QStringLiteral("Некорректный JSON профиля: %1").arg(parseError.errorString()));
        return std::nullopt;
    }
    const auto root = document.object();
    if (!root.value(QStringLiteral("input")).isObject()
        || !root.value(QStringLiteral("output")).isObject()) {
        fail(error, QStringLiteral("Профиль должен содержать разделы input и output"));
        return std::nullopt;
    }

    ReportProfile profile;
    profile.name = root.value(QStringLiteral("name")).toString(QFileInfo(path).completeBaseName());
    profile.input = root.value(QStringLiteral("input")).toObject();
    profile.output = root.value(QStringLiteral("output")).toObject();

    const auto axes = profile.input.value(QStringLiteral("axes"));
    if (!axes.isUndefined()) {
        if (!axes.isObject()) {
            fail(error, QStringLiteral("input.axes должен быть объектом"));
            return std::nullopt;
        }
        const auto axisObject = axes.toObject();
        for (auto it = axisObject.begin(); it != axisObject.end(); ++it) {
            int offset = -1;
            const auto spec = it.value().toObject();
            if (!isKnown(it.key(), AxisNames, std::size(AxisNames))
                || !readOffset(spec, QStringLiteral("offset"), &offset)
                || spec.value(QStringLiteral("min")).toInt(0) >=
                   spec.value(QStringLiteral("max")).toInt(255)) {
                fail(error, QStringLiteral("Некорректная настройка оси: %1").arg(it.key()));
                return std::nullopt;
            }
        }
    }
    const auto buttons = profile.input.value(QStringLiteral("buttons"));
    if (!buttons.isUndefined() && !buttons.isArray()) {
        fail(error, QStringLiteral("input.buttons должен быть списком"));
        return std::nullopt;
    }
    for (const auto value : buttons.toArray()) {
        const auto item = value.toObject();
        int offset = -1;
        const QString name = item.value(QStringLiteral("name")).toString();
        const int mask = item.value(QStringLiteral("mask")).toInt();
        if (!isKnown(name, ButtonNames, std::size(ButtonNames))
            || !readOffset(item, QStringLiteral("offset"), &offset)
            || mask < 1 || mask > 255) {
            fail(error, QStringLiteral("Некорректная настройка кнопки: %1").arg(name));
            return std::nullopt;
        }
    }
    if (profile.input.value(QStringLiteral("dpad")).isObject()) {
        const auto dpad = profile.input.value(QStringLiteral("dpad")).toObject();
        int offset = -1;
        if (!readOffset(dpad, QStringLiteral("offset"), &offset)
            || dpad.value(QStringLiteral("mask")).toInt() < 1
            || !dpad.value(QStringLiteral("values")).isObject()) {
            fail(error, QStringLiteral("Некорректная настройка input.dpad"));
            return std::nullopt;
        }
    }

    for (const QString key : {QStringLiteral("lightbar"), QStringLiteral("rumble")}) {
        const auto report = profile.output.value(key);
        if (report.isNull() || report.isUndefined())
            continue;
        if (!report.isObject()) {
            fail(error, QStringLiteral("output.%1 должен быть объектом или null").arg(key));
            return std::nullopt;
        }
        const auto object = report.toObject();
        const auto data = QByteArray::fromHex(object.value(QStringLiteral("template_hex"))
                                                  .toString().toLatin1());
        if (data.isEmpty() || object.value(QStringLiteral("report_id")).toInt(-1) < 0) {
            fail(error, QStringLiteral("Некорректный HID output.%1").arg(key));
            return std::nullopt;
        }
        const auto offsets = object.value(QStringLiteral("offsets")).toObject();
        const QStringList names = key == QStringLiteral("lightbar")
            ? QStringList{QStringLiteral("r"), QStringLiteral("g"), QStringLiteral("b")}
            : QStringList{QStringLiteral("weak"), QStringLiteral("strong")};
        for (const auto &field : names) {
            const int offset = offsets.value(field).toInt(-1);
            if (offset < 0 || offset >= data.size()) {
                fail(error, QStringLiteral("Некорректное смещение %1 в output.%2").arg(field, key));
                return std::nullopt;
            }
        }
        const auto checksum = object.value(QStringLiteral("crc32"));
        if (checksum.isObject()) {
            const int offset = checksum.toObject().value(QStringLiteral("offset")).toInt(-1);
            if (offset < 0 || offset + 4 > data.size()) {
                fail(error, QStringLiteral("Некорректное смещение CRC32 в output.%1").arg(key));
                return std::nullopt;
            }
        }
    }
    return profile;
}

std::optional<ControllerState> ReportProfile::mapInput(quint16 reportId,
                                                        const QByteArray &data) const
{
    const auto expected = input.value(QStringLiteral("report_id"));
    if (!expected.isNull() && !expected.isUndefined() && expected.toInt(-1) != reportId)
        return std::nullopt;

    ControllerState state;
    const auto axisSpecs = input.value(QStringLiteral("axes")).toObject();
    for (auto it = axisSpecs.begin(); it != axisSpecs.end(); ++it) {
        const auto spec = it.value().toObject();
        const int offset = spec.value(QStringLiteral("offset")).toInt(-1);
        if (offset < 0 || offset >= data.size())
            continue;
        const int low = spec.value(QStringLiteral("min")).toInt(0);
        const int high = spec.value(QStringLiteral("max")).toInt(255);
        if (high <= low)
            continue;
        const int raw = quint8(data[offset]);
        int value = qRound(double(raw - low) * 255.0 / double(high - low));
        value = clampedByte(value);
        if (spec.value(QStringLiteral("invert")).toBool())
            value = 255 - value;
        state.axes.insert(it.key(), value);
    }

    for (const auto value : input.value(QStringLiteral("buttons")).toArray()) {
        const auto item = value.toObject();
        const int offset = item.value(QStringLiteral("offset")).toInt(-1);
        if (offset < 0 || offset >= data.size())
            continue;
        const quint8 mask = quint8(item.value(QStringLiteral("mask")).toInt());
        const bool set = (quint8(data[offset]) & mask) != 0;
        state.buttons.insert(item.value(QStringLiteral("name")).toString(),
                             set == item.value(QStringLiteral("active_high")).toBool(true));
    }

    const auto dpad = input.value(QStringLiteral("dpad")).toObject();
    const int dpadOffset = dpad.value(QStringLiteral("offset")).toInt(-1);
    if (!dpad.isEmpty() && dpadOffset >= 0 && dpadOffset < data.size()) {
        for (const QString direction : {QStringLiteral("dpad_up"), QStringLiteral("dpad_down"),
                                        QStringLiteral("dpad_left"), QStringLiteral("dpad_right")})
            state.buttons.insert(direction, false);
        const int value = quint8(data[dpadOffset]) & dpad.value(QStringLiteral("mask")).toInt();
        const auto names = dpad.value(QStringLiteral("values")).toObject()
                               .value(QString::number(value)).toArray();
        for (const auto name : names)
            state.buttons.insert(name.toString(), true);
    }

    const auto extended = input.value(QStringLiteral("ds4_extended")).toObject();
    if (!extended.isEmpty()) {
        int commonOffset = -1;
        int countOffset = -1;
        int touchOffset = -1;
        if (readOffset(extended, QStringLiteral("common_offset"), &commonOffset)
            && readOffset(extended, QStringLiteral("touch_count_offset"), &countOffset)
            && readOffset(extended, QStringLiteral("touch_reports_offset"), &touchOffset)) {
            const int touchSize = extended.value(QStringLiteral("touch_report_size")).toInt(9);
            const int sourceMax = extended.value(QStringLiteral("source_touch_count_max")).toInt(2);
            const int virtualMax = extended.value(QStringLiteral("virtual_touch_count_max")).toInt(2);
            if (touchSize > 0 && commonOffset + 32 <= data.size()
                && countOffset < data.size() && touchOffset <= data.size()) {
                const int available = (data.size() - touchOffset) / touchSize;
                const int sourceCount = std::clamp(int(quint8(data[countOffset])),
                                                   0, std::min(sourceMax, available));
                const int outputCount = std::min(sourceCount, virtualMax);
                const int firstRecord = sourceCount - outputCount;
                state.extendedReport = QByteArray(63, char(0));
                std::copy_n(data.constData() + commonOffset, 32, state.extendedReport.data());
                state.extendedReport[32] = char(outputCount);
                const int sourceStart = touchOffset + firstRecord * touchSize;
                std::copy_n(data.constData() + sourceStart, outputCount * touchSize,
                            state.extendedReport.data() + 33);
            }
        }
    }
    return state;
}

std::optional<OutputReport> ReportProfile::buildLightbarReport(int red, int green, int blue,
                                                               QString *error) const
{
    return buildOutput(QStringLiteral("lightbar"),
                       {{QStringLiteral("r"), red}, {QStringLiteral("g"), green},
                        {QStringLiteral("b"), blue}}, error);
}

std::optional<OutputReport> ReportProfile::buildRumbleReport(int weak, int strong,
                                                             QString *error) const
{
    return buildOutput(QStringLiteral("rumble"),
                       {{QStringLiteral("weak"), weak}, {QStringLiteral("strong"), strong}},
                       error);
}

std::optional<OutputReport> ReportProfile::buildCombinedReport(int weak, int strong, int red,
                                                               int green, int blue,
                                                               QString *error) const
{
    const auto lightValue = output.value(QStringLiteral("lightbar"));
    const auto rumbleValue = output.value(QStringLiteral("rumble"));
    if (!lightValue.isObject() || !rumbleValue.isObject()) {
        fail(error, QStringLiteral("В профиле нужны и output.lightbar, и output.rumble"));
        return std::nullopt;
    }
    const auto light = lightValue.toObject();
    const auto rumble = rumbleValue.toObject();
    QByteArray data = QByteArray::fromHex(light.value(QStringLiteral("template_hex"))
                                              .toString().toLatin1());
    const QByteArray rumbleData = QByteArray::fromHex(rumble.value(QStringLiteral("template_hex"))
                                                          .toString().toLatin1());
    if (data.isEmpty() || data.size() != rumbleData.size()
        || light.value(QStringLiteral("report_id")).toInt(-1)
               != rumble.value(QStringLiteral("report_id")).toInt(-2)
        || light.value(QStringLiteral("map_index")).toInt()
               != rumble.value(QStringLiteral("map_index")).toInt()) {
        fail(error, QStringLiteral("Шаблоны света и вибрации нельзя объединить"));
        return std::nullopt;
    }
    // Merge the constant bytes of both templates (e.g. the "enable rumble" and
    // "enable lightbar" flags); the value bytes are overwritten below.
    for (int i = 0; i < int(data.size()); ++i)
        data[i] = char(quint8(data[i]) | quint8(rumbleData[i]));

    const auto setValue = [&](const QJsonObject &config, const QString &field, int value) {
        const int offset = config.value(QStringLiteral("offsets")).toObject()
                               .value(field).toInt(-1);
        if (offset < 0 || offset >= data.size())
            return false;
        data[offset] = char(clampedByte(value));
        return true;
    };
    if (!setValue(light, QStringLiteral("r"), red) || !setValue(light, QStringLiteral("g"), green)
        || !setValue(light, QStringLiteral("b"), blue)
        || !setValue(rumble, QStringLiteral("weak"), weak)
        || !setValue(rumble, QStringLiteral("strong"), strong)) {
        fail(error, QStringLiteral("Некорректные смещения в output.lightbar/output.rumble"));
        return std::nullopt;
    }

    const auto checksum = light.value(QStringLiteral("crc32"));
    if (checksum.isObject()) {
        const auto crcConfig = checksum.toObject();
        const int offset = crcConfig.value(QStringLiteral("offset")).toInt(-1);
        if (offset < 0 || offset + 4 > data.size()) {
            fail(error, QStringLiteral("Некорректное смещение CRC32"));
            return std::nullopt;
        }
        QByteArray crcInput(1, char(crcConfig.value(QStringLiteral("seed")).toInt()));
        if (crcConfig.value(QStringLiteral("include_report_id")).toBool())
            crcInput.append(char(light.value(QStringLiteral("report_id")).toInt()));
        crcInput.append(data.constData(), offset);
        const quint32 crc = crc32(crcInput);
        qToLittleEndian(crc, reinterpret_cast<uchar *>(data.data() + offset));
    }

    OutputReport result;
    result.mapIndex = quint8(light.value(QStringLiteral("map_index")).toInt());
    result.reportId = quint16(light.value(QStringLiteral("report_id")).toInt());
    result.data = data;
    return result;
}

bool ReportProfile::hasLightbar() const
{
    return output.value(QStringLiteral("lightbar")).isObject();
}

bool ReportProfile::hasRumble() const
{
    return output.value(QStringLiteral("rumble")).isObject();
}

std::optional<OutputReport> ReportProfile::buildOutput(const QString &name,
                                                       const QMap<QString, int> &values,
                                                       QString *error) const
{
    const auto configValue = output.value(name);
    if (!configValue.isObject()) {
        fail(error, QStringLiteral("В профиле не задан output.%1").arg(name));
        return std::nullopt;
    }
    const auto config = configValue.toObject();
    QByteArray data = QByteArray::fromHex(config.value(QStringLiteral("template_hex"))
                                              .toString().toLatin1());
    if (data.isEmpty()) {
        fail(error, QStringLiteral("Пустой HID-шаблон output.%1").arg(name));
        return std::nullopt;
    }
    const auto offsets = config.value(QStringLiteral("offsets")).toObject();
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const int offset = offsets.value(it.key()).toInt(-1);
        if (offset < 0 || offset >= data.size()) {
            fail(error, QStringLiteral("Некорректное смещение %1 в output.%2").arg(it.key(), name));
            return std::nullopt;
        }
        data[offset] = char(clampedByte(it.value()));
    }

    const auto checksum = config.value(QStringLiteral("crc32"));
    if (checksum.isObject()) {
        const auto crcConfig = checksum.toObject();
        const int offset = crcConfig.value(QStringLiteral("offset")).toInt(-1);
        if (offset < 0 || offset + 4 > data.size()) {
            fail(error, QStringLiteral("Некорректное смещение CRC32 в output.%1").arg(name));
            return std::nullopt;
        }
        QByteArray crcInput(1, char(crcConfig.value(QStringLiteral("seed")).toInt()));
        if (crcConfig.value(QStringLiteral("include_report_id")).toBool())
            crcInput.append(char(config.value(QStringLiteral("report_id")).toInt()));
        crcInput.append(data.constData(), offset);
        const quint32 crc = crc32(crcInput);
        qToLittleEndian(crc, reinterpret_cast<uchar *>(data.data() + offset));
    }

    OutputReport result;
    result.mapIndex = quint8(config.value(QStringLiteral("map_index")).toInt());
    result.reportId = quint16(config.value(QStringLiteral("report_id")).toInt());
    result.data = data;
    return result;
}
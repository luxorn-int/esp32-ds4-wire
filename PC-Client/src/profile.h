#pragma once

#include <QByteArray>
#include <QMap>
#include <QJsonObject>
#include <QString>

#include <optional>

struct ControllerState {
    QMap<QString, int> axes;
    QMap<QString, bool> buttons;
    QByteArray extendedReport;
};

struct OutputReport {
    quint8 mapIndex = 0;
    quint16 reportId = 0;
    QByteArray data;
};

class ReportProfile {
public:
    QString name;
    QJsonObject input;
    QJsonObject output;

    static std::optional<ReportProfile> load(const QString &path, QString *error = nullptr);
    std::optional<ControllerState> mapInput(quint16 reportId, const QByteArray &data) const;
    std::optional<OutputReport> buildLightbarReport(int red, int green, int blue,
                                                    QString *error = nullptr) const;
    std::optional<OutputReport> buildRumbleReport(int weak, int strong,
                                                 QString *error = nullptr) const;
    // Lightbar + rumble in ONE report (a DS4 output report carries both; sending
    // them separately makes each one reset the other). Needs both templates to
    // use the same report id / map index / size, otherwise returns nullopt.
    std::optional<OutputReport> buildCombinedReport(int weak, int strong, int red, int green,
                                                    int blue, QString *error = nullptr) const;
    bool hasLightbar() const;
    bool hasRumble() const;

private:
    std::optional<OutputReport> buildOutput(const QString &name,
                                            const QMap<QString, int> &values,
                                            QString *error) const;
};
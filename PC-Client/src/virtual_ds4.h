#pragma once

#include "profile.h"

#include <QString>

#include <functional>
#include <memory>

QByteArray makeDs4Report(const ControllerState &state);

class VirtualDS4 {
public:
    VirtualDS4();
    ~VirtualDS4();

    VirtualDS4(const VirtualDS4 &) = delete;
    VirtualDS4 &operator=(const VirtualDS4 &) = delete;

    bool available(int slot) const;
    int availableCount() const;
    QString statusText() const;
    bool apply(int slot, const ControllerState &state);
    void neutral(int slot);
    void close();

    // Called (from a ViGEm worker thread!) when an application such as Steam
    // sends rumble / lightbar data to the virtual DS4 in `slot`.
    using OutputCallback = std::function<void(int slot, quint8 largeMotor, quint8 smallMotor,
                                              quint8 red, quint8 green, quint8 blue)>;
    void setOutputCallback(OutputCallback callback);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
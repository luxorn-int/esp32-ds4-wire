#include "virtual_ds4.h"

#include <QByteArray>
#include <QOperatingSystemVersion>

#include <array>
#include <cstring>
#include <mutex>

#ifdef Q_OS_WIN
#include <QLibrary>
#endif

// Calling convention of the ViGEmClient callbacks (CALLBACK == __stdcall, which
// only matters on 32-bit x86).
#if defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_32)
#define VIGEM_CALL __stdcall
#else
#define VIGEM_CALL
#endif

namespace {
constexpr int ReportSize = 63;
constexpr quint32 VigemSuccess = 0x20000000u;
constexpr quint16 ButtonBits[] = {
    quint16(1u << 5), quint16(1u << 6), quint16(1u << 4), quint16(1u << 7),
    quint16(1u << 8), quint16(1u << 9), quint16(1u << 14), quint16(1u << 15),
    quint16(1u << 12), quint16(1u << 13)
};
constexpr const char *ButtonNames[] = {
    "cross", "circle", "square", "triangle", "l1", "r1", "l3", "r3",
    "share", "options"
};
} // namespace

QByteArray makeDs4Report(const ControllerState &state)
{
    QByteArray report(ReportSize, char(0));
    report[0] = report[1] = report[2] = report[3] = char(128);
    report[4] = char(0x08); // Neutral D-pad.

    if (state.extendedReport.size() == ReportSize)
        report = state.extendedReport;

    const auto setAxis = [&](const QString &name, int offset) {
        const auto it = state.axes.constFind(name);
        if (it != state.axes.cend())
            report[offset] = char(qBound(0, it.value(), 255));
    };
    setAxis(QStringLiteral("lx"), 0);
    setAxis(QStringLiteral("ly"), 1);
    setAxis(QStringLiteral("rx"), 2);
    setAxis(QStringLiteral("ry"), 3);
    setAxis(QStringLiteral("lt"), 7);
    setAxis(QStringLiteral("rt"), 8);

    quint16 buttons = quint8(report[4]) | (quint16(quint8(report[5])) << 8);
    for (qsizetype i = 0; i < qsizetype(std::size(ButtonNames)); ++i) {
        const auto it = state.buttons.constFind(QLatin1String(ButtonNames[i]));
        if (it == state.buttons.cend())
            continue;
        if (it.value())
            buttons |= ButtonBits[i];
        else
            buttons &= quint16(~ButtonBits[i]);
    }

    const bool up = state.buttons.value(QStringLiteral("dpad_up"));
    const bool down = state.buttons.value(QStringLiteral("dpad_down"));
    const bool left = state.buttons.value(QStringLiteral("dpad_left"));
    const bool right = state.buttons.value(QStringLiteral("dpad_right"));
    int dpad = 8;
    if (up && right) dpad = 1;
    else if (right && down) dpad = 3;
    else if (down && left) dpad = 5;
    else if (left && up) dpad = 7;
    else if (up) dpad = 0;
    else if (right) dpad = 2;
    else if (down) dpad = 4;
    else if (left) dpad = 6;
    buttons = quint16((buttons & 0xFFF0u) | quint16(dpad));
    report[4] = char(buttons & 0xFF);
    report[5] = char((buttons >> 8) & 0xFF);

    quint8 special = quint8(report[6]) & 0xFCu;
    if (state.buttons.value(QStringLiteral("ps")))
        special |= 0x01;
    if (state.buttons.value(QStringLiteral("touchpad")))
        special |= 0x02;
    report[6] = char(special);
    return report;
}

struct VirtualDS4::Impl {
    std::mutex callbackMutex;
    VirtualDS4::OutputCallback callback;

    void emitOutput(int slot, quint8 largeMotor, quint8 smallMotor, quint8 red, quint8 green,
                    quint8 blue)
    {
        VirtualDS4::OutputCallback copy;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            copy = callback;
        }
        if (copy)
            copy(slot, largeMotor, smallMotor, red, green, blue);
    }

#ifdef Q_OS_WIN
    QString status;
#else
    QString status = QStringLiteral("Виртуальные DS4 доступны только в Windows");
#endif
#ifdef Q_OS_WIN
    using ClientHandle = void *;
    using TargetHandle = void *;
    using Error = quint32;
    struct ExReport { unsigned char bytes[ReportSize]; };

    using AllocFn = ClientHandle (*)();
    using ConnectFn = Error (*)(ClientHandle);
    using DisconnectFn = void (*)(ClientHandle);
    using FreeFn = void (*)(ClientHandle);
    using TargetAllocFn = TargetHandle (*)();
    using TargetAddFn = Error (*)(ClientHandle, TargetHandle);
    using TargetRemoveFn = Error (*)(ClientHandle, TargetHandle);
    using TargetFreeFn = void (*)(TargetHandle);
    using UpdateExFn = Error (*)(ClientHandle, TargetHandle, ExReport);

    // DS4_LIGHTBAR_COLOR from ViGEm/Common.h (3 bytes, passed by value).
    struct LightbarColor { unsigned char red, green, blue; };
    using NotificationFn = void (VIGEM_CALL *)(void *client, void *target,
                                               unsigned char largeMotor,
                                               unsigned char smallMotor,
                                               LightbarColor color, void *userData);
    using RegisterNotificationFn = Error (*)(ClientHandle, TargetHandle, NotificationFn, void *);
    using UnregisterNotificationFn = void (*)(TargetHandle);

    struct NotifyContext {
        Impl *owner = nullptr;
        int slot = 0;
    };
    std::array<NotifyContext, 2> contexts;
    RegisterNotificationFn registerNotification = nullptr;
    UnregisterNotificationFn unregisterNotification = nullptr;

    // Runs on a ViGEmClient worker thread.
    static void VIGEM_CALL notificationThunk(void *, void *, unsigned char largeMotor,
                                             unsigned char smallMotor, LightbarColor color,
                                             void *userData)
    {
        auto *context = static_cast<NotifyContext *>(userData);
        if (context && context->owner)
            context->owner->emitOutput(context->slot, largeMotor, smallMotor, color.red,
                                       color.green, color.blue);
    }

    QLibrary library{QStringLiteral("ViGEmClient")};
    ClientHandle client = nullptr;
    std::array<TargetHandle, 2> targets{nullptr, nullptr};
    DisconnectFn disconnect = nullptr;
    FreeFn freeClient = nullptr;
    TargetRemoveFn removeTarget = nullptr;
    TargetFreeFn freeTarget = nullptr;
    UpdateExFn updateEx = nullptr;
#endif

    Impl()
    {
#ifdef Q_OS_WIN
        if (!library.load()) {
            status = QStringLiteral("Не найдена ViGEmClient.dll. Установите ViGEmBus/ViGEmClient.");
            return;
        }
        const auto alloc = reinterpret_cast<AllocFn>(library.resolve("vigem_alloc"));
        const auto connect = reinterpret_cast<ConnectFn>(library.resolve("vigem_connect"));
        disconnect = reinterpret_cast<DisconnectFn>(library.resolve("vigem_disconnect"));
        freeClient = reinterpret_cast<FreeFn>(library.resolve("vigem_free"));
        const auto targetAlloc = reinterpret_cast<TargetAllocFn>(
            library.resolve("vigem_target_ds4_alloc"));
        const auto targetAdd = reinterpret_cast<TargetAddFn>(
            library.resolve("vigem_target_add"));
        removeTarget = reinterpret_cast<TargetRemoveFn>(
            library.resolve("vigem_target_remove"));
        freeTarget = reinterpret_cast<TargetFreeFn>(
            library.resolve("vigem_target_free"));
        updateEx = reinterpret_cast<UpdateExFn>(
            library.resolve("vigem_target_ds4_update_ex"));
        // Optional: only needed to pass rumble/lightbar from the host back to the pad.
        registerNotification = reinterpret_cast<RegisterNotificationFn>(
            library.resolve("vigem_target_ds4_register_notification"));
        unregisterNotification = reinterpret_cast<UnregisterNotificationFn>(
            library.resolve("vigem_target_ds4_unregister_notification"));

        if (!alloc || !connect || !disconnect || !freeClient || !targetAlloc || !targetAdd
            || !removeTarget || !freeTarget || !updateEx) {
            status = QStringLiteral("В ViGEmClient.dll отсутствует необходимый API DS4_REPORT_EX.");
            library.unload();
            return;
        }
        client = alloc();
        if (!client || connect(client) != VigemSuccess) {
            status = QStringLiteral("Не удалось подключиться к ViGEmBus. Проверьте установку драйвера.");
            if (client) {
                freeClient(client);
                client = nullptr;
            }
            return;
        }

        int count = 0;
        for (int slot = 0; slot < 2; ++slot) {
            auto target = targetAlloc();
            if (target && targetAdd(client, target) == VigemSuccess) {
                targets[slot] = target;
                ++count;
            } else if (target) {
                freeTarget(target);
            }
        }
        status = QStringLiteral("Создано виртуальных DS4: %1/2").arg(count);

        if (registerNotification && unregisterNotification) {
            int notified = 0;
            for (int slot = 0; slot < 2; ++slot) {
                if (!targets[slot])
                    continue;
                contexts[size_t(slot)] = NotifyContext{this, slot};
                if (registerNotification(client, targets[slot], &Impl::notificationThunk,
                                         &contexts[size_t(slot)]) == VigemSuccess)
                    ++notified;
            }
            status += QStringLiteral(", обратная связь (свет/вибрация): %1/2").arg(notified);
        } else {
            status += QStringLiteral(", обратной связи нет (в ViGEmClient.dll нет API уведомлений)");
        }
#endif
    }

    ~Impl()
    {
#ifdef Q_OS_WIN
        if (client) {
            for (auto &target : targets) {
                if (target) {
                    // Stops the worker thread; no callback runs after this returns.
                    if (unregisterNotification)
                        unregisterNotification(target);
                    removeTarget(client, target);
                    freeTarget(target);
                    target = nullptr;
                }
            }
            disconnect(client);
            freeClient(client);
            client = nullptr;
        }
        if (library.isLoaded())
            library.unload();
#endif
    }
};

VirtualDS4::VirtualDS4() : m_impl(std::make_unique<Impl>()) {}
VirtualDS4::~VirtualDS4() = default;

bool VirtualDS4::available(int slot) const
{
#ifdef Q_OS_WIN
    return m_impl && slot >= 0 && slot < int(m_impl->targets.size())
        && m_impl->targets[size_t(slot)];
#else
    Q_UNUSED(slot);
    return false;
#endif
}

int VirtualDS4::availableCount() const
{
#ifdef Q_OS_WIN
    if (!m_impl)
        return 0;
    int count = 0;
    for (const auto target : m_impl->targets)
        count += target != nullptr;
    return count;
#else
    return 0;
#endif
}

QString VirtualDS4::statusText() const
{
    return m_impl ? m_impl->status : QStringLiteral("Виртуальные DS4 отключены");
}

bool VirtualDS4::apply(int slot, const ControllerState &state)
{
#ifdef Q_OS_WIN
    if (!m_impl || !available(slot) || !m_impl->client || !m_impl->updateEx)
        return false;
    const QByteArray bytes = ::makeDs4Report(state);
    Impl::ExReport report{};
    std::memcpy(report.bytes, bytes.constData(), ReportSize);
    return m_impl->updateEx(m_impl->client, m_impl->targets[size_t(slot)], report)
        == VigemSuccess;
#else
    Q_UNUSED(slot);
    Q_UNUSED(state);
    return false;
#endif
}

void VirtualDS4::neutral(int slot)
{
    apply(slot, ControllerState{});
}

void VirtualDS4::setOutputCallback(OutputCallback callback)
{
    if (!m_impl)
        return;
    std::lock_guard<std::mutex> lock(m_impl->callbackMutex);
    m_impl->callback = std::move(callback);
}

void VirtualDS4::close()
{
    m_impl.reset();
}
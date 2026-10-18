// SpaceWindow (Qt) - what the desktop says about how programs should look and behave.
#include "Desktop.h"

#include <array>

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QGuiApplication>
#include <QStyleHints>

#include "QtCompat.h"

#if SPACEWINDOW_QT_API >= QT_VERSION_CHECK(6, 10, 0)
#include <QAccessibilityHints>
#endif

namespace sm::qt
{
    namespace
    {
        constexpr QLatin1String kPortal("org.freedesktop.portal.Desktop");
        constexpr QLatin1String kPortalPath("/org/freedesktop/portal/desktop");
        constexpr QLatin1String kSettings("org.freedesktop.portal.Settings");
        constexpr QLatin1String kAppearance("org.freedesktop.appearance");
        constexpr QLatin1String kContrast("contrast");

        // power-profiles-daemon, under its name since 0.20 and its older one.
        struct PowerProfiles
        {
            QLatin1String service;
            QLatin1String path;
        };
        constexpr std::array kPowerProfiles = {
            PowerProfiles{ QLatin1String("org.freedesktop.UPower.PowerProfiles"),
                           QLatin1String("/org/freedesktop/UPower/PowerProfiles") },
            PowerProfiles{ QLatin1String("net.hadess.PowerProfiles"), QLatin1String("/net/hadess/PowerProfiles") },
        };
        constexpr QLatin1String kProperties("org.freedesktop.DBus.Properties");
        constexpr QLatin1String kActiveProfile("ActiveProfile");

        // Unwraps a D-Bus variant, however deep (the portal's old Read wraps its answer twice).
        QVariant Unwrapped(QVariant value)
        {
            while (value.canConvert<QDBusVariant>() && value.metaType() == QMetaType::fromType<QDBusVariant>())
            {
                value = qvariant_cast<QDBusVariant>(value).variant();
            }
            return value;
        }

        // Runs `answered` with the reply to `message` once it comes, if it comes.
        template <typename Answered>
        void Ask(QDBusConnection const& bus, QDBusMessage const& message, QObject* context, Answered answered)
        {
            auto* const watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), context);
            QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context,
                             [answered = std::move(answered)](QDBusPendingCallWatcher* call) {
                                 QDBusMessage const reply = call->reply();
                                 call->deleteLater();
                                 answered(reply);
                             });
        }
    }

    Desktop::Desktop(QObject* parent) : QObject(parent)
    {
        QDBusConnection const session = QDBusConnection::sessionBus();
        if (session.isConnected())
        {
            (void)QDBusConnection(session).connect(kPortal, kPortalPath, kSettings, QStringLiteral("SettingChanged"),
                                                   this, SLOT(OnSettingChanged(QString, QString, QDBusVariant)));
            ReadContrast();
        }
        QDBusConnection const system = QDBusConnection::systemBus();
        if (system.isConnected())
        {
            for (PowerProfiles const& daemon : kPowerProfiles)
            {
                (void)QDBusConnection(system).connect(daemon.service, daemon.path, kProperties,
                                                      QStringLiteral("PropertiesChanged"), this,
                                                      SLOT(OnPowerChanged(QString, QVariantMap, QStringList)));
            }
            ReadPowerProfile();
        }
#if SPACEWINDOW_QT_API >= QT_VERSION_CHECK(6, 10, 0)
        connect(QGuiApplication::styleHints()->accessibility(), &QAccessibilityHints::contrastPreferenceChanged, this,
                &Desktop::Changed);
#endif
    }

    bool Desktop::HighContrast() const
    {
        if (pretendedContrast_)
        {
            return *pretendedContrast_;
        }
#if SPACEWINDOW_QT_API >= QT_VERSION_CHECK(6, 10, 0)
        if (QGuiApplication::styleHints()->accessibility()->contrastPreference() ==
            Qt::ContrastPreference::HighContrast)
        {
            return true;
        }
#endif
        return contrast_;
    }

    void Desktop::Pretend(std::optional<bool> highContrast, std::optional<bool> powerSaving)
    {
        pretendedContrast_ = highContrast;
        pretendedPowerSaving_ = powerSaving;
        Q_EMIT Changed();
    }

    void Desktop::ReadContrast()
    {
        QDBusMessage readOne =
            QDBusMessage::createMethodCall(kPortal, kPortalPath, kSettings, QStringLiteral("ReadOne"));
        readOne << QString(kAppearance) << QString(kContrast);
        Ask(QDBusConnection::sessionBus(), readOne, this, [this](QDBusMessage const& reply) {
            if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
            {
                SetContrast(reply.arguments().front());
                return;
            }
            // Portals before version 2 have only Read.
            QDBusMessage read = QDBusMessage::createMethodCall(kPortal, kPortalPath, kSettings, QStringLiteral("Read"));
            read << QString(kAppearance) << QString(kContrast);
            Ask(QDBusConnection::sessionBus(), read, this, [this](QDBusMessage const& old) {
                if (old.type() == QDBusMessage::ReplyMessage && !old.arguments().isEmpty())
                {
                    SetContrast(old.arguments().front());
                }
            });
        });
    }

    void Desktop::ReadPowerProfile()
    {
        for (PowerProfiles const& daemon : kPowerProfiles)
        {
            QDBusMessage get =
                QDBusMessage::createMethodCall(daemon.service, daemon.path, kProperties, QStringLiteral("Get"));
            get << QString(daemon.service) << QString(kActiveProfile);
            Ask(QDBusConnection::systemBus(), get, this, [this](QDBusMessage const& reply) {
                if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty())
                {
                    SetProfile(Unwrapped(reply.arguments().front()).toString());
                }
            });
        }
    }

    void Desktop::SetContrast(QVariant const& value)
    {
        bool const on = Unwrapped(value).toUInt() == 1;  // 0: no preference, 1: high contrast
        if (on != contrast_)
        {
            contrast_ = on;
            Q_EMIT Changed();
        }
    }

    void Desktop::SetProfile(QString const& profile)
    {
        bool const saving = profile == QStringLiteral("power-saver");
        if (saving != powerSaving_)
        {
            powerSaving_ = saving;
            Q_EMIT Changed();
        }
    }

    void Desktop::OnSettingChanged(QString const& group, QString const& key, QDBusVariant const& value)
    {
        if (group == kAppearance && key == kContrast)
        {
            SetContrast(value.variant());
        }
    }

    void Desktop::OnPowerChanged(QString const&, QVariantMap const& changed, QStringList const&)
    {
        if (auto const profile = changed.constFind(QString(kActiveProfile)); profile != changed.constEnd())
        {
            SetProfile(Unwrapped(*profile).toString());
        }
    }
}

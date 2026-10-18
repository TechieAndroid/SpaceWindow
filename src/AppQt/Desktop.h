// SpaceWindow (Qt) - what the desktop says about how programs should look
// and behave: high contrast, and saving energy.
//
// Linux has no single switch for either (Windows has SPI_GETHIGHCONTRAST and
// the battery saver).  High contrast comes from the freedesktop.org Settings
// portal (org.freedesktop.appearance "contrast": GNOME, KDE Plasma 6), and
// from Qt where it reads it itself (6.10); saving energy from the
// "power-saver" profile of power-profiles-daemon, which the battery menus of
// GNOME and KDE switch.  Everything is asked without waiting, and followed.
#pragma once

#include <optional>

#include <QDBusVariant>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

namespace sm::qt
{
    class Desktop : public QObject
    {
        Q_OBJECT

    public:
        explicit Desktop(QObject* parent = nullptr);

        bool HighContrast() const;
        bool PowerSaving() const noexcept { return pretendedPowerSaving_.value_or(powerSaving_); }

        // For tests: as if the desktop said so (nothing: as it does say).
        void Pretend(std::optional<bool> highContrast, std::optional<bool> powerSaving);

    Q_SIGNALS:
        void Changed();

    private Q_SLOTS:
        void OnSettingChanged(QString const& group, QString const& key, QDBusVariant const& value);
        void OnPowerChanged(QString const& interface, QVariantMap const& changed, QStringList const& invalidated);

    private:  // NOLINT(readability-redundant-access-specifiers): Q_SLOTS is not one to clang-tidy
        void ReadContrast();
        void ReadPowerProfile();
        void SetContrast(QVariant const& value);
        void SetProfile(QString const& profile);

        bool contrast_ = false;  // the portal's
        bool powerSaving_ = false;
        std::optional<bool> pretendedContrast_;
        std::optional<bool> pretendedPowerSaving_;
    };
}

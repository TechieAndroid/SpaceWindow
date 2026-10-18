// SpaceWindow (Qt) - the dialogs.
#include "Dialogs.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include "Commands.h"
#include "Format.h"
#include "QtText.h"
#include "Shell.h"
#include "Text.h"
#include "Version.h"

namespace sm::qt
{
    namespace
    {
        QStringList Names(std::span<std::wstring_view const> names)
        {
            QStringList list;
            for (std::wstring_view const name : names)
            {
                list << Q(name);
            }
            return list;
        }
    }

    // ---------------------------------------------------------------- drive or folder

    std::optional<std::wstring> PickDriveOrFolder(QWidget* parent, Lang const& lang, std::wstring const& current)
    {
        QDialog dialog(parent);
        dialog.setWindowTitle(Q(lang.selectDrive));
        dialog.setMinimumWidth(460);
        auto* const layout = new QVBoxLayout(&dialog);
        auto* const list = new QListWidget(&dialog);
        list->setIconSize(QSize(32, 32));
        list->setSpacing(2);
        layout->addWidget(list);

        std::vector<shell::Drive> const drives = shell::Drives();
        int select = drives.empty() ? -1 : 0;
        for (std::size_t i = 0; i < drives.size(); ++i)
        {
            shell::Drive const& drive = drives[i];
            auto* const row = new QWidget(list);
            auto* const rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(6, 6, 6, 6);
            rowLayout->setSpacing(12);
            auto* const icon = new QLabel(row);
            QIcon const themed =
                QIcon::fromTheme(drive.remote ? QStringLiteral("folder-remote") : QStringLiteral("drive-harddisk"));
            icon->setPixmap(themed.pixmap(32, 32));
            rowLayout->addWidget(icon);
            auto* const text = new QVBoxLayout();
            text->setSpacing(3);
            QString name = Q(drive.name);
            if (drive.name != drive.root && drive.root != L"/")
            {
                name += QStringLiteral("  (") + Q(drive.root) + QLatin1Char(')');
            }
            text->addWidget(new QLabel(name, row));
            // A capacity bar as file managers draw it.
            std::uint32_t const used = PerMille(drive.totalBytes - drive.freeBytes, drive.totalBytes);
            auto* const bar = new QProgressBar(row);
            bar->setRange(0, 1000);
            bar->setValue(static_cast<int>(used));
            bar->setTextVisible(false);
            bar->setMaximumHeight(8);
            text->addWidget(bar);
            auto* const capacity =
                new QLabel(Q(Join(FormatSize(drive.freeBytes, lang), L" ", lang.free, L"  /  ",
                                  FormatSize(drive.totalBytes, lang), L" ", lang.total, L"   ", drive.device)),
                           row);
            capacity->setForegroundRole(QPalette::PlaceholderText);  // the secondary colour
            text->addWidget(capacity);
            rowLayout->addLayout(text, 1);

            auto* const item = new QListWidgetItem(list);
            item->setSizeHint(row->sizeHint());
            // The drive's place in `drives`: its path may not be text.
            item->setData(Qt::UserRole, static_cast<int>(i));
            item->setData(Qt::AccessibleTextRole, Q(drive.name));
            list->setItemWidget(item, row);
            if (!current.empty() && drive.root == current)
            {
                select = static_cast<int>(i);
            }
        }
        list->setCurrentRow(select);

        auto* const browse = new QPushButton(Q(lang.browseFolder), &dialog);
        browse->setFlat(true);
        browse->setCursor(Qt::PointingHandCursor);
        layout->addWidget(browse, 0, Qt::AlignLeft);
        auto* const buttons = new QDialogButtonBox(&dialog);
        buttons->addButton(Q(lang.ok), QDialogButtonBox::AcceptRole);
        buttons->addButton(Q(lang.cancel), QDialogButtonBox::RejectRole);
        layout->addWidget(buttons);

        bool browsing = false;
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);  // as in 1.4.0
        QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
            browsing = true;
            dialog.reject();
        });

        if (dialog.exec() == QDialog::Accepted && list->currentItem() != nullptr)
        {
            return drives.at(static_cast<std::size_t>(list->currentItem()->data(Qt::UserRole).toInt())).root;
        }
        if (browsing)
        {
            // As a URL: the desktop's own picker (a portal) hands over the folder's bytes whole.
            QUrl const folder = QFileDialog::getExistingDirectoryUrl(parent, Q(lang.selectDrive));
            if (std::optional<std::wstring> path = shell::LocalPath(folder))
            {
                return path;
            }
        }
        return std::nullopt;
    }

    // ---------------------------------------------------------------- settings

    namespace
    {
        // The Settings dialog: its widgets kept, so that a language picked
        // there renames everything in it at once (Retranslate).
        class SettingsDialog : public QDialog
        {
        public:
            SettingsDialog(QWidget* parent, Settings const& settings, std::vector<Theme> themes,
                           std::vector<std::wstring> themeErrors);

            void Retranslate(Lang const& lang);
            void SetThemes(std::vector<Theme> themes, std::vector<std::wstring> errors);
            Settings Edited() const;  // the settings as the dialog has them now

            QComboBox* languageBox = nullptr;
            QComboBox* themeBox = nullptr;
            QComboBox* backdropBox = nullptr;

        private:
            QLabel* Label(QWidget* parent);
            void FillThemes(std::wstring const& picked);
            void FillShortcuts();
            void UpdateTips();

            Settings original_;
            Lang const* lang_ = nullptr;
            std::vector<Theme> themes_;
            std::vector<std::wstring> themeErrors_;

            QGroupBox* appearance_ = nullptr;
            QLabel* languageLabel_ = nullptr;
            QLabel* themeLabel_ = nullptr;
            QPushButton* themesFolder_ = nullptr;
            QLabel* themeErrorsLabel_ = nullptr;
            QLabel* backdropLabel_ = nullptr;
            QLabel* backdropHint_ = nullptr;
            QGroupBox* layoutGroup_ = nullptr;
            QLabel* densityLabel_ = nullptr;
            QComboBox* densityBox_ = nullptr;
            QLabel* biasLabel_ = nullptr;
            QSlider* bias_ = nullptr;
            QLabel* horz_ = nullptr;
            QLabel* equal_ = nullptr;
            QLabel* vert_ = nullptr;
            QGroupBox* colors_ = nullptr;
            QLabel* filesLabel_ = nullptr;
            QLabel* foldersLabel_ = nullptr;
            QComboBox* fileColor_ = nullptr;
            QComboBox* folderColor_ = nullptr;
            QGroupBox* tips_ = nullptr;
            QCheckBox* nameTips_ = nullptr;
            QSpinBox* nameDelay_ = nullptr;
            QCheckBox* infoTips_ = nullptr;
            QSpinBox* infoDelay_ = nullptr;
            std::vector<std::pair<QCheckBox*, TipFlags>> flagBoxes_;
            QCheckBox* rollover_ = nullptr;
            QGroupBox* misc_ = nullptr;
            QCheckBox* autoRescan_ = nullptr;
            QCheckBox* disableDelete_ = nullptr;
            QCheckBox* animatedZoom_ = nullptr;
            QCheckBox* savePosition_ = nullptr;
            QGroupBox* shortcuts_ = nullptr;
            QGridLayout* shortcutGrid_ = nullptr;
            QPushButton* ok_ = nullptr;
            QPushButton* cancel_ = nullptr;
        };

        QLabel* SettingsDialog::Label(QWidget* parent)
        {
            auto* const label = new QLabel(parent);
            label->setWordWrap(true);
            return label;
        }

        SettingsDialog::SettingsDialog(QWidget* parent, Settings const& settings, std::vector<Theme> themes,
                                       std::vector<std::wstring> themeErrors)
            : QDialog(parent), original_(settings), themes_(std::move(themes)), themeErrors_(std::move(themeErrors))
        {
            auto* const outer = new QVBoxLayout(this);
            auto* const scroll = new QScrollArea(this);
            scroll->setWidgetResizable(true);
            scroll->setFrameShape(QFrame::NoFrame);
            auto* const page = new QWidget(scroll);
            auto* const layout = new QVBoxLayout(page);
            scroll->setWidget(page);
            outer->addWidget(scroll);

            // Appearance: language, theme and the background animation.
            appearance_ = new QGroupBox(page);
            auto* const appearanceForm = new QFormLayout(appearance_);
            languageBox = new QComboBox(appearance_);
            for (Lang const& candidate : Languages())
            {
                languageBox->addItem(Q(candidate.name), Q(candidate.code));
            }
            languageLabel_ = Label(appearance_);
            appearanceForm->addRow(languageLabel_, languageBox);
            themeBox = new QComboBox(appearance_);
            themeLabel_ = Label(appearance_);
            appearanceForm->addRow(themeLabel_, themeBox);
            themesFolder_ = new QPushButton(appearance_);
            themesFolder_->setFlat(true);
            themesFolder_->setCursor(Qt::PointingHandCursor);
            std::vector<std::wstring> const folders = shell::ThemeFolders();
            if (!folders.empty())
            {
                themesFolder_->setToolTip(Q(folders.front()));
            }
            connect(themesFolder_, &QPushButton::clicked, this, [folders] {
                if (!folders.empty())
                {
                    (void)shell::OpenFolder(folders.front());
                }
            });
            appearanceForm->addRow(QString(), themesFolder_);
            themeErrorsLabel_ = Label(appearance_);
            themeErrorsLabel_->setStyleSheet(QStringLiteral("color: #b35900;"));
            appearanceForm->addRow(themeErrorsLabel_);
            backdropBox = new QComboBox(appearance_);
            for (int i = 0; i < kBackdropCount; ++i)
            {
                backdropBox->addItem(QString(), i);
            }
            backdropBox->setCurrentIndex(static_cast<int>(settings.backdrop));
            backdropLabel_ = Label(appearance_);
            appearanceForm->addRow(backdropLabel_, backdropBox);
            backdropHint_ = Label(appearance_);
            backdropHint_->setForegroundRole(QPalette::PlaceholderText);
            appearanceForm->addRow(QString(), backdropHint_);
            layout->addWidget(appearance_);

            // File layout: density and bias.
            layoutGroup_ = new QGroupBox(page);
            auto* const fileForm = new QFormLayout(layoutGroup_);
            densityBox_ = new QComboBox(layoutGroup_);
            densityLabel_ = Label(layoutGroup_);
            fileForm->addRow(densityLabel_, densityBox_);
            bias_ = new QSlider(Qt::Horizontal, layoutGroup_);
            bias_->setRange(Settings::kMinBias, Settings::kMaxBias);
            bias_->setTickInterval(5);
            bias_->setTickPosition(QSlider::TicksBelow);
            bias_->setValue(settings.bias);
            auto* const biasBox = new QWidget(layoutGroup_);
            auto* const biasLayout = new QGridLayout(biasBox);
            biasLayout->setContentsMargins(0, 0, 0, 0);
            biasLayout->addWidget(bias_, 0, 0, 1, 3);
            horz_ = new QLabel(biasBox);
            equal_ = new QLabel(biasBox);
            vert_ = new QLabel(biasBox);
            biasLayout->addWidget(horz_, 1, 0, Qt::AlignLeft);
            biasLayout->addWidget(equal_, 1, 1, Qt::AlignHCenter);
            biasLayout->addWidget(vert_, 1, 2, Qt::AlignRight);
            biasLabel_ = Label(layoutGroup_);
            fileForm->addRow(biasLabel_, biasBox);
            layout->addWidget(layoutGroup_);

            // Display colours.
            colors_ = new QGroupBox(page);
            auto* const colorForm = new QFormLayout(colors_);
            fileColor_ = new QComboBox(colors_);
            folderColor_ = new QComboBox(colors_);
            filesLabel_ = Label(colors_);
            foldersLabel_ = Label(colors_);
            colorForm->addRow(filesLabel_, fileColor_);
            colorForm->addRow(foldersLabel_, folderColor_);
            layout->addWidget(colors_);

            // Tooltips.
            tips_ = new QGroupBox(page);
            auto* const tipsGrid = new QGridLayout(tips_);
            nameTips_ = new QCheckBox(tips_);
            nameTips_->setChecked(settings.showNameTips);
            nameDelay_ = new QSpinBox(tips_);
            nameDelay_->setRange(0, Settings::kMaxDelay);
            nameDelay_->setValue(settings.nameTipDelay);
            infoTips_ = new QCheckBox(tips_);
            infoTips_->setChecked(settings.showInfoTips);
            infoDelay_ = new QSpinBox(tips_);
            infoDelay_->setRange(0, Settings::kMaxDelay);
            infoDelay_->setValue(settings.infoTipDelay);
            tipsGrid->addWidget(nameTips_, 0, 0);
            tipsGrid->addWidget(nameDelay_, 0, 1);
            tipsGrid->addWidget(infoTips_, 1, 0);
            tipsGrid->addWidget(infoDelay_, 1, 1);
            // What the information tips show, set in under them.
            auto* const flagsPanel = new QWidget(tips_);
            auto* const flagsGrid = new QGridLayout(flagsPanel);
            flagsGrid->setContentsMargins(24, 0, 0, 0);
            constexpr std::array kFlags = { TipPath, TipDate, TipName, TipSize, TipIcon, TipAttrib };
            for (std::size_t i = 0; i < kFlags.size(); ++i)
            {
                auto* const box = new QCheckBox(flagsPanel);
                box->setChecked((settings.infoTipFlags & kFlags[i]) != 0);
                flagsGrid->addWidget(box, static_cast<int>(i / 2), static_cast<int>(i % 2));
                flagBoxes_.emplace_back(box, kFlags[i]);
            }
            tipsGrid->addWidget(flagsPanel, 2, 0, 1, 2);
            rollover_ = new QCheckBox(tips_);
            rollover_->setChecked(settings.rolloverBox);
            tipsGrid->addWidget(rollover_, 3, 0, 1, 2);
            connect(nameTips_, &QCheckBox::toggled, this, [this] { UpdateTips(); });
            connect(infoTips_, &QCheckBox::toggled, this, [this] { UpdateTips(); });
            UpdateTips();
            layout->addWidget(tips_);

            // Miscellaneous.
            misc_ = new QGroupBox(page);
            auto* const miscLayout = new QVBoxLayout(misc_);
            auto const check = [this, miscLayout](bool on) {
                auto* const box = new QCheckBox(misc_);
                box->setChecked(on);
                miscLayout->addWidget(box);
                return box;
            };
            autoRescan_ = check(settings.autoRescan);
            disableDelete_ = check(settings.disableDelete);
            animatedZoom_ = check(settings.animatedZoom);
            savePosition_ = check(settings.savePosition);
            layout->addWidget(misc_);

            // The keyboard shortcuts, from the table the window's keys come from.
            shortcuts_ = new QGroupBox(page);
            shortcutGrid_ = new QGridLayout(shortcuts_);
            shortcutGrid_->setHorizontalSpacing(16);
            shortcutGrid_->setColumnStretch(1, 1);
            layout->addWidget(shortcuts_);

            auto* const buttons = new QDialogButtonBox(this);
            ok_ = buttons->addButton(QString(), QDialogButtonBox::AcceptRole);
            ok_->setDefault(true);
            cancel_ = buttons->addButton(QString(), QDialogButtonBox::RejectRole);
            connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
            outer->addWidget(buttons);

            Lang const& lang =
                FindLanguage(settings.language.empty() ? DefaultLanguageCode() : std::wstring_view(settings.language));
            languageBox->setCurrentIndex(std::max(0, languageBox->findData(Q(lang.code))));
            Retranslate(lang);
            FillThemes(settings.theme);
            densityBox_->setCurrentIndex(settings.density - Settings::kMinDensity);
            fileColor_->setCurrentIndex(settings.fileColor);
            folderColor_->setCurrentIndex(settings.folderColor);
            page->adjustSize();
            resize(std::max(560, page->sizeHint().width() + 40), 640);
        }

        void SettingsDialog::UpdateTips()
        {
            nameDelay_->setEnabled(nameTips_->isChecked());
            infoDelay_->setEnabled(infoTips_->isChecked());
            for (auto const& [box, flag] : flagBoxes_)
            {
                box->setEnabled(infoTips_->isChecked());
            }
        }

        // Every text in the dialog, in `lang`: the window follows a language
        // picked here (preview), and so does the dialog itself.
        void SettingsDialog::Retranslate(Lang const& lang)
        {
            lang_ = &lang;
            Wording const& wording = WordingFor(lang);
            setWindowTitle(Q(lang.settings));
            appearance_->setTitle(Q(lang.appearance));
            languageLabel_->setText(Q(lang.language));
            themeLabel_->setText(Q(lang.theme));
            themesFolder_->setText(Q(lang.themesFolder));
            backdropLabel_->setText(Q(lang.backgroundAnimation));
            backdropHint_->setText(Q(lang.backgroundAnimationHint));
            for (int i = 0; i < kBackdropCount; ++i)
            {
                backdropBox->setItemText(i, Q(lang.backdropNames.at(static_cast<std::size_t>(i))));
            }
            QString errors = Q(lang.themeErrors);
            for (std::wstring const& error : themeErrors_)
            {
                errors += QLatin1Char('\n') + Q(error);
            }
            themeErrorsLabel_->setText(errors);
            themeErrorsLabel_->setVisible(!themeErrors_.empty());
            if (themeBox->count() > 0)
            {
                FillThemes(W(themeBox->currentData().toString()));  // the classic themes' names are the language's
            }

            layoutGroup_->setTitle(Q(lang.layout));
            densityLabel_->setText(Q(lang.density));
            int const density = densityBox_->currentIndex();
            densityBox_->clear();
            densityBox_->addItems(Names(lang.densityNames));
            densityBox_->setCurrentIndex(density);
            biasLabel_->setText(Q(lang.bias));
            horz_->setText(Q(lang.horz));
            equal_->setText(Q(lang.equal));
            vert_->setText(Q(lang.vert));

            colors_->setTitle(Q(lang.displayColors));
            filesLabel_->setText(Q(lang.files));
            foldersLabel_->setText(Q(lang.folders));
            QStringList colorNames = Names(lang.colorNames);
            colorNames[kSchemeWindows] = Q(wording.windowsColors);
            for (QComboBox* const box : { fileColor_, folderColor_ })
            {
                int const picked = box->currentIndex();
                box->clear();
                box->addItems(colorNames);
                box->setCurrentIndex(picked);
            }

            tips_->setTitle(Q(lang.toolTips));
            nameTips_->setText(Q(lang.showNameTips));
            infoTips_->setText(Q(lang.showInfoTips));
            for (QSpinBox* const delay : { nameDelay_, infoDelay_ })
            {
                delay->setPrefix(Q(lang.delay) + QStringLiteral(": "));
            }
            std::array const captions = { lang.fullPath, lang.dateTime, lang.fileName,
                                          lang.fileSize, lang.icon,     lang.attrib };
            for (std::size_t i = 0; i < flagBoxes_.size(); ++i)
            {
                flagBoxes_[i].first->setText(Q(captions.at(i)));
            }
            rollover_->setText(Q(lang.showRolloverBox));

            misc_->setTitle(Q(lang.miscOptions));
            autoRescan_->setText(Q(lang.autoRescan));
            disableDelete_->setText(Q(lang.disableDelete));
            animatedZoom_->setText(Q(lang.animatedZoom));
            savePosition_->setText(Q(lang.savePos));

            shortcuts_->setTitle(Q(lang.keyboardShortcuts));
            FillShortcuts();
            ok_->setText(Q(lang.ok));
            cancel_->setText(Q(lang.cancel));
        }

        void SettingsDialog::FillThemes(std::wstring const& picked)
        {
            QSignalBlocker const quiet(themeBox);  // refilling is no pick
            themeBox->clear();
            Theme const& current = FindTheme(themes_, picked);
            for (Theme const& theme : themes_)
            {
                std::size_t const classic = theme.base == ThemeBase::System  ? 0
                                            : theme.base == ThemeBase::Light ? 1
                                                                             : 2;
                themeBox->addItem(theme.classic ? Q(lang_->themeNames.at(classic)) : Q(theme.name), Q(theme.id));
                if (&theme == &current)
                {
                    themeBox->setCurrentIndex(themeBox->count() - 1);
                }
            }
        }

        void SettingsDialog::SetThemes(std::vector<Theme> themes, std::vector<std::wstring> errors)
        {
            std::wstring const picked = W(themeBox->currentData().toString());
            themes_ = std::move(themes);
            themeErrors_ = std::move(errors);
            FillThemes(picked);
            Retranslate(*lang_);  // the errors
        }

        void SettingsDialog::FillShortcuts()
        {
            while (QLayoutItem const* const item = shortcutGrid_->takeAt(0))
            {
                delete item->widget();
                delete item;
            }
            Lang const& lang = *lang_;
            Wording const& wording = WordingFor(lang);
            int row = 0;
            for (Command const command : CommandsWithKeys())
            {
                auto* const keys = new QWidget(shortcuts_);
                auto* const keysLayout = new QHBoxLayout(keys);
                keysLayout->setContentsMargins(0, 0, 0, 0);
                keysLayout->setSpacing(4);
                bool first = true;
                for (KeyBinding const& binding : KeyBindings())
                {
                    if (binding.command != command)
                    {
                        continue;
                    }
                    if (!first)
                    {
                        keysLayout->addWidget(new QLabel(QStringLiteral("/"), keys));
                    }
                    first = false;
                    for (std::wstring const& key : KeysOf(binding, lang))
                    {
                        auto* const cap = new QLabel(Q(key), keys);
                        cap->setAlignment(Qt::AlignCenter);
                        cap->setMinimumWidth(24);
                        cap->setStyleSheet(
                            QStringLiteral("QLabel { border: 1px solid palette(mid); border-radius: 4px; "
                                           "padding: 1px 6px 2px 6px; background: palette(button); }"));
                        keysLayout->addWidget(cap);
                    }
                }
                keysLayout->addStretch();
                std::wstring_view const does =
                    command == Command::Delete ? wording.deleteHelp : ShortcutAction(command, lang);
                auto* const action = new QLabel(Q(does), shortcuts_);
                action->setWordWrap(true);
                shortcutGrid_->addWidget(keys, row, 0);
                shortcutGrid_->addWidget(action, row, 1);
                ++row;
            }
        }

        Settings SettingsDialog::Edited() const
        {
            // Out-of-range picks (-1: nothing selected) are clamped by Validate().
            Settings settings = original_;
            settings.language = W(languageBox->currentData().toString());
            settings.theme = W(themeBox->currentData().toString());
            settings.backdrop = static_cast<Backdrop>(backdropBox->currentIndex());
            settings.density = densityBox_->currentIndex() + Settings::kMinDensity;
            settings.bias = bias_->value();
            settings.fileColor = fileColor_->currentIndex();
            settings.folderColor = folderColor_->currentIndex();
            settings.showNameTips = nameTips_->isChecked();
            settings.nameTipDelay = nameDelay_->value();
            settings.showInfoTips = infoTips_->isChecked();
            settings.infoTipDelay = infoDelay_->value();
            std::uint32_t tipFlags = 0;
            for (auto const& [box, flag] : flagBoxes_)
            {
                tipFlags |= box->isChecked() ? static_cast<std::uint32_t>(flag) : 0u;
            }
            settings.infoTipFlags = tipFlags;
            settings.rolloverBox = rollover_->isChecked();
            settings.autoRescan = autoRescan_->isChecked();
            settings.disableDelete = disableDelete_->isChecked();
            settings.animatedZoom = animatedZoom_->isChecked();
            settings.savePosition = savePosition_->isChecked();
            settings.Validate();
            return settings;
        }
    }

    bool EditSettings(QWidget* parent, Lang const& lang, Settings& settings, std::vector<Theme> themes,
                      std::vector<std::wstring> themeErrors, SettingsHooks const& hooks)
    {
        Settings original = settings;
        original.language = std::wstring(lang.code);
        SettingsDialog dialog(parent, original, std::move(themes), std::move(themeErrors));

        // A theme, a language or an animation picked shows at once, as WinUI's
        // Settings does; Cancel puts back what there was.
        auto const tried = [&dialog, &hooks] {
            if (hooks.preview)
            {
                hooks.preview(dialog.Edited());
            }
        };
        QObject::connect(dialog.languageBox, &QComboBox::currentIndexChanged, &dialog, [&dialog, tried] {
            dialog.Retranslate(FindLanguage(W(dialog.languageBox->currentData().toString())));
            tried();
        });
        QObject::connect(dialog.themeBox, &QComboBox::currentIndexChanged, &dialog, tried);
        QObject::connect(dialog.backdropBox, &QComboBox::currentIndexChanged, &dialog, tried);
        // Back from editing a theme file: the themes as the files have them now.
        QObject::connect(qApp, &QGuiApplication::applicationStateChanged, &dialog,
                         [&dialog, &hooks, tried](Qt::ApplicationState state) {
                             if (state == Qt::ApplicationActive && hooks.reloadThemes)
                             {
                                 std::vector<std::wstring> errors;
                                 std::vector<Theme> reloaded = hooks.reloadThemes(errors);
                                 dialog.SetThemes(std::move(reloaded), std::move(errors));
                                 tried();
                             }
                         });

        bool const accepted = dialog.exec() == QDialog::Accepted;
        if (!accepted)
        {
            if (hooks.preview)
            {
                hooks.preview(original);
            }
            return false;
        }
        settings = dialog.Edited();
        return true;
    }

    // ---------------------------------------------------------------- about

    void ShowAbout(QWidget* parent, Lang const& lang)
    {
        Wording const& wording = WordingFor(lang);
        QString const text =
            QStringLiteral("<h3>SpaceWindow</h3><p>%1 &middot; Qt %2</p><p>Copyright &copy; 2026 recompiler<br>"
                           "SpaceMonger &copy; 1998&ndash;2020 Sean Werkema</p><p><b>%3</b></p><p>%4</p><p>%5</p>")
                .arg(QStringLiteral(SPACEWINDOW_VERSION), QString::fromLatin1(qVersion()),
                     Q(lang.freeware).toHtmlEscaped(), Q(lang.warranty).toHtmlEscaped(),
                     Q(wording.credits).toHtmlEscaped());
        QMessageBox box(parent);
        box.setWindowTitle(Q(lang.aboutApp));
        box.setIconPixmap(QApplication::windowIcon().pixmap(64, 64));
        box.setTextFormat(Qt::RichText);
        box.setText(text);
        box.addButton(Q(lang.ok), QMessageBox::AcceptRole);
        box.exec();
    }

    // ---------------------------------------------------------------- unreadable folders

    void ShowUnreadable(QWidget* parent, Lang const& lang, std::vector<std::wstring> const& paths)
    {
        constexpr std::size_t kListed = 1000;  // a longer list goes to the clipboard whole
        QString all;
        QString listed;
        for (std::size_t i = 0; i < paths.size(); ++i)
        {
            all += Q(paths[i]) + QLatin1Char('\n');
            if (i < kListed)
            {
                listed += (i == 0 ? QString() : QStringLiteral("\n")) + Q(paths[i]);
            }
        }
        if (paths.size() > kListed)
        {
            std::wstring const more = FormatCount(paths.size() - kListed, lang);
            listed += QLatin1Char('\n') + Q(FormatText(lang.moreFolders, { more }));
        }
        QDialog dialog(parent);
        dialog.setWindowTitle(Q(lang.unreadableTitle));
        dialog.resize(620, 420);
        auto* const layout = new QVBoxLayout(&dialog);
        auto* const intro = new QLabel(Q(WordingFor(lang).unreadableText), &dialog);
        intro->setWordWrap(true);
        layout->addWidget(intro);
        auto* const list = new QPlainTextEdit(listed, &dialog);
        list->setReadOnly(true);
        list->setLineWrapMode(QPlainTextEdit::NoWrap);
        layout->addWidget(list);
        auto* const buttons = new QDialogButtonBox(&dialog);
        QPushButton const* const copy = buttons->addButton(Q(lang.copyList), QDialogButtonBox::ActionRole);
        buttons->addButton(Q(lang.close), QDialogButtonBox::RejectRole)->setDefault(true);
        QObject::connect(copy, &QPushButton::clicked, &dialog, [all] { QApplication::clipboard()->setText(all); });
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        dialog.exec();
    }
}

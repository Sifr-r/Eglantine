// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <QMessageBox>
#include <QPushButton>
#include <QString>
#include <QTranslator>
#include "common/fs/path_util.h"
#include "qt_common/qt_string_lookup.h"
#include "user_data_migration.h"
#include "yuzu/migration_dialog.h"

// Needs to be included at the end due to https://bugreports.qt.io/browse/QTBUG-73263
#include <filesystem>
#include <QButtonGroup>
#include <QCheckBox>
#include <QEventLoop>
#include <QProgressDialog>
#include <QRadioButton>
#include <QThread>

bool UserDataMigrator::MigrateIfNeeded() {
    // NOTE: Logging is not initialized yet, do not produce logs here.

    if (!std::filesystem::is_directory(Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir))) {
        return ShowMigrationPrompt();
    }
    return true;
}

bool UserDataMigrator::ShowMigrationPrompt() {
    namespace fs = std::filesystem;
    using namespace QtCommon::StringLookup;

    MigrationDialog migration_prompt;
    migration_prompt.setWindowTitle(QObject::tr("Migration"));

    // mutually exclusive
    QButtonGroup* group = new QButtonGroup(&migration_prompt);

    // MACRO MADNESS, indeed

#define BUTTON(clazz, name, text, tooltip, checkState)                                             \
    clazz* name = new clazz(&migration_prompt);                                                    \
    name->setText(text);                                                                           \
    name->setToolTip(Lookup(tooltip));                                                             \
    name->setChecked(checkState);                                                                  \
    migration_prompt.addBox(name);

    BUTTON(QCheckBox, clear_shaders, QObject::tr("Clear Shader Cache"), MigrationTooltipClearShader,
           true)

#define RADIO(name, text, tooltip, checkState, strategy)                                           \
    BUTTON(QRadioButton, name, text, tooltip, checkState)                                          \
    group->addButton(name, static_cast<int>(MigrationWorker::MigrationStrategy::strategy));

    RADIO(keep_old, QObject::tr("Keep Old Data"), MigrationTooltipKeepOld, true, Copy)
    RADIO(clear_old, QObject::tr("Clear Old Data"), MigrationTooltipClearOld, false, Move)
    RADIO(link_old, QObject::tr("Link Old Directory"), MigrationTooltipLinkOld, false, Link)

#undef RADIO
#undef BUTTON

    std::vector<Emulator> found{};

    for (const Emulator& emu : legacy_emus)
        if (fs::is_directory(Common::FS::GetLegacyPath(emu.e_user_dir)))
            found.emplace_back(emu);

    if (found.empty()) {
        return true;
    }

    // makes my life easier
    qRegisterMetaType<Emulator>();

    QString prompt_text = Lookup(MigrationPromptPrefix);

    // natural language processing is a nightmare
    for (const Emulator& emu : found) {
        prompt_text = prompt_text % QStringLiteral("\n ") % emu.name();

        QAbstractButton* button = migration_prompt.addButton(emu.name());

        // This is cursed, but it's actually the most efficient way by a mile
        button->setProperty("emulator", QVariant::fromValue(emu));
    }

    prompt_text.append(QObject::tr("\n\n"));
    prompt_text = prompt_text % QStringLiteral("\n\n") % Lookup(MigrationPrompt);

    migration_prompt.setText(prompt_text);
    migration_prompt.addButton(QObject::tr("No"), true);

    migration_prompt.exec();

    QAbstractButton* button = migration_prompt.clickedButton();

    if (!button || button->text() == QObject::tr("No")) {
        ShowMigrationCancelledMessage();
        return true;
    }

    MigrationWorker::MigrationStrategy strategy =
        static_cast<MigrationWorker::MigrationStrategy>(group->checkedId());

    return MigrateUserData(button->property("emulator").value<Emulator>(),
                           clear_shaders->isChecked(), strategy);
}

void UserDataMigrator::ShowMigrationCancelledMessage() {
    using Common::FS::EdenPath;
    using Common::FS::GetEdenPathString;
    QMessageBox::information(
        nullptr, QObject::tr("Migration"),
        QObject::tr("Migration was skipped. This prompt appears when the configuration directory "
                    "is absent. Migration requires the destination data and cache directories to "
                    "be empty or absent to preserve existing files.\n\n"
                    "Configuration: %1\nData: %2\nCache: %3")
            .arg(QString::fromStdString(GetEdenPathString(EdenPath::ConfigDir)),
                 QString::fromStdString(GetEdenPathString(EdenPath::EdenDir)),
                 QString::fromStdString(GetEdenPathString(EdenPath::CacheDir))),
        QMessageBox::Ok);
}

bool UserDataMigrator::MigrateUserData(Emulator selected_emu, bool clear_shader_cache,
                                       MigrationWorker::MigrationStrategy strategy) {
    QProgressDialog progress;
    progress.setWindowTitle(QObject::tr("Migrating"));
    progress.setLabelText(QObject::tr("Migrating, this may take a while..."));
    progress.setRange(0, 0);
    progress.setCancelButton(nullptr);
    progress.setWindowModality(Qt::WindowModality::ApplicationModal);

    QEventLoop completion;
    QThread thread;
    auto* worker = new MigrationWorker(selected_emu, clear_shader_cache, strategy);
    worker->moveToThread(&thread);
    bool succeeded = false;
    QString success_message;
    QString error_message;
    QObject::connect(&thread, &QThread::started, worker, &MigrationWorker::process);
    QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
    QObject::connect(worker, &MigrationWorker::finished, &completion,
                     [&](const QString& message) {
                         success_message = message;
                         succeeded = true;
                         completion.quit();
                     });
    QObject::connect(worker, &MigrationWorker::error, &completion, [&](const QString& message) {
        error_message = message;
        completion.quit();
    });

    progress.show();
    thread.start();
    completion.exec();
    thread.quit();
    thread.wait();
    progress.close();
    if (!succeeded) {
        QMessageBox::critical(nullptr, QObject::tr("Migration Failed"), error_message,
                              QMessageBox::Ok);
        return false;
    }
    QMessageBox::information(nullptr, QObject::tr("Migration"), success_message, QMessageBox::Ok);
    return true;
}

// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGuiApplication>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "yuzu/diagnostics_dialog.h"
#include "yuzu/diagnostics_log.h"

namespace {

QLabel* MakeValueLabel(QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

QPlainTextEdit* MakeLogView(QWidget* parent, const QString& name) {
    auto* view = new QPlainTextEdit(parent);
    view->setAccessibleName(name);
    view->setReadOnly(true);
    view->setTabChangesFocus(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    return view;
}

QString DisplayedText(const QPlainTextEdit& view) {
    const QString text = view.toPlainText();
    return text.isEmpty() ? view.placeholderText() : text;
}

} // namespace

DiagnosticsDialog::DiagnosticsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Diagnostics"));
    resize(780, 520);

    auto* layout = new QVBoxLayout(this);
    auto* summary = new QFormLayout;
    gpu_value = MakeValueLabel(this);
    driver_value = MakeValueLabel(this);
    summary->addRow(tr("GPU"), gpu_value);
    summary->addRow(tr("Driver"), driver_value);
    layout->addLayout(summary);

    auto* tabs = new QTabWidget(this);
    const QString errors_title = tr("Recent errors");
    const QString tail_title = tr("Log tail");
    recent_errors = MakeLogView(tabs, errors_title);
    log_tail = MakeLogView(tabs, tail_title);
    tabs->addTab(recent_errors, errors_title);
    tabs->addTab(log_tail, tail_title);
    layout->addWidget(tabs, 1);

    auto* review_label = new QLabel(tr("Review the report before sharing it."), this);
    review_label->setWordWrap(true);
    layout->addWidget(review_label);
    status_label = new QLabel(this);
    status_label->setTextFormat(Qt::PlainText);
    status_label->setWordWrap(true);
    layout->addWidget(status_label);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    refresh_button = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    copy_button = buttons->addButton(tr("Copy All"), QDialogButtonBox::ActionRole);
    connect(refresh_button, &QPushButton::clicked, this, &DiagnosticsDialog::Refresh);
    connect(copy_button, &QPushButton::clicked, this, &DiagnosticsDialog::CopyAll);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    Refresh();
}

void DiagnosticsDialog::Refresh() {
    refresh_button->setEnabled(false);
    copy_button->setEnabled(false);
    status_label->setText(tr("Reading the session log…"));
    QTimer::singleShot(0, this, &DiagnosticsDialog::LoadSnapshot);
}

void DiagnosticsDialog::LoadSnapshot() {
    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / LOG_FILE;
    const auto snapshot =
        DiagnosticsLog::Read(QString::fromStdString(Common::FS::PathToUTF8String(path)));
    const QString unknown = tr("Unknown");
    gpu_value->setText(snapshot.gpu.isEmpty() ? unknown : snapshot.gpu);
    driver_value->setText(snapshot.driver.isEmpty() ? unknown : snapshot.driver);
    recent_errors->setPlainText(snapshot.recent_errors);
    log_tail->setPlainText(snapshot.log_tail);
    log_tail->verticalScrollBar()->setValue(log_tail->verticalScrollBar()->maximum());
    truncated = snapshot.truncated;

    if (snapshot.readable) {
        recent_errors->setPlaceholderText(tr("No recent errors in the log excerpt."));
        log_tail->setPlaceholderText(tr("No log entries yet."));
        status_label->setText(truncated ? tr("Showing a limited log excerpt.") : QString{});
    } else {
        recent_errors->setPlaceholderText({});
        log_tail->setPlaceholderText({});
        status_label->setText(tr("The session log is missing or could not be read."));
    }
    refresh_button->setEnabled(true);
    copy_button->setEnabled(snapshot.readable);
}

void DiagnosticsDialog::CopyAll() {
    QString report = tr("GPU\n%1\n\nDriver\n%2\n\nRecent errors\n%3\n\nLog tail\n%4")
                         .arg(gpu_value->text(), driver_value->text(),
                              DisplayedText(*recent_errors), DisplayedText(*log_tail));
    if (truncated) {
        report += QStringLiteral("\n\n") + tr("Showing a limited log excerpt.");
    }
    QGuiApplication::clipboard()->setText(report);
    status_label->setText(tr("Copied to clipboard."));
}

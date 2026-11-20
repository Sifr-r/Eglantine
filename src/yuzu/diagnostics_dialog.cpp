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
#include <QTextDocument>
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

DiagnosticsLog::Snapshot ReadSessionLog() {
    const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / LOG_FILE;
    return DiagnosticsLog::Read(QString::fromStdString(Common::FS::PathToUTF8String(path)));
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

    tabs = new QTabWidget(this);
    const QString actionable_title = tr("Actionable issues");
    const QString errors_title = tr("Recent errors");
    const QString tail_title = tr("Log tail");
    actionable_issues = MakeLogView(tabs, actionable_title);
    recent_errors = MakeLogView(tabs, errors_title);
    log_tail = MakeLogView(tabs, tail_title);
    tabs->addTab(actionable_issues, actionable_title);
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
    copy_button = buttons->addButton(tr("Copy Issues"), QDialogButtonBox::ActionRole);
    copy_button->setEnabled(false);
    connect(refresh_button, &QPushButton::clicked, this, &DiagnosticsDialog::Refresh);
    connect(copy_button, &QPushButton::clicked, this, &DiagnosticsDialog::CopyReport);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(tabs, &QTabWidget::currentChanged, this, &DiagnosticsDialog::UpdateControls);
    layout->addWidget(buttons);

    live_timer = new QTimer(this);
    live_timer->setInterval(2000);
    connect(live_timer, &QTimer::timeout, this, &DiagnosticsDialog::RefreshActionableIssues);
}

void DiagnosticsDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    Refresh();
    live_timer->start();
}

void DiagnosticsDialog::hideEvent(QHideEvent* event) {
    live_timer->stop();
    QDialog::hideEvent(event);
}

void DiagnosticsDialog::Refresh() {
    if (!refresh_button->isEnabled()) {
        return;
    }
    refresh_button->setEnabled(false);
    copy_button->setEnabled(false);
    status_label->setText(tr("Reading the session log…"));
    QTimer::singleShot(0, this, &DiagnosticsDialog::LoadSnapshot);
}

void DiagnosticsDialog::LoadSnapshot() {
    const auto snapshot = ReadSessionLog();
    const QString unknown = tr("Unknown");
    gpu_value->setText(snapshot.gpu.isEmpty() ? unknown : snapshot.gpu);
    driver_value->setText(snapshot.driver.isEmpty() ? unknown : snapshot.driver);
    recent_errors->setPlainText(snapshot.recent_errors);
    log_tail->setPlainText(snapshot.log_tail);
    log_tail->verticalScrollBar()->setValue(log_tail->verticalScrollBar()->maximum());
    report_readable = snapshot.readable;
    truncated = snapshot.truncated;

    if (snapshot.readable) {
        recent_errors->setPlaceholderText(tr("No recent errors in the log excerpt."));
        log_tail->setPlaceholderText(tr("No log entries yet."));
    } else {
        recent_errors->setPlaceholderText({});
        log_tail->setPlaceholderText({});
    }
    UpdateActionableIssues(snapshot);
    refresh_button->setEnabled(true);
    UpdateControls();
}

void DiagnosticsDialog::RefreshActionableIssues() {
    UpdateActionableIssues(ReadSessionLog());
}

void DiagnosticsDialog::UpdateActionableIssues(const DiagnosticsLog::Snapshot& snapshot) {
    if (actionable_issues->toPlainText() != snapshot.actionable_items) {
        actionable_issues->setPlainText(snapshot.actionable_items);
    }
    actionable_issues->setPlaceholderText(
        snapshot.readable ? tr("No actionable issues in the recent log.")
                          : tr("The session log is missing or could not be read."));
    if (tabs->currentWidget() == actionable_issues) {
        copy_button->setEnabled(refresh_button->isEnabled() && !snapshot.actionable_items.isEmpty());
    }
}

void DiagnosticsDialog::UpdateControls() {
    const bool live_tab = tabs->currentWidget() == actionable_issues;
    copy_button->setText(live_tab ? tr("Copy Issues") : tr("Copy All"));
    const bool can_copy = live_tab ? !actionable_issues->document()->isEmpty() : report_readable;
    copy_button->setEnabled(refresh_button->isEnabled() && can_copy);
    if (!refresh_button->isEnabled()) {
        return;
    }

    QString status;
    if (!live_tab) {
        if (!report_readable) {
            status = tr("The session log is missing or could not be read.");
        } else if (truncated) {
            status = tr("Showing a limited log excerpt.");
        }
    }
    status_label->setText(status);
}

void DiagnosticsDialog::CopyReport() {
    if (tabs->currentWidget() == actionable_issues) {
        QGuiApplication::clipboard()->setText(actionable_issues->toPlainText());
        status_label->setText(tr("Copied to clipboard."));
        return;
    }

    QString report = tr("GPU\n%1\n\nDriver\n%2\n\nRecent errors\n%3\n\nLog tail\n%4")
                         .arg(gpu_value->text(), driver_value->text(),
                              DisplayedText(*recent_errors), DisplayedText(*log_tail));
    if (truncated) {
        report += QStringLiteral("\n\n") + tr("Showing a limited log excerpt.");
    }
    QGuiApplication::clipboard()->setText(report);
    status_label->setText(tr("Copied to clipboard."));
}

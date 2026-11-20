// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QHideEvent;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QShowEvent;
class QTabWidget;
class QTimer;

namespace DiagnosticsLog {
struct Snapshot;
}

class DiagnosticsDialog : public QDialog {
    Q_OBJECT

public:
    explicit DiagnosticsDialog(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void Refresh();
    void LoadSnapshot();
    void RefreshActionableIssues();
    void UpdateActionableIssues(const DiagnosticsLog::Snapshot& snapshot);
    void UpdateControls();
    void CopyReport();

    QLabel* gpu_value{};
    QLabel* driver_value{};
    QLabel* status_label{};
    QTabWidget* tabs{};
    QPlainTextEdit* actionable_issues{};
    QPlainTextEdit* recent_errors{};
    QPlainTextEdit* log_tail{};
    QPushButton* refresh_button{};
    QPushButton* copy_button{};
    QTimer* live_timer{};
    bool report_readable{};
    bool truncated{};
};

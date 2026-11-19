// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

class QLabel;
class QPlainTextEdit;
class QPushButton;

class DiagnosticsDialog : public QDialog {
    Q_OBJECT

public:
    explicit DiagnosticsDialog(QWidget* parent = nullptr);

private:
    void Refresh();
    void LoadSnapshot();
    void CopyAll();

    QLabel* gpu_value{};
    QLabel* driver_value{};
    QLabel* status_label{};
    QPlainTextEdit* recent_errors{};
    QPlainTextEdit* log_tail{};
    QPushButton* refresh_button{};
    QPushButton* copy_button{};
    bool truncated{};
};

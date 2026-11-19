// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include <QByteArray>
#include <QByteArrayView>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringView>

#include "yuzu/diagnostics_log.h"

namespace DiagnosticsLog {
namespace {

constexpr qint64 StartupBytes = 512 * 1024;
constexpr qint64 TailBytes = 256 * 1024;

enum class Renderer { Unknown, Vulkan, OpenGL };

QString CompleteLines(QByteArrayView bytes) {
    const auto last_newline = bytes.lastIndexOf('\n');
    return QString::fromUtf8(bytes.first(last_newline + 1))
        .replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
}

void ReadRecords(QStringView text, Snapshot& snapshot, bool collect_errors) {
    // The logger writes a timestamp, class, level and source before the message.
    static const QRegularExpression header{QStringLiteral(
        R"(^\[ *[0-9]+\.[0-9]{6}\] ([A-Za-z0-9_.?]+) <(Trace|Debug|Info|Warning|Error|Critical|\?)> ((?:[A-Za-z]:)?[^:\r\n]+):[0-9]+:((?:[^:\r\n]|::)+): )")};
    Renderer renderer = Renderer::Unknown;
    bool error_record = false;
    for (qsizetype start = 0; start < text.size();) {
        const auto end = text.indexOf(u'\n', start) + 1;
        const auto line = text.sliced(start, end - start);
        start = end;
        const auto match = line.startsWith(u'[') ? header.match(line.toString())
                                                : QRegularExpressionMatch{};
        if (!match.hasMatch()) {
            if (collect_errors && error_record) {
                snapshot.recent_errors += line;
            }
            continue;
        }

        const auto level = match.capturedView(2);
        error_record = level == u"Error" || level == u"Critical";
        if (collect_errors && error_record) {
            snapshot.recent_errors += line;
        }
        if (level != u"Info") {
            continue;
        }

        const auto category = match.capturedView(1);
        const auto source = match.capturedView(3);
        const auto file_name =
            source.sliced(std::max(source.lastIndexOf(u'/'), source.lastIndexOf(u'\\')) + 1);
        const auto function = match.capturedView(4);
        const auto message = line.sliced(match.capturedEnd());
        Renderer record_renderer;
        QStringView gpu_prefix;
        QStringView driver_prefix;
        if (category == u"Render.Vulkan" && file_name == u"renderer_vulkan.cpp" &&
            function == u"Report") {
            record_renderer = Renderer::Vulkan;
            gpu_prefix = u"Device: ";
            driver_prefix = u"Driver: ";
        } else if (category == u"Render.OpenGL" && file_name == u"renderer_opengl.cpp" &&
                   function == u"AddTelemetryFields") {
            record_renderer = Renderer::OpenGL;
            gpu_prefix = u"GL_RENDERER: ";
            driver_prefix = u"GL_VERSION: ";
        } else {
            continue;
        }

        if (message.startsWith(driver_prefix)) {
            // Each renderer report starts with its driver/version.
            snapshot.gpu.clear();
            snapshot.driver = message.sliced(driver_prefix.size()).trimmed().toString();
            renderer = record_renderer;
        } else if (message.startsWith(gpu_prefix)) {
            if (record_renderer != renderer) {
                snapshot.driver.clear();
            }
            snapshot.gpu = message.sliced(gpu_prefix.size()).trimmed().toString();
            renderer = record_renderer;
        }
    }
}

} // namespace

Snapshot Read(const QString& path) {
    QFile file(path);
    if (!QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly)) {
        return {};
    }

    const qint64 size = file.size();
    const qint64 tail_start = size > TailBytes ? size - TailBytes : 0;
    Snapshot snapshot;
    if (tail_start > 0) {
        // Renderer details are normally written near the start, even when the log is large.
        const auto startup = file.read(StartupBytes);
        if (file.error() != QFileDevice::NoError || !file.seek(tail_start - 1)) {
            return {};
        }
        ReadRecords(CompleteLines(startup), snapshot, false);
    }

    // Read the byte before the tail to tell an exact line boundary from a split line.
    const auto tail = file.read(size - tail_start + (tail_start > 0 ? 1 : 0));
    if (file.error() != QFileDevice::NoError) {
        return {};
    }
    snapshot.readable = true;
    snapshot.truncated = tail_start > 0 || tail.size() < size ||
                         (!tail.isEmpty() && !tail.endsWith('\n'));
    const qsizetype first_line = tail_start > 0 ? tail.indexOf('\n') + 1 : 0;
    if (tail_start == 0 || first_line > 0) {
        snapshot.log_tail = CompleteLines(QByteArrayView{tail}.sliced(first_line));
        ReadRecords(snapshot.log_tail, snapshot, true);
    }
    return snapshot;
}

} // namespace DiagnosticsLog

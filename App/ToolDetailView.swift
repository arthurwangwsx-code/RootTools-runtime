import SwiftUI

struct ToolDetailView: View {
    let tool: ToolKind
    @State private var output = "Loading…"
    @State private var loading = true
    @State private var error: String?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                HStack(spacing: 12) {
                    Image(systemName: tool.symbol).font(.title2)
                        .frame(width: 44, height: 44)
                        .background(Color.accentColor.opacity(0.18), in: RoundedRectangle(cornerRadius: 13))
                    VStack(alignment: .leading, spacing: 3) {
                        Text(tool.title).font(.headline)
                        Text(tool.subtitle).font(.caption).foregroundStyle(.secondary)
                    }
                }

                if let error {
                    Label(error, systemImage: "exclamationmark.triangle.fill")
                        .font(.caption).foregroundStyle(.orange)
                        .padding(12)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(Color.orange.opacity(0.1), in: RoundedRectangle(cornerRadius: 14))
                }

                Text(output.isEmpty ? "No data" : output)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(14)
                    .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 18))
            }
            .padding(16)
        }
        .background(Color(uiColor: .systemGroupedBackground).ignoresSafeArea())
        .navigationTitle(tool.title)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                if loading { ProgressView().controlSize(.small) }
                else { Button { Task { await load() } } label: { Image(systemName: "arrow.clockwise") } }
            }
        }
        .task { await load() }
    }

    @MainActor
    private func load() async {
        loading = true
        defer { loading = false }
        do {
            let payload = try await DaemonClient.shared.text(path: tool.endpoint)
            output = payload.output
            error = nil
        } catch {
            self.error = error.localizedDescription
            output = ""
        }
    }
}

struct PerformanceView: View {
    @State private var snapshot: DevicePerformanceSnapshot?
    @State private var loading = false
    @State private var errorMessage: String?

    private let columns = [GridItem(.flexible(), spacing: 10), GridItem(.flexible(), spacing: 10)]

    var body: some View {
        ScrollView {
            VStack(spacing: 16) {
                if let snapshot {
                    LazyVGrid(columns: columns, spacing: 10) {
                        PerformanceMetricCard(title: "Uptime", value: formatDuration(snapshot.uptimeSeconds), symbol: "clock.fill")
                        PerformanceMetricCard(title: "Processes", value: "\(snapshot.processCount)", symbol: "waveform.path.ecg")
                        PerformanceMetricCard(title: "Daemon RSS", value: formatBytes(snapshot.daemon.residentBytes), symbol: "cpu.fill")
                        PerformanceMetricCard(title: "Active Tasks", value: "\(snapshot.activeTaskCount)", symbol: "bolt.horizontal.circle.fill")
                    }

                    performanceSection(title: "System load", symbol: "gauge.with.dots.needle.67percent") {
                        if snapshot.loadAverage.available {
                            HStack(spacing: 10) {
                                PerformanceValue(title: "1 min", value: String(format: "%.2f", snapshot.loadAverage.oneMinute))
                                PerformanceValue(title: "5 min", value: String(format: "%.2f", snapshot.loadAverage.fiveMinute))
                                PerformanceValue(title: "15 min", value: String(format: "%.2f", snapshot.loadAverage.fifteenMinute))
                            }
                        } else {
                            Text("Load average unavailable").font(.caption).foregroundStyle(.secondary)
                        }
                        Text("\(snapshot.cpuCount) logical CPU cores")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }

                    performanceSection(title: "Memory", symbol: "memorychip.fill") {
                        if snapshot.memory.available {
                            metricRow("Total", formatBytes(snapshot.memory.totalBytes))
                            metricRow("Free", formatBytes(snapshot.memory.freeBytes))
                            metricRow("Active", formatBytes(snapshot.memory.activeBytes))
                            metricRow("Inactive", formatBytes(snapshot.memory.inactiveBytes))
                            metricRow("Wired", formatBytes(snapshot.memory.wiredBytes))
                        } else {
                            Text("VM memory statistics unavailable").font(.caption).foregroundStyle(.secondary)
                        }
                    }

                    performanceSection(title: "Storage & runtime", symbol: "internaldrive.fill") {
                        metricRow("/ free", formatBytes(snapshot.storage.rootFreeBytes))
                        metricRow("/var free", formatBytes(snapshot.storage.varFreeBytes))
                        metricRow("Providers", "\(snapshot.providers.ready) / \(snapshot.providers.total) ready")
                        metricRow("Daemon PID", "\(snapshot.daemon.pid)")
                    }
                } else if loading {
                    ProgressView("Reading performance snapshot…")
                        .frame(maxWidth: .infinity)
                        .padding(.top, 40)
                }

                if let errorMessage {
                    Label(errorMessage, systemImage: "exclamationmark.triangle.fill")
                        .font(.caption)
                        .foregroundStyle(.orange)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            .padding(16)
        }
        .background(Color(uiColor: .systemGroupedBackground).ignoresSafeArea())
        .navigationTitle("Performance")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                Button { Task { await load() } } label: { Image(systemName: "arrow.clockwise") }
                    .disabled(loading)
            }
        }
        .task { await load() }
    }

    @ViewBuilder
    private func performanceSection<Content: View>(title: String, symbol: String, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            Label(title, systemImage: symbol).font(.headline)
            content()
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 18))
    }

    private func metricRow(_ title: String, _ value: String) -> some View {
        HStack {
            Text(title).foregroundStyle(.secondary)
            Spacer()
            Text(value).font(.system(.body, design: .monospaced))
        }
        .font(.caption)
    }

    @MainActor
    private func load() async {
        guard !loading else { return }
        loading = true
        defer { loading = false }
        do {
            snapshot = try await DaemonClient.shared.performance()
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func formatBytes(_ bytes: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .memory)
    }

    private func formatDuration(_ seconds: UInt64) -> String {
        let days = seconds / 86_400
        let hours = (seconds % 86_400) / 3_600
        if days > 0 { return "\(days)d \(hours)h" }
        let minutes = (seconds % 3_600) / 60
        return "\(hours)h \(minutes)m"
    }
}

private struct PerformanceMetricCard: View {
    let title: String
    let value: String
    let symbol: String

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Image(systemName: symbol).foregroundStyle(.secondary)
            Text(value).font(.headline).lineLimit(1).minimumScaleFactor(0.7)
            Text(title).font(.caption2).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, minHeight: 96, alignment: .leading)
        .padding(14)
        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 16))
    }
}

private struct PerformanceValue: View {
    let title: String
    let value: String

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(value).font(.headline.monospacedDigit())
            Text(title).font(.caption2).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(10)
        .background(Color.primary.opacity(0.05), in: RoundedRectangle(cornerRadius: 12))
    }
}


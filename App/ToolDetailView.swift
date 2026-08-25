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

struct ApplicationsManagerView: View {
    @State private var applications: [ApplicationDescriptor] = []
    @State private var query = ""
    @State private var loading = false
    @State private var errorMessage: String?

    private var filtered: [ApplicationDescriptor] {
        let normalized = query.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        let source = normalized.isEmpty ? applications : applications.filter {
            $0.displayName.lowercased().contains(normalized) ||
            $0.bundleID.lowercased().contains(normalized) ||
            $0.executable.lowercased().contains(normalized)
        }
        return source.sorted {
            if $0.running != $1.running { return $0.running && !$1.running }
            return $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending
        }
    }

    var body: some View {
        List {
            if let errorMessage {
                Section { Label(errorMessage, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange) }
            }
            Section {
                HStack {
                    Label("\(applications.count) applications", systemImage: "square.grid.2x2.fill")
                    Spacer()
                    Text("\(applications.filter(\.running).count) running")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            Section("Installed applications") {
                ForEach(filtered) { app in
                    NavigationLink {
                        ApplicationDetailView(application: app)
                    } label: {
                        HStack(spacing: 12) {
                            ZStack {
                                RoundedRectangle(cornerRadius: 11).fill(Color.accentColor.opacity(0.11))
                                Image(systemName: app.source == "jailbreak" ? "shippingbox.fill" : "app.fill")
                                    .foregroundStyle(.secondary)
                            }
                            .frame(width: 40, height: 40)
                            VStack(alignment: .leading, spacing: 3) {
                                HStack(spacing: 6) {
                                    Text(app.displayName).font(.subheadline.weight(.semibold)).lineLimit(1)
                                    if app.running { Circle().fill(Color.green).frame(width: 6, height: 6) }
                                }
                                Text(app.bundleID).font(.caption2.monospaced()).foregroundStyle(.secondary).lineLimit(1)
                                Text("v\(app.version) (\(app.build)) · \(app.source.capitalized)")
                                    .font(.caption2).foregroundStyle(.tertiary)
                            }
                        }
                        .padding(.vertical, 3)
                    }
                }
            }
        }
        .navigationTitle("Applications")
        .navigationBarTitleDisplayMode(.inline)
        .searchable(text: $query, prompt: "Name or bundle ID")
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                if loading { ProgressView().controlSize(.small) }
                else { Button { Task { await load() } } label: { Image(systemName: "arrow.clockwise") } }
            }
        }
        .task { await load() }
        .refreshable { await load() }
    }

    @MainActor
    private func load() async {
        guard !loading else { return }
        loading = true
        defer { loading = false }
        do {
            applications = try await DaemonClient.shared.applicationCatalog().applications
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}

private struct ApplicationDetailView: View {
    let application: ApplicationDescriptor
    @State private var inspection: ApplicationInspection?
    @State private var busy = false
    @State private var resultMessage: String?

    var body: some View {
        List {
            Section("Identity") {
                detailRow("Name", inspection?.displayName ?? application.displayName)
                detailRow("Bundle ID", application.bundleID, monospaced: true)
                detailRow("Executable", application.executable, monospaced: true)
                detailRow("Version", inspection?.version ?? application.version)
                detailRow("Build", inspection?.build ?? application.build)
                detailRow("Source", inspection?.source ?? application.source)
            }
            Section("Runtime") {
                Label((inspection?.running ?? application.running) ? "Running" : "Not running", systemImage: (inspection?.running ?? application.running) ? "play.circle.fill" : "stop.circle")
                    .foregroundStyle((inspection?.running ?? application.running) ? Color.green : Color.secondary)
                if application.critical { Label("Critical application", systemImage: "lock.shield.fill") }
            }
            Section("Location") {
                Text(inspection?.bundlePath ?? application.path)
                    .font(.caption.monospaced())
                    .textSelection(.enabled)
            }
            Section("Actions") {
                Button { Task { await launch() } } label: { Label("Launch", systemImage: "play.fill") }
                Button(role: .destructive) { Task { await terminate() } } label: { Label("Terminate", systemImage: "stop.fill") }
                    .disabled(application.critical)
            }
            if let resultMessage {
                Section("Last result") { Text(resultMessage).font(.caption).textSelection(.enabled) }
            }
        }
        .navigationTitle(application.displayName)
        .navigationBarTitleDisplayMode(.inline)
        .disabled(busy)
        .task { await refresh() }
    }

    private func detailRow(_ title: String, _ value: String, monospaced: Bool = false) -> some View {
        HStack(alignment: .top) {
            Text(title).foregroundStyle(.secondary)
            Spacer()
            if monospaced { Text(value).font(.caption.monospaced()).textSelection(.enabled) }
            else { Text(value).font(.caption).textSelection(.enabled) }
        }
    }

    @MainActor private func refresh() async { inspection = try? await DaemonClient.shared.inspectApp(bundleID: application.bundleID) }

    @MainActor
    private func launch() async {
        busy = true; defer { busy = false }
        do {
            let receipt = try await DaemonClient.shared.launchApp(bundleID: application.bundleID)
            resultMessage = receipt.message
            await refresh()
        } catch { resultMessage = error.localizedDescription }
    }

    @MainActor
    private func terminate() async {
        busy = true; defer { busy = false }
        do {
            let receipt = try await DaemonClient.shared.terminateApp(bundleID: application.bundleID)
            resultMessage = receipt.message
            await refresh()
        } catch { resultMessage = error.localizedDescription }
    }
}

struct ProcessesManagerView: View {
    @State private var processes: [ProcessDescriptor] = []
    @State private var query = ""
    @State private var loading = false
    @State private var errorMessage: String?

    private var filtered: [ProcessDescriptor] {
        let normalized = query.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        let rows = normalized.isEmpty ? processes : processes.filter {
            $0.command.lowercased().contains(normalized) || String($0.pid).contains(normalized) || String($0.uid).contains(normalized)
        }
        return rows.sorted {
            if $0.privileged != $1.privileged { return !$0.privileged && $1.privileged }
            return $0.pid < $1.pid
        }
    }

    var body: some View {
        List {
            if let errorMessage { Section { Label(errorMessage, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange) } }
            Section {
                HStack {
                    Label("\(processes.count) processes", systemImage: "waveform.path.ecg.rectangle.fill")
                    Spacer()
                    Text("\(processes.filter(\.privileged).count) UID 0").font(.caption).foregroundStyle(.secondary)
                }
            }
            Section("Running processes") {
                ForEach(filtered) { process in
                    NavigationLink {
                        ProcessDetailView(process: process)
                    } label: {
                        VStack(alignment: .leading, spacing: 4) {
                            HStack {
                                Text(process.command).font(.subheadline.weight(.semibold)).lineLimit(1)
                                Spacer()
                                if process.privileged {
                                    Text("ROOT").font(.caption2.weight(.bold)).foregroundStyle(.orange)
                                }
                            }
                            Text("PID \(process.pid) · UID \(process.uid)")
                                .font(.caption2.monospaced()).foregroundStyle(.secondary)
                        }
                        .padding(.vertical, 3)
                    }
                }
            }
        }
        .navigationTitle("Processes")
        .navigationBarTitleDisplayMode(.inline)
        .searchable(text: $query, prompt: "Process, PID or UID")
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                if loading { ProgressView().controlSize(.small) }
                else { Button { Task { await load() } } label: { Image(systemName: "arrow.clockwise") } }
            }
        }
        .task { await load() }
        .refreshable { await load() }
    }

    @MainActor
    private func load() async {
        guard !loading else { return }
        loading = true; defer { loading = false }
        do {
            processes = try await DaemonClient.shared.processCatalog().processes
            errorMessage = nil
        } catch { errorMessage = error.localizedDescription }
    }
}

private struct ProcessDetailView: View {
    let process: ProcessDescriptor
    @State private var inspection: ProcessInspection?
    @State private var busy = false
    @State private var showTerminate = false
    @State private var resultMessage: String?

    var body: some View {
        List {
            Section("Identity") {
                LabeledContent("PID", value: "\(process.pid)")
                LabeledContent("UID", value: "\(process.uid)")
                LabeledContent("Command", value: process.command)
                if process.privileged { Label("UID 0 privileged process", systemImage: "lock.shield.fill").foregroundStyle(.orange) }
                if process.critical { Label("Critical process", systemImage: "exclamationmark.shield.fill").foregroundStyle(.orange) }
            }
            if let metrics = inspection?.metrics, inspection?.metricsAvailable == true {
                Section("Resources") {
                    metric("Footprint", bytes(metrics.footprintBytes))
                    metric("Resident", bytes(metrics.residentBytes))
                    metric("CPU user", duration(metrics.userTimeNs))
                    metric("CPU system", duration(metrics.systemTimeNs))
                    metric("Disk read", bytes(metrics.diskReadBytes))
                    metric("Disk write", bytes(metrics.diskWriteBytes))
                    metric("Page-ins", "\(metrics.pageins)")
                    metric("Idle wakeups", "\(metrics.idleWakeups)")
                    metric("Interrupt wakeups", "\(metrics.interruptWakeups)")
                }
            } else {
                Section("Resources") { Text("Detailed process metrics are unavailable for this process.").font(.caption).foregroundStyle(.secondary) }
            }
            if !process.privileged && !process.critical {
                Section("Actions") {
                    Button("Terminate process", role: .destructive) { showTerminate = true }
                }
            }
            if let resultMessage { Section("Last result") { Text(resultMessage).font(.caption).textSelection(.enabled) } }
        }
        .navigationTitle(process.command)
        .navigationBarTitleDisplayMode(.inline)
        .disabled(busy)
        .task { await refresh() }
        .confirmationDialog("Terminate PID \(process.pid)?", isPresented: $showTerminate, titleVisibility: .visible) {
            Button("Terminate", role: .destructive) { Task { await terminate() } }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("This is an R2 action. The daemon still rejects UID 0 and critical processes.")
        }
    }

    private func metric(_ title: String, _ value: String) -> some View {
        HStack { Text(title).foregroundStyle(.secondary); Spacer(); Text(value).font(.caption.monospacedDigit()) }
    }

    private func bytes(_ value: UInt64) -> String { ByteCountFormatter.string(fromByteCount: Int64(value), countStyle: .memory) }
    private func duration(_ nanoseconds: UInt64) -> String { String(format: "%.2f s", Double(nanoseconds) / 1_000_000_000) }

    @MainActor private func refresh() async { inspection = try? await DaemonClient.shared.inspectProcess(pid: process.pid) }

    @MainActor
    private func terminate() async {
        busy = true; defer { busy = false }
        do {
            let receipt = try await DaemonClient.shared.terminateProcess(pid: process.pid, confirmed: true)
            resultMessage = receipt.message
        } catch { resultMessage = error.localizedDescription }
    }
}


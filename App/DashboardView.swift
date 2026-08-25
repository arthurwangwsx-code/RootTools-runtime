import SwiftUI

struct DashboardView: View {
    @EnvironmentObject private var store: DeviceStore
    private let columns = [GridItem(.flexible(), spacing: 12), GridItem(.flexible(), spacing: 12)]

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    hero
                    healthStrip
                    NavigationLink {
                        ActionsView()
                    } label: {
                        HStack(spacing: 14) {
                            Image(systemName: "switch.2")
                                .font(.title2)
                                .frame(width: 44, height: 44)
                                .background(Color.orange.opacity(0.16), in: RoundedRectangle(cornerRadius: 13))
                            VStack(alignment: .leading, spacing: 4) {
                                Text("Controlled Actions").font(.subheadline.weight(.semibold)).foregroundStyle(.primary)
                                Text("Typed R1/R2 operations · audited · no raw shell")
                                    .font(.caption2).foregroundStyle(.secondary)
                            }
                            Spacer()
                            Image(systemName: "chevron.right").font(.caption.weight(.bold)).foregroundStyle(.tertiary)
                        }
                        .padding(15)
                        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 18))
                    }
                    .buttonStyle(.plain)
                    LazyVGrid(columns: columns, spacing: 12) {
                        ForEach(ToolKind.allCases) { tool in
                            NavigationLink(value: tool) {
                                ToolCard(tool: tool, status: store.status)
                            }
                            .buttonStyle(.plain)
                        }
                    }
                    capabilityFooter
                }
                .padding(16)
            }
            .background(Color(uiColor: .systemGroupedBackground).ignoresSafeArea())
            .navigationTitle("Overview")
            .navigationDestination(for: ToolKind.self) { tool in
                RootToolDestination(tool: tool)
            }
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button {
                        Task { await store.refresh() }
                    } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                }
            }
            .task { await store.refresh() }
        }
    }

    private var hero: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(alignment: .top) {
                ZStack {
                    RoundedRectangle(cornerRadius: 16).fill(.ultraThinMaterial).frame(width: 54, height: 54)
                    Image(systemName: "iphone.gen3.radiowaves.left.and.right").font(.title2)
                }
                VStack(alignment: .leading, spacing: 4) {
                    Text(store.status.machine).font(.headline)
                    Text("iOS build \(store.status.osBuild)").font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                StatusPill(ok: store.daemonReachable, text: store.daemonReachable ? "ROOT ONLINE" : "OFFLINE")
            }

            HStack(spacing: 9) {
                MiniBadge(text: store.status.jailbreakRootless ? "ROOTLESS" : "NO JB", symbol: "checkmark.shield.fill")
                MiniBadge(text: "UID \(store.status.uid)", symbol: "person.badge.key.fill")
                MiniBadge(text: "v\(store.status.daemonVersion)", symbol: "cpu.fill")
            }
        }
        .padding(18)
        .background(
            LinearGradient(colors: [Color.indigo.opacity(0.55), Color.blue.opacity(0.22)], startPoint: .topLeading, endPoint: .bottomTrailing),
            in: RoundedRectangle(cornerRadius: 24)
        )
        .overlay(RoundedRectangle(cornerRadius: 24).stroke(Color.white.opacity(0.08)))
    }

    private var healthStrip: some View {
        HStack(spacing: 10) {
            StatTile(title: "CPU", value: store.status.cpuCount == 0 ? "—" : "\(store.status.cpuCount) cores")
            StatTile(title: "Memory", value: formatBytes(store.status.memoryBytes))
            StatTile(title: "/var free", value: formatBytes(store.status.varFreeBytes))
        }
    }

    private var capabilityFooter: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Device capability truth").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
            if store.capabilities.isEmpty {
                Text("Capability catalog is unavailable until the v0.3 control plane is installed.")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            } else {
                HStack(spacing: 8) {
                    RiskBadge(risk: "R0", count: enabledCapabilityCount(risk: "R0"))
                    RiskBadge(risk: "R1", count: enabledCapabilityCount(risk: "R1"))
                    RiskBadge(risk: "R2", count: enabledCapabilityCount(risk: "R2"))
                    RiskBadge(risk: "R3", count: enabledCapabilityCount(risk: "R3"))
                }
                if let invariants = store.capabilityInvariants {
                    Label(
                        invariants.rawPrivilegedShellExposed || invariants.r3Exposed
                            ? "Execution policy needs attention"
                            : "R3 and raw privileged shell are blocked",
                        systemImage: invariants.rawPrivilegedShellExposed || invariants.r3Exposed
                            ? "exclamationmark.shield.fill"
                            : "checkmark.shield.fill"
                    )
                    .font(.caption2.weight(.medium))
                    .foregroundStyle(invariants.rawPrivilegedShellExposed || invariants.r3Exposed ? Color.orange : Color.secondary)
                }
            }

            Divider()
            Text("Lock-aware execution").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
            HStack(spacing: 8) {
                CapabilityDot(label: (store.status.lockState ?? "unknown").uppercased(), ready: store.status.deviceLocked == false)
                CapabilityDot(label: "HEADLESS", ready: store.status.headlessExecutionReady == true)
                CapabilityDot(label: "UI READY", ready: store.status.uiExecutionReady == true)
                if let pending = store.status.automationPendingCount, pending > 0 {
                    MiniBadge(text: "\(pending) queued", symbol: "clock.arrow.circlepath")
                }
            }

            Divider()
            Text("Provider plane").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
            if store.providers.isEmpty {
                Text("Provider catalog unavailable")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            } else {
                HStack(spacing: 8) {
                    CapabilityDot(label: "JB", ready: providerReady("jailbreak"))
                    CapabilityDot(label: "PACKAGE", ready: providerReady("package"))
                    CapabilityDot(label: "RUNTIME", ready: providerReady("runtime"))
                    CapabilityDot(label: "UI", ready: providerReady("ui"))
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(15)
        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 18))
    }

    private func enabledCapabilityCount(risk: String) -> Int {
        store.capabilities.filter { $0.risk == risk && $0.enabled }.count
    }

    private func providerReady(_ domain: String) -> Bool {
        store.providers.contains { $0.domain == domain && $0.available }
    }

    private func formatBytes(_ bytes: UInt64) -> String {
        guard bytes > 0 else { return "—" }
        return ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .memory)
    }
}

struct DeviceHubView: View {
    @EnvironmentObject private var store: DeviceStore

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 18) {
                    HubHero(
                        title: store.status.machine == "—" ? "This iPhone" : store.status.machine,
                        subtitle: store.daemonReachable
                            ? "iOS build \(store.status.osBuild) · privileged runtime online"
                            : "Privileged runtime unavailable",
                        symbol: "iphone.gen3",
                        healthy: store.daemonReachable
                    )

                    HubSection(title: "Applications & packages", subtitle: "Install, inspect, launch and manage software") {
                        NavigationLink(value: ToolKind.apps) { HubRow(tool: .apps) }
                        Divider().padding(.leading, 54)
                        NavigationLink(value: ToolKind.packages) { HubRow(tool: .packages) }
                        Divider().padding(.leading, 54)
                        NavigationLink(value: ToolKind.processes) { HubRow(tool: .processes) }
                    }

                    HubSection(title: "System", subtitle: "Storage, network and jailbreak runtime") {
                        NavigationLink(value: ToolKind.files) { HubRow(tool: .files) }
                        Divider().padding(.leading, 54)
                        NavigationLink(value: ToolKind.network) { HubRow(tool: .network) }
                        Divider().padding(.leading, 54)
                        NavigationLink(value: ToolKind.runtime) { HubRow(tool: .runtime) }
                    }
                }
                .padding(16)
            }
            .background(Color(uiColor: .systemGroupedBackground).ignoresSafeArea())
            .navigationTitle("Device")
            .navigationDestination(for: ToolKind.self) { tool in RootToolDestination(tool: tool) }
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button { Task { await store.refresh() } } label: { Image(systemName: "arrow.clockwise") }
                }
            }
        }
    }
}

struct TasksHubView: View {
    @EnvironmentObject private var store: DeviceStore
    @State private var automation: AutomationState?
    @State private var jobs: [AutomationJob] = []
    @State private var loadError: String?

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 18) {
                    HubHero(
                        title: automation?.uiExecutionReady == true ? "Ready to execute" : "Headless runtime ready",
                        subtitle: executionSubtitle,
                        symbol: "bolt.horizontal.circle.fill",
                        healthy: automation?.headlessExecutionReady ?? store.status.headlessExecutionReady ?? false
                    )

                    if let automation {
                        HStack(spacing: 10) {
                            StatTile(title: "Pending", value: "\(automation.queue.pending)")
                            StatTile(title: "Completed", value: "\(automation.queue.completed)")
                            StatTile(title: "Failed", value: "\(automation.queue.failed)")
                        }
                    }

                    HubSection(title: "Command center", subtitle: "Typed actions, receipts and lock-aware execution") {
                        NavigationLink {
                            ActionsView()
                        } label: {
                            HubRow(title: "Run controlled action", subtitle: "App · process · file · deferred UI", symbol: "play.circle.fill")
                        }
                        Divider().padding(.leading, 54)
                        NavigationLink(value: ToolKind.audit) { HubRow(tool: .audit) }
                    }

                    if !jobs.isEmpty {
                        HubSection(title: "Recent jobs", subtitle: "Persisted device-side automation queue") {
                            ForEach(Array(jobs.prefix(5).enumerated()), id: \.element.id) { index, job in
                                if index > 0 { Divider().padding(.leading, 14) }
                                VStack(alignment: .leading, spacing: 5) {
                                    HStack {
                                        Text(job.kind).font(.subheadline.weight(.semibold))
                                        Spacer()
                                        Text(job.state.uppercased())
                                            .font(.caption2.weight(.bold))
                                            .foregroundStyle(job.state == "failed" ? Color.red : Color.secondary)
                                    }
                                    Text(job.target).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                                }
                                .padding(.vertical, 8)
                            }
                        }
                    }

                    if let loadError {
                        Text(loadError)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                .padding(16)
            }
            .background(Color(uiColor: .systemGroupedBackground).ignoresSafeArea())
            .navigationTitle("Tasks")
            .navigationDestination(for: ToolKind.self) { tool in RootToolDestination(tool: tool) }
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button { Task { await refresh() } } label: { Image(systemName: "arrow.clockwise") }
                }
            }
            .task { await refresh() }
        }
    }

    private var executionSubtitle: String {
        guard let automation else { return "Loading device execution state…" }
        if automation.uiExecutionReady {
            return "Unlocked · UI and headless actions available"
        }
        return "\(automation.lockState.capitalized) · UI jobs wait, headless jobs may continue"
    }

    @MainActor
    private func refresh() async {
        async let state = try? DaemonClient.shared.automationState()
        async let queue = try? DaemonClient.shared.automationQueue()
        let (loadedState, loadedQueue) = await (state, queue)
        automation = loadedState
        jobs = loadedQueue?.jobs ?? []
        loadError = loadedState == nil ? "Automation state is currently unavailable." : nil
    }
}

struct AgentsHubView: View {
    @EnvironmentObject private var store: DeviceStore

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 18) {
                    HubHero(
                        title: "Trusted command sources",
                        subtitle: "One policy plane for Mac, AiBox and future network skills",
                        symbol: "person.2.badge.gearshape.fill",
                        healthy: store.daemonReachable
                    )

                    HubSection(title: "Connected today", subtitle: "Authenticated callers already supported by the daemon") {
                        SourceRow(title: "RootTools", subtitle: "On-device owner interface", symbol: "iphone.gen3", state: "OWNER")
                        Divider().padding(.leading, 54)
                        SourceRow(title: "Trusted host", subtitle: "Mac / USB / Device Service client", symbol: "laptopcomputer", state: "AGENT")
                    }

                    HubSection(title: "Agent integration", subtitle: "Stable seams; callers never receive a raw root shell") {
                        SourceRow(title: "AiBox Agent", subtitle: "Local DeviceExecution adapter", symbol: "sparkles.rectangle.stack", state: "NEXT")
                        Divider().padding(.leading, 54)
                        SourceRow(title: "Network Skills", subtitle: "Outbound trusted relay + scoped capability grants", symbol: "network", state: "PLANNED")
                    }

                    HubSection(title: "Trust & credentials", subtitle: "Rotate credentials and review execution policy") {
                        NavigationLink {
                            TrustedAgentsView()
                        } label: {
                            HubRow(title: "Trusted Agents", subtitle: "Credential lifecycle and R2 trust model", symbol: "person.badge.key.fill")
                        }
                        Divider().padding(.leading, 54)
                        NavigationLink(value: ToolKind.capabilities) { HubRow(tool: .capabilities) }
                    }
                }
                .padding(16)
            }
            .background(Color(uiColor: .systemGroupedBackground).ignoresSafeArea())
            .navigationTitle("Agents")
            .navigationDestination(for: ToolKind.self) { tool in RootToolDestination(tool: tool) }
        }
    }
}

struct SettingsHubView: View {
    @EnvironmentObject private var store: DeviceStore

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 18) {
                    HubSection(title: "Execution policy", subtitle: "Permissions, providers and privileged capability truth") {
                        NavigationLink(value: ToolKind.capabilities) { HubRow(tool: .capabilities) }
                        Divider().padding(.leading, 54)
                        NavigationLink(value: ToolKind.permissions) { HubRow(tool: .permissions) }
                        Divider().padding(.leading, 54)
                        NavigationLink(value: ToolKind.providers) { HubRow(tool: .providers) }
                    }

                    HubSection(title: "Maintenance", subtitle: "Health, audit and recovery surfaces") {
                        NavigationLink(value: ToolKind.diagnostics) { HubRow(tool: .diagnostics) }
                        Divider().padding(.leading, 54)
                        NavigationLink(value: ToolKind.audit) { HubRow(tool: .audit) }
                    }

                    VStack(alignment: .leading, spacing: 10) {
                        Label(
                            store.capabilityInvariants?.rawPrivilegedShellExposed == false
                                ? "Raw privileged shell is not exposed"
                                : "Execution policy requires review",
                            systemImage: "checkmark.shield.fill"
                        )
                        .font(.caption.weight(.semibold))
                        Text("RootTools grants privileged implementation through typed capabilities, policy checks, receipts and audit. Callers do not inherit UID 0 directly.")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(16)
                    .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 18))
                }
                .padding(16)
            }
            .background(Color(uiColor: .systemGroupedBackground).ignoresSafeArea())
            .navigationTitle("Settings")
            .navigationDestination(for: ToolKind.self) { tool in RootToolDestination(tool: tool) }
        }
    }
}

private struct RootToolDestination: View {
    let tool: ToolKind

    @ViewBuilder
    var body: some View {
        if tool == .capabilities {
            CapabilitiesView()
        } else if tool == .providers {
            ProvidersView()
        } else if tool == .packages {
            PackagesView()
        } else if tool == .permissions {
            PermissionsView()
        } else if tool == .trustedAgents {
            TrustedAgentsView()
        } else {
            ToolDetailView(tool: tool)
        }
    }
}

private struct HubHero: View {
    let title: String
    let subtitle: String
    let symbol: String
    let healthy: Bool

    var body: some View {
        HStack(spacing: 15) {
            Image(systemName: symbol)
                .font(.title2)
                .frame(width: 52, height: 52)
                .background(Color.accentColor.opacity(0.16), in: RoundedRectangle(cornerRadius: 16))
            VStack(alignment: .leading, spacing: 5) {
                Text(title).font(.headline)
                Text(subtitle).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 8)
            Circle().fill(healthy ? Color.green : Color.orange).frame(width: 9, height: 9)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(18)
        .background(
            LinearGradient(
                colors: [Color.accentColor.opacity(0.16), Color(uiColor: .secondarySystemGroupedBackground)],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            ),
            in: RoundedRectangle(cornerRadius: 22)
        )
    }
}

private struct HubSection<Content: View>: View {
    let title: String
    let subtitle: String
    @ViewBuilder let content: Content

    init(title: String, subtitle: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.subtitle = subtitle
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            VStack(alignment: .leading, spacing: 4) {
                Text(title).font(.headline)
                Text(subtitle).font(.caption).foregroundStyle(.secondary)
            }
            .padding(.bottom, 10)
            content
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 20))
    }
}

private struct HubRow: View {
    let title: String
    let subtitle: String
    let symbol: String

    init(tool: ToolKind) {
        title = tool.title
        subtitle = tool.subtitle
        symbol = tool.symbol
    }

    init(title: String, subtitle: String, symbol: String) {
        self.title = title
        self.subtitle = subtitle
        self.symbol = symbol
    }

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: symbol)
                .font(.body.weight(.semibold))
                .frame(width: 40, height: 40)
                .background(Color.accentColor.opacity(0.12), in: RoundedRectangle(cornerRadius: 12))
            VStack(alignment: .leading, spacing: 3) {
                Text(title).font(.subheadline.weight(.semibold)).foregroundStyle(.primary)
                Text(subtitle).font(.caption2).foregroundStyle(.secondary).lineLimit(2)
            }
            Spacer()
            Image(systemName: "chevron.right").font(.caption.weight(.bold)).foregroundStyle(.tertiary)
        }
        .padding(.vertical, 7)
        .contentShape(Rectangle())
    }
}

private struct SourceRow: View {
    let title: String
    let subtitle: String
    let symbol: String
    let state: String

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: symbol)
                .frame(width: 40, height: 40)
                .background(Color.accentColor.opacity(0.12), in: RoundedRectangle(cornerRadius: 12))
            VStack(alignment: .leading, spacing: 3) {
                Text(title).font(.subheadline.weight(.semibold))
                Text(subtitle).font(.caption2).foregroundStyle(.secondary)
            }
            Spacer()
            Text(state)
                .font(.caption2.weight(.bold))
                .foregroundStyle(.secondary)
                .padding(.horizontal, 8)
                .padding(.vertical, 5)
                .background(Color.primary.opacity(0.06), in: Capsule())
        }
        .padding(.vertical, 7)
    }
}

private struct ToolCard: View {
    let tool: ToolKind
    let status: DeviceStatus

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Image(systemName: tool.symbol)
                .font(.title2)
                .frame(width: 38, height: 38)
                .background(Color.accentColor.opacity(0.18), in: RoundedRectangle(cornerRadius: 12))
            Text(tool.title).font(.subheadline.weight(.semibold)).foregroundStyle(.primary)
            Text(tool.subtitle).font(.caption2).foregroundStyle(.secondary).lineLimit(2)
        }
        .frame(maxWidth: .infinity, minHeight: 118, alignment: .leading)
        .padding(14)
        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 20))
        .overlay(RoundedRectangle(cornerRadius: 20).stroke(Color.white.opacity(0.05)))
    }
}

private struct StatTile: View {
    let title: String
    let value: String
    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(title).font(.caption2).foregroundStyle(.secondary)
            Text(value).font(.caption.weight(.semibold)).lineLimit(1).minimumScaleFactor(0.7)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(12)
        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 15))
    }
}

private struct StatusPill: View {
    let ok: Bool
    let text: String
    var body: some View {
        HStack(spacing: 5) {
            Circle().fill(ok ? Color.green : Color.red).frame(width: 7, height: 7)
            Text(text).font(.caption2.weight(.bold))
        }
        .padding(.horizontal, 9).padding(.vertical, 6)
        .background(.ultraThinMaterial, in: Capsule())
    }
}

private struct MiniBadge: View {
    let text: String
    let symbol: String
    var body: some View {
        Label(text, systemImage: symbol)
            .font(.caption2.weight(.semibold))
            .padding(.horizontal, 9).padding(.vertical, 6)
            .background(Color.white.opacity(0.08), in: Capsule())
    }
}

private struct CapabilityDot: View {
    let label: String
    let ready: Bool
    var body: some View {
        HStack(spacing: 5) {
            Circle().fill(ready ? Color.green : Color.orange).frame(width: 6, height: 6)
            Text(label).font(.caption2)
        }
        .padding(.horizontal, 8).padding(.vertical, 5)
        .background(Color.primary.opacity(0.06), in: Capsule())
    }
}

private struct RiskBadge: View {
    let risk: String
    let count: Int

    var body: some View {
        HStack(spacing: 5) {
            Text(risk).font(.caption2.weight(.bold))
            Text("\(count)").font(.caption2.monospacedDigit())
        }
        .padding(.horizontal, 8).padding(.vertical, 5)
        .background(Color.primary.opacity(0.06), in: Capsule())
    }
}


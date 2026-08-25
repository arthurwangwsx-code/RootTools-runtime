import SwiftUI
import UniformTypeIdentifiers

struct CapabilitiesView: View {
    @EnvironmentObject private var store: DeviceStore
    @State private var mutatingID: String?
    @State private var applyingMode = false
    @State private var pendingDeveloperMode = false
    @State private var errorMessage: String?

    private let risks = ["R0", "R1", "R2", "R3"]

    var body: some View {
        List {
            Section {
                policyModeRow(
                    mode: "restricted",
                    title: "Restricted",
                    subtitle: "Observation only. R1/R2 operations are disabled except the Owner policy recovery switch.",
                    symbol: "lock.shield.fill"
                )
                policyModeRow(
                    mode: "standard",
                    title: "Standard",
                    subtitle: "All compiled capabilities are available; R2 still needs explicit Owner confirmation.",
                    symbol: "checkmark.shield.fill"
                )
                policyModeRow(
                    mode: "developer",
                    title: "Developer",
                    subtitle: "One-tap full compiled surface for the local Owner. R2 Owner approval is automatic.",
                    symbol: "hammer.fill"
                )
                if let policy = store.policyStatus {
                    HStack(spacing: 8) {
                        badge("R0 \(policy.enabled.R0)")
                        badge("R1 \(policy.enabled.R1)")
                        badge("R2 \(policy.enabled.R2)")
                        if policy.disabledCount > 0 { badge("\(policy.disabledCount) OFF") }
                    }
                    Text(policy.mode == "custom"
                         ? "Individual capability changes moved the device into Custom mode."
                         : "Current mode: \(policy.mode.capitalized)")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            } header: {
                Text("Permission Mode")
            } footer: {
                Text("Developer Mode never enables R3/raw shell and never expands a Host, App, Skill or Automation principal's grants. It only maximizes the local Owner execution surface.")
            }

            Section {
                Label("Layer 1 · hard policy is compiled into the daemon", systemImage: "cpu")
                Label("Layer 2 · Owner mode can narrow the compiled surface", systemImage: "slider.horizontal.3")
                Label("Layer 3 · each remote principal needs an explicit R0/R1 grant", systemImage: "person.badge.key.fill")
                Label("Layer 4 · runtime lock/provider/post-condition checks still apply", systemImage: "checkmark.circle.badge.questionmark")
                Label("R3 and raw privileged shell remain hard-disabled", systemImage: "lock.shield.fill")
            } header: {
                Text("Permission Model")
            }

            ForEach(risks, id: \.self) { risk in
                Section(riskTitle(risk)) {
                    ForEach(store.capabilities.filter { $0.risk == risk }) { capability in
                        capabilityRow(capability)
                    }
                }
            }

            if let errorMessage {
                Section("Last Error") {
                    Text(errorMessage)
                        .font(.caption)
                        .foregroundStyle(.red)
                        .textSelection(.enabled)
                }
            }
        }
        .navigationTitle("Capabilities")
        .navigationBarTitleDisplayMode(.inline)
        .task {
            if store.capabilities.isEmpty { await store.refresh() }
        }
        .refreshable { await store.refresh() }
        .confirmationDialog(
            "Enable Developer Mode?",
            isPresented: $pendingDeveloperMode,
            titleVisibility: .visible
        ) {
            Button("Enable Developer Mode", role: .destructive) {
                Task { await applyMode("developer") }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("All compiled R0/R1/R2 capabilities will be enabled for the local Owner and future Owner R2 actions will not require a second confirmation. Remote principals keep their existing grants. R3/raw shell stay blocked.")
        }
    }

    @ViewBuilder
    private func policyModeRow(mode: String, title: String, subtitle: String, symbol: String) -> some View {
        let selected = store.policyStatus?.mode == mode
        Button {
            if mode == "developer" && !selected {
                pendingDeveloperMode = true
            } else if !selected {
                Task { await applyMode(mode) }
            }
        } label: {
            HStack(spacing: 12) {
                Image(systemName: symbol)
                    .frame(width: 34, height: 34)
                    .background(Color.accentColor.opacity(0.12), in: RoundedRectangle(cornerRadius: 10))
                VStack(alignment: .leading, spacing: 3) {
                    Text(title).font(.subheadline.weight(.semibold)).foregroundStyle(.primary)
                    Text(subtitle).font(.caption2).foregroundStyle(.secondary)
                }
                Spacer()
                if applyingMode && !selected {
                    ProgressView()
                } else if selected {
                    Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                }
            }
        }
        .buttonStyle(.plain)
        .disabled(applyingMode)
    }

    @ViewBuilder
    private func capabilityRow(_ capability: DeviceCapabilityDescriptor) -> some View {
        let hardEnabled = capability.hardEnabled ?? capability.enabled
        HStack(alignment: .top, spacing: 12) {
            VStack(alignment: .leading, spacing: 4) {
                Text(capability.title).font(.subheadline.weight(.semibold))
                Text(capability.id).font(.caption2.monospaced()).foregroundStyle(.secondary)
                HStack(spacing: 6) {
                    badge(capability.risk)
                    if capability.requiresConfirmation { badge("CONFIRM") }
                    if capability.reversible { badge("REVERSIBLE") }
                    if !hardEnabled { badge("HARD BLOCK") }
                }
            }
            Spacer(minLength: 8)
            if hardEnabled && capability.risk != "R3" {
                Toggle("", isOn: Binding(
                    get: { capability.enabled },
                    set: { value in Task { await set(value, capability) } }
                ))
                .labelsHidden()
                .disabled(mutatingID != nil)
            } else {
                Image(systemName: "lock.fill")
                    .foregroundStyle(.secondary)
                    .padding(.top, 4)
            }
        }
        .padding(.vertical, 3)
        .opacity(mutatingID == capability.id ? 0.55 : 1)
    }

    private func badge(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 9, weight: .bold, design: .rounded))
            .padding(.horizontal, 6)
            .padding(.vertical, 3)
            .background(Color.primary.opacity(0.07), in: Capsule())
    }

    private func riskTitle(_ risk: String) -> String {
        switch risk {
        case "R0": return "R0 · Observation"
        case "R1": return "R1 · Scoped reversible operations"
        case "R2": return "R2 · Explicit confirmation required"
        default: return "R3 · Hard denied"
        }
    }

    @MainActor
    private func set(_ enabled: Bool, _ capability: DeviceCapabilityDescriptor) async {
        guard mutatingID == nil else { return }
        mutatingID = capability.id
        errorMessage = nil
        defer { mutatingID = nil }
        do {
            try await store.setCapabilityEnabled(enabled, capabilityID: capability.id)
        } catch {
            errorMessage = error.localizedDescription
            await store.refresh()
        }
    }

    @MainActor
    private func applyMode(_ mode: String) async {
        guard !applyingMode else { return }
        applyingMode = true
        errorMessage = nil
        defer { applyingMode = false }
        do {
            try await store.setPolicyMode(mode)
        } catch {
            errorMessage = error.localizedDescription
            await store.refresh()
        }
    }
}

struct ProvidersView: View {
    @EnvironmentObject private var store: DeviceStore
    @State private var debPlan: PackageProviderPlan?
    @State private var ipaPlan: PackageProviderPlan?
    @State private var fridaStatus: FridaRuntimeStatus?
    @State private var elleKitStatus: ElleKitRuntimeStatus?
    @State private var errorMessage: String?

    private let domains = ["control", "native", "jailbreak", "package", "runtime", "transport", "ui", "permission"]

    var body: some View {
        List {
            Section("Routing invariant") {
                Label("Capabilities bind to semantic providers, never command strings", systemImage: "arrow.triangle.branch")
                Label("Provider changes do not change Agent-facing Device Ops", systemImage: "shield.lefthalf.filled")
            }

            Section("Package routing") {
                planRow("DEB", plan: debPlan)
                planRow("IPA / TIPA", plan: ipaPlan)
                Text("Package providers require owner confirmation. Raw shell and arbitrary executables remain outside the protocol.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Runtime observations") {
                if let fridaStatus {
                    VStack(alignment: .leading, spacing: 5) {
                        HStack {
                            Text("Frida").font(.subheadline.weight(.semibold))
                            Spacer()
                            Text(fridaStatus.state.uppercased()).font(.caption2.weight(.bold))
                                .foregroundStyle(fridaStatus.state == "available" ? Color.green : Color.orange)
                        }
                        Text("port \(fridaStatus.port) · process \(fridaStatus.process.running ? "pid \(fridaStatus.process.pid) uid \(fridaStatus.process.uid)" : "offline")")
                            .font(.caption2.monospaced()).foregroundStyle(.secondary)
                        if let version = fridaStatus.package.version {
                            Text("\(fridaStatus.package.id ?? "frida") · \(version)").font(.caption2).foregroundStyle(.tertiary)
                        }
                        Text("Script execution: blocked · arbitrary attach: blocked")
                            .font(.caption2).foregroundStyle(.secondary)
                    }
                }
                if let elleKitStatus {
                    VStack(alignment: .leading, spacing: 5) {
                        HStack {
                            Text("ElleKit").font(.subheadline.weight(.semibold))
                            Spacer()
                            Text(elleKitStatus.state.uppercased()).font(.caption2.weight(.bold))
                                .foregroundStyle(elleKitStatus.state == "available" ? Color.green : Color.orange)
                        }
                        HStack(spacing: 6) {
                            badge(elleKitStatus.components.library ? "LIB" : "NO LIB")
                            badge(elleKitStatus.components.loader ? "LOADER" : "NO LOADER")
                            badge(elleKitStatus.components.injector ? "INJECTOR" : "NO INJECTOR")
                            badge(elleKitStatus.components.pspawn ? "PSPAWN" : "NO PSPAWN")
                        }
                        if let version = elleKitStatus.package.version {
                            Text("\(elleKitStatus.package.id ?? "ellekit") · \(version)").font(.caption2).foregroundStyle(.tertiary)
                        }
                        Text("Raw hooks/injection are not exposed to callers")
                            .font(.caption2).foregroundStyle(.secondary)
                    }
                }
                if fridaStatus == nil || elleKitStatus == nil { ProgressView() }
            }

            ForEach(domains, id: \.self) { domain in
                let providers = store.providers.filter { $0.domain == domain }
                if !providers.isEmpty {
                    Section(domain.uppercased()) {
                        ForEach(providers) { provider in
                            VStack(alignment: .leading, spacing: 5) {
                                HStack {
                                    Text(provider.title).font(.subheadline.weight(.semibold))
                                    Spacer()
                                    Text(provider.available ? "READY" : "OFFLINE")
                                        .font(.caption2.weight(.bold))
                                        .foregroundStyle(provider.available ? Color.green : Color.orange)
                                }
                                Text(provider.id).font(.caption2.monospaced()).foregroundStyle(.secondary)
                                Text(provider.implementation).font(.caption2).foregroundStyle(.tertiary)
                                HStack(spacing: 6) {
                                    badge("P\(provider.priority)")
                                    if provider.supportsHeadless { badge("HEADLESS") }
                                    if provider.requiresUnlock { badge("UNLOCK") }
                                    if provider.survivesAppExit { badge("PERSISTENT") }
                                }
                            }
                            .padding(.vertical, 3)
                        }
                    }
                }
            }

            if let errorMessage {
                Section("Last Error") {
                    Text(errorMessage).font(.caption).foregroundStyle(.red).textSelection(.enabled)
                }
            }
        }
        .navigationTitle("Providers")
        .navigationBarTitleDisplayMode(.inline)
        .task { await refresh() }
        .refreshable { await refresh() }
    }

    @ViewBuilder
    private func planRow(_ title: String, plan: PackageProviderPlan?) -> some View {
        HStack {
            Text(title).font(.subheadline.weight(.semibold))
            Spacer()
            if let plan {
                VStack(alignment: .trailing, spacing: 2) {
                    Text(plan.selectedProviderId).font(.caption.monospaced())
                    Text(plan.ready ? "READY" : "NOT READY")
                        .font(.caption2.weight(.bold))
                        .foregroundStyle(plan.ready ? Color.green : Color.orange)
                }
            } else {
                ProgressView()
            }
        }
    }

    private func badge(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 9, weight: .bold, design: .rounded))
            .padding(.horizontal, 6).padding(.vertical, 3)
            .background(Color.primary.opacity(0.07), in: Capsule())
    }

    @MainActor
    private func refresh() async {
        do {
            let catalog = try await DaemonClient.shared.providerCatalog()
            store.providers = catalog.providers
            async let deb = DaemonClient.shared.packagePlan(format: "deb")
            async let ipa = DaemonClient.shared.packagePlan(format: "ipa")
            async let frida = DaemonClient.shared.fridaRuntimeStatus()
            async let ellekit = DaemonClient.shared.elleKitRuntimeStatus()
            debPlan = try await deb
            ipaPlan = try await ipa
            fridaStatus = try await frida
            elleKitStatus = try await ellekit
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}

private enum InstalledPackageFilter: String, CaseIterable, Identifiable {
    case all = "All"
    case essential = "Essential"

    var id: String { rawValue }
}

struct PackagesView: View {
    @State private var packages: [StagedPackageDescriptor] = []
    @State private var installedPackages: [InstalledPackageDescriptor] = []
    @State private var history: [PackageHistoryEvent] = []
    @State private var selfUpdates: [SelfUpdateDescriptor] = []
    @State private var importing = false
    @State private var running = false
    @State private var message = "Stage a DEB, IPA, or TIPA into the RootTools-owned package store."
    @State private var installedInventoryError: String?
    @State private var installedSearch = ""
    @State private var installedFilter: InstalledPackageFilter = .all
    @State private var pendingInstall: StagedPackageDescriptor?
    @State private var pendingUninstall: StagedPackageDescriptor?
    @State private var pendingRollback: StagedPackageDescriptor?
    @State private var pendingSelfUpdate: StagedPackageDescriptor?

    private var visibleInstalledPackages: [InstalledPackageDescriptor] {
        installedPackages.filter { package in
            let filterMatches = installedFilter == .all || package.essential
            guard filterMatches else { return false }
            let query = installedSearch.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !query.isEmpty else { return true }
            return package.packageId.localizedCaseInsensitiveContains(query)
                || package.description.localizedCaseInsensitiveContains(query)
                || package.section.localizedCaseInsensitiveContains(query)
                || package.version.localizedCaseInsensitiveContains(query)
        }
    }

    var body: some View {
        List {
            Section {
                Button {
                    importing = true
                } label: {
                    Label("Stage Package", systemImage: "square.and.arrow.down")
                }
                .disabled(running)

                if running { ProgressView("Processing package…") }
                Text(message)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
            } header: {
                Text("Package Controller")
            } footer: {
                Text("Files are SHA-256 verified in a RootTools-owned staging area. Install is an R2 owner action routed only to the selected fixed provider; no package path or command is caller-controlled.")
            }

            Section("Staged packages") {
                if packages.isEmpty {
                    Text("No staged packages")
                        .foregroundStyle(.secondary)
                }
                ForEach(packages) { package in
                    packageRow(package)
                }
            }

            Section {
                Picker("Installed filter", selection: $installedFilter) {
                    ForEach(InstalledPackageFilter.allCases) { filter in
                        Text(filter.rawValue).tag(filter)
                    }
                }
                .pickerStyle(.segmented)

                if let installedInventoryError {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Installed package inventory unavailable")
                            .font(.subheadline.weight(.semibold))
                        Text(installedInventoryError)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .textSelection(.enabled)
                    }
                } else if visibleInstalledPackages.isEmpty {
                    Text(installedSearch.isEmpty ? "No installed packages match this filter" : "No installed packages match your search")
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(visibleInstalledPackages) { package in
                        installedPackageRow(package)
                    }
                }
            } header: {
                HStack {
                    Text("Installed on device")
                    Spacer()
                    if installedInventoryError == nil {
                        Text("\(installedPackages.count)")
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }
                }
            } footer: {
                Text("Read-only inventory from the rootless Procursus/dpkg database. Seeing a package here does not make it RootTools-managed and does not authorize device-wide uninstall or mutation.")
            }

            if !history.isEmpty {
                Section("Recent lifecycle") {
                    ForEach(history.prefix(12)) { event in
                        VStack(alignment: .leading, spacing: 3) {
                            HStack {
                                Text(event.action.uppercased()).font(.caption2.weight(.bold))
                                Spacer()
                                Text(event.result).font(.caption2).foregroundStyle(.secondary)
                            }
                            Text(event.identifier).font(.caption.monospaced()).textSelection(.enabled)
                            Text("\(event.providerId) · #\(event.sequence)")
                                .font(.caption2).foregroundStyle(.tertiary)
                        }
                    }
                }
            }

            if !selfUpdates.isEmpty {
                Section("RootTools updates") {
                    ForEach(selfUpdates.prefix(6)) { update in
                        VStack(alignment: .leading, spacing: 3) {
                            HStack {
                                Text(update.state.uppercased()).font(.caption2.weight(.bold))
                                Spacer()
                                if !update.targetVersion.isEmpty {
                                    Text("v\(update.targetVersion)").font(.caption2.monospaced())
                                }
                            }
                            Text(update.requestId).font(.caption2.monospaced()).foregroundStyle(.secondary)
                            if let result = update.result { Text(result).font(.caption2).foregroundStyle(.secondary) }
                            if let error = update.error { Text(error).font(.caption2).foregroundStyle(.red) }
                        }
                    }
                }
            }
        }
        .navigationTitle("Packages")
        .navigationBarTitleDisplayMode(.inline)
        .searchable(text: $installedSearch, prompt: "Search installed packages")
        .task { await refresh() }
        .refreshable { await refresh() }
        .fileImporter(
            isPresented: $importing,
            allowedContentTypes: [.data],
            allowsMultipleSelection: false
        ) { result in
            switch result {
            case .success(let urls):
                guard let url = urls.first else { return }
                Task { await stage(url) }
            case .failure(let error):
                message = error.localizedDescription
            }
        }
        .confirmationDialog(
            "Install staged package?",
            isPresented: Binding(
                get: { pendingInstall != nil },
                set: { if !$0 { pendingInstall = nil } }
            ),
            titleVisibility: .visible
        ) {
            Button("Install", role: .destructive) {
                guard let package = pendingInstall else { return }
                pendingInstall = nil
                Task { await install(package) }
            }
            Button("Cancel", role: .cancel) { pendingInstall = nil }
        } message: {
            if let package = pendingInstall {
                Text("R2 owner confirmation. \(package.name) will be installed through \(package.format == "deb" ? "Procursus/dpkg" : "TrollStore") and verified after installation.")
            }
        }
        .confirmationDialog(
            "Uninstall managed package?",
            isPresented: Binding(
                get: { pendingUninstall != nil },
                set: { if !$0 { pendingUninstall = nil } }
            ),
            titleVisibility: .visible
        ) {
            Button("Uninstall", role: .destructive) {
                guard let package = pendingUninstall else { return }
                pendingUninstall = nil
                Task { await uninstall(package) }
            }
            Button("Cancel", role: .cancel) { pendingUninstall = nil }
        } message: {
            if let package = pendingUninstall {
                Text("R2 owner confirmation. Only the RootTools-managed install for \(package.expectedIdentifier) will be removed. The verified artifact is retained for reinstall.")
            }
        }
        .confirmationDialog(
            "Rollback to retained package?",
            isPresented: Binding(
                get: { pendingRollback != nil },
                set: { if !$0 { pendingRollback = nil } }
            ),
            titleVisibility: .visible
        ) {
            Button("Rollback", role: .destructive) {
                guard let package = pendingRollback else { return }
                pendingRollback = nil
                Task { await rollback(package) }
            }
            Button("Cancel", role: .cancel) { pendingRollback = nil }
        } message: {
            if let package = pendingRollback {
                Text("R2 owner confirmation. The retained, SHA-256-verified artifact \(package.name) will become active again through its fixed provider.")
            }
        }
        .confirmationDialog(
            "Update RootTools?",
            isPresented: Binding(
                get: { pendingSelfUpdate != nil },
                set: { if !$0 { pendingSelfUpdate = nil } }
            ),
            titleVisibility: .visible
        ) {
            Button("Schedule Update", role: .destructive) {
                guard let package = pendingSelfUpdate else { return }
                pendingSelfUpdate = nil
                Task { await scheduleSelfUpdate(package) }
            }
            Button("Cancel", role: .cancel) { pendingSelfUpdate = nil }
        } message: {
            if let package = pendingSelfUpdate {
                Text("R2 owner confirmation. \(package.name) will be handed to the independent updater. The serving daemon records the request first; the updater then switches binaries and rolls back if the new daemon fails health verification.")
            }
        }
    }

    @ViewBuilder
    private func packageRow(_ package: StagedPackageDescriptor) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            HStack(alignment: .firstTextBaseline) {
                Text(package.name).font(.subheadline.weight(.semibold))
                Spacer()
                Text(package.state.uppercased())
                    .font(.caption2.weight(.bold))
                    .padding(.horizontal, 7).padding(.vertical, 4)
                    .background(Color.primary.opacity(0.07), in: Capsule())
            }
            Text("\(package.format.uppercased()) · \(ByteCountFormatter.string(fromByteCount: package.totalSize, countStyle: .file))")
                .font(.caption)
                .foregroundStyle(.secondary)
            if !package.expectedIdentifier.isEmpty {
                Text(package.expectedIdentifier)
                    .font(.caption2.monospaced())
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }
            Text(package.packageId)
                .font(.caption2.monospaced())
                .foregroundStyle(.tertiary)
                .textSelection(.enabled)

            HStack {
                if package.state == "ready" && package.format == "deb" && package.expectedIdentifier == "com.arthur.roottools" {
                    Button("Update RootTools", role: .destructive) { pendingSelfUpdate = package }
                } else if package.state == "ready" || package.state == "uninstalled" {
                    Button("Install", role: .destructive) { pendingInstall = package }
                }
                if package.state == "installed" && package.expectedIdentifier != "com.arthur.roottools" {
                    Button("Uninstall", role: .destructive) { pendingUninstall = package }
                }
                if package.state == "retained" && package.expectedIdentifier != "com.arthur.roottools" {
                    Button("Rollback", role: .destructive) { pendingRollback = package }
                }
                if package.state != "installed" && package.state != "discarded" {
                    Spacer()
                    Button("Discard") { Task { await discard(package) } }
                }
            }
            .disabled(running)
        }
        .padding(.vertical, 4)
    }

    @ViewBuilder
    private func installedPackageRow(_ package: InstalledPackageDescriptor) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(alignment: .firstTextBaseline) {
                Text(package.packageId)
                    .font(.subheadline.weight(.semibold))
                    .textSelection(.enabled)
                Spacer()
                if package.essential {
                    Text("ESSENTIAL")
                        .font(.system(size: 9, weight: .bold, design: .rounded))
                        .padding(.horizontal, 6).padding(.vertical, 3)
                        .background(Color.primary.opacity(0.07), in: Capsule())
                }
            }

            HStack(spacing: 6) {
                packageBadge(package.source == "procursus-dpkg" ? "PROCURSUS · DPKG" : package.source.uppercased())
                packageBadge(package.status.uppercased())
                if !package.section.isEmpty { packageBadge(package.section.uppercased()) }
            }

            Text([package.version, package.architecture].filter { !$0.isEmpty }.joined(separator: " · "))
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)

            if package.installedSizeKB > 0 {
                Text("Installed size · \(ByteCountFormatter.string(fromByteCount: package.installedSizeKB * 1024, countStyle: .file))")
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }
            if !package.description.isEmpty {
                Text(package.description)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
        }
        .padding(.vertical, 4)
    }

    private func packageBadge(_ text: String) -> some View {
        Text(text)
            .font(.system(size: 9, weight: .bold, design: .rounded))
            .padding(.horizontal, 6).padding(.vertical, 3)
            .background(Color.primary.opacity(0.07), in: Capsule())
    }

    @MainActor
    private func refresh() async {
        do {
            async let catalog = DaemonClient.shared.packageCatalog()
            async let historyPayload = DaemonClient.shared.packageHistory()
            async let updatePayload = DaemonClient.shared.selfUpdateStatus()
            packages = try await catalog.packages
            history = (try? await historyPayload.events) ?? []
            selfUpdates = (try? await updatePayload.updates) ?? []
            do {
                let installedCatalog = try await DaemonClient.shared.installedPackageCatalog()
                installedPackages = installedCatalog.packages
                installedInventoryError = nil
            } catch {
                installedPackages = []
                installedInventoryError = error.localizedDescription
            }
        } catch {
            message = error.localizedDescription
        }
    }

    @MainActor
    private func stage(_ url: URL) async {
        guard !running else { return }
        running = true
        defer { running = false }
        do {
            let receipt = try await DaemonClient.shared.stagePackage(url: url)
            message = "READY · \(receipt.providerId ?? "roottools.execd") · \(receipt.message)"
            await refresh()
        } catch {
            message = error.localizedDescription
            await refresh()
        }
    }

    @MainActor
    private func install(_ package: StagedPackageDescriptor) async {
        guard !running else { return }
        running = true
        defer { running = false }
        do {
            let receipt = try await DaemonClient.shared.installPackage(package, confirmed: true)
            message = "\(receipt.ok ? "INSTALLED" : "FAILED") · \(receipt.providerId ?? "—") · \(receipt.message)"
            await refresh()
        } catch {
            message = error.localizedDescription
            await refresh()
        }
    }

    @MainActor
    private func rollback(_ package: StagedPackageDescriptor) async {
        guard !running else { return }
        running = true
        defer { running = false }
        do {
            let receipt = try await DaemonClient.shared.rollbackPackage(package, confirmed: true)
            message = "\(receipt.ok ? "ROLLED BACK" : "FAILED") · \(receipt.providerId ?? "—") · \(receipt.message)"
            await refresh()
        } catch {
            message = error.localizedDescription
            await refresh()
        }
    }

    @MainActor
    private func uninstall(_ package: StagedPackageDescriptor) async {
        guard !running else { return }
        running = true
        defer { running = false }
        do {
            let receipt = try await DaemonClient.shared.uninstallPackage(package, confirmed: true)
            message = "\(receipt.ok ? "UNINSTALLED" : "FAILED") · \(receipt.providerId ?? "—") · \(receipt.message)"
            await refresh()
        } catch {
            message = error.localizedDescription
            await refresh()
        }
    }

    @MainActor
    private func scheduleSelfUpdate(_ package: StagedPackageDescriptor) async {
        guard !running else { return }
        running = true
        defer { running = false }
        do {
            let receipt = try await DaemonClient.shared.scheduleSelfUpdate(package, confirmed: true)
            message = "\(receipt.ok ? "UPDATE QUEUED" : "FAILED") · \(receipt.providerId ?? "—") · \(receipt.message)"
            await refresh()
        } catch {
            message = error.localizedDescription
            await refresh()
        }
    }

    @MainActor
    private func discard(_ package: StagedPackageDescriptor) async {
        guard !running else { return }
        running = true
        defer { running = false }
        do {
            let receipt = try await DaemonClient.shared.discardPackage(package)
            message = receipt.message
            await refresh()
        } catch {
            message = error.localizedDescription
            await refresh()
        }
    }
}

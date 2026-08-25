import SwiftUI
import UniformTypeIdentifiers

struct CapabilitiesView: View {
    @EnvironmentObject private var store: DeviceStore
    @State private var mutatingID: String?
    @State private var errorMessage: String?

    private let risks = ["R0", "R1", "R2", "R3"]

    var body: some View {
        List {
            Section {
                Label("Agent credentials can execute enabled capabilities but cannot change this policy.", systemImage: "person.badge.shield.checkmark.fill")
                Label("R3 and raw privileged shell are hard-disabled in the daemon binary.", systemImage: "lock.shield.fill")
            } header: {
                Text("Owner Policy")
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
}

struct ProvidersView: View {
    @EnvironmentObject private var store: DeviceStore
    @State private var debPlan: PackageProviderPlan?
    @State private var ipaPlan: PackageProviderPlan?
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
            debPlan = try await deb
            ipaPlan = try await ipa
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}

struct PackagesView: View {
    @State private var packages: [StagedPackageDescriptor] = []
    @State private var history: [PackageHistoryEvent] = []
    @State private var selfUpdates: [SelfUpdateDescriptor] = []
    @State private var importing = false
    @State private var running = false
    @State private var message = "Stage a DEB, IPA, or TIPA into the RootTools-owned package store."
    @State private var pendingInstall: StagedPackageDescriptor?
    @State private var pendingUninstall: StagedPackageDescriptor?
    @State private var pendingRollback: StagedPackageDescriptor?
    @State private var pendingSelfUpdate: StagedPackageDescriptor?

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

    @MainActor
    private func refresh() async {
        do {
            async let catalog = DaemonClient.shared.packageCatalog()
            async let historyPayload = DaemonClient.shared.packageHistory()
            async let updatePayload = DaemonClient.shared.selfUpdateStatus()
            packages = try await catalog.packages
            history = (try? await historyPayload.events) ?? []
            selfUpdates = (try? await updatePayload.updates) ?? []
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

import SwiftUI

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

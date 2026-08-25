import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

struct TrustedAgentsView: View {
    @State private var principals: [TrustedPrincipalDescriptor] = []
    @State private var loading = false
    @State private var showCreate = false
    @State private var selectedForRevoke: TrustedPrincipalDescriptor?
    @State private var newToken: String?
    @State private var errorMessage: String?
    @State private var copied = false
    @State private var rotatingLegacy = false
    @State private var showLegacyConfirmation = false

    var body: some View {
        List {
            Section {
                HStack {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Command principals")
                            .font(.headline)
                        Text("Each Mac, app, Skill or workflow gets its own identity and revocable credential.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button {
                        showCreate = true
                    } label: {
                        Image(systemName: "plus")
                    }
                    .buttonStyle(.bordered)
                }
            }

            Section("Trusted principals") {
                if loading && principals.isEmpty {
                    HStack { Spacer(); ProgressView(); Spacer() }
                } else if principals.isEmpty {
                    VStack(spacing: 8) {
                        Image(systemName: "person.crop.circle.badge.questionmark")
                            .font(.title2)
                            .foregroundStyle(.secondary)
                        Text("No named principals")
                            .font(.subheadline.weight(.semibold))
                        Text("Add a trusted Mac, AiBox installation, Skill or automation workflow.")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 18)
                } else {
                    ForEach(principals) { principal in
                        principalRow(principal)
                    }
                }
            }

            if let newToken {
                Section("New credential — copy now") {
                    Text(newToken)
                        .font(.caption.monospaced())
                        .textSelection(.enabled)
                    Button {
                        copyToPasteboard(newToken)
                        copied = true
                    } label: {
                        Label(copied ? "Copied" : "Copy credential", systemImage: copied ? "checkmark" : "doc.on.doc")
                    }
                    Text("This plaintext credential is shown only after creation. RootTools stores only its SHA-256 hash.")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }

            Section("Approval model") {
                Label("New principals start with zero capability grants", systemImage: "lock.shield")
                Label("Only explicit R0 / R1 grants may run unattended", systemImage: "checkmark.shield")
                Label("A principal cannot self-confirm an R2 action", systemImage: "lock.shield.fill")
                Label("UID 0 and provider internals are never delegated directly", systemImage: "terminal.fill")
                Text("RootTools derives caller identity from the authenticated credential. A caller-supplied name cannot spoof audit ownership.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Legacy host credential") {
                Button(role: .destructive) {
                    showLegacyConfirmation = true
                } label: {
                    HStack {
                        Label("Rotate legacy Agent credential", systemImage: "arrow.triangle.2.circlepath")
                        Spacer()
                        if rotatingLegacy { ProgressView() }
                    }
                }
                .disabled(rotatingLegacy)

                Text("Kept for existing Mac tooling during migration. New integrations should use a named principal instead.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if let errorMessage {
                Section("Last error") {
                    Text(errorMessage)
                        .font(.caption)
                        .foregroundStyle(.red)
                        .textSelection(.enabled)
                }
            }
        }
        .navigationTitle("Trusted Agents")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                Button { Task { await load() } } label: { Image(systemName: "arrow.clockwise") }
            }
        }
        .sheet(isPresented: $showCreate) {
            PrincipalCreateView { principalID, kind, displayName in
                await create(principalID: principalID, kind: kind, displayName: displayName)
            }
        }
        .confirmationDialog(
            "Revoke this principal?",
            isPresented: Binding(
                get: { selectedForRevoke != nil },
                set: { if !$0 { selectedForRevoke = nil } }
            ),
            titleVisibility: .visible
        ) {
            if let principal = selectedForRevoke {
                Button("Revoke \(principal.displayName)", role: .destructive) {
                    Task { await revoke(principal) }
                }
            }
            Button("Cancel", role: .cancel) { selectedForRevoke = nil }
        } message: {
            Text("Its credential will stop authenticating immediately. Existing receipts and audit history are retained.")
        }
        .alert("Rotate legacy Agent credential?", isPresented: $showLegacyConfirmation) {
            Button("Cancel", role: .cancel) {}
            Button("Rotate", role: .destructive) { Task { await rotateLegacy() } }
        } message: {
            Text("Any client using the previous legacy credential will be disconnected immediately.")
        }
        .task { await load() }
        .onDisappear {
            newToken = nil
            copied = false
        }
    }

    @ViewBuilder
    private func principalRow(_ principal: TrustedPrincipalDescriptor) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 12) {
                Image(systemName: symbol(for: principal.kind))
                    .frame(width: 38, height: 38)
                    .background(Color.accentColor.opacity(0.12), in: RoundedRectangle(cornerRadius: 11))
                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 7) {
                        Text(principal.displayName).font(.subheadline.weight(.semibold))
                        Text(principal.kind.uppercased())
                            .font(.caption2.weight(.bold))
                            .foregroundStyle(.secondary)
                    }
                    Text(principal.principalId)
                        .font(.caption2.monospaced())
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                    Text(activityText(principal))
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                }
                Spacer()
                if principal.active {
                    Menu {
                        Button("Revoke", role: .destructive) { selectedForRevoke = principal }
                    } label: {
                        Image(systemName: "ellipsis.circle")
                    }
                } else {
                    Text("REVOKED")
                        .font(.caption2.weight(.bold))
                        .foregroundStyle(.secondary)
                }
            }
            if principal.active {
                NavigationLink {
                    PrincipalGrantsView(principal: principal)
                } label: {
                    Label("\(principal.grantCount) capability grants", systemImage: "key.horizontal.fill")
                        .font(.caption.weight(.medium))
                }
            }
        }
        .padding(.vertical, 4)
    }

    @MainActor
    private func load() async {
        guard !loading else { return }
        loading = true
        defer { loading = false }
        do {
            let catalog = try await DaemonClient.shared.principalCatalog()
            principals = catalog.principals
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    @MainActor
    private func create(principalID: String, kind: String, displayName: String) async -> Bool {
        do {
            let receipt = try await DaemonClient.shared.createPrincipal(
                principalID: principalID,
                kind: kind,
                displayName: displayName
            )
            guard receipt.ok, let token = receipt.output, !token.isEmpty else {
                errorMessage = receipt.message
                return false
            }
            newToken = token
            copied = false
            errorMessage = nil
            await load()
            return true
        } catch {
            errorMessage = error.localizedDescription
            return false
        }
    }

    @MainActor
    private func revoke(_ principal: TrustedPrincipalDescriptor) async {
        selectedForRevoke = nil
        do {
            let receipt = try await DaemonClient.shared.revokePrincipal(principalID: principal.principalId)
            if !receipt.ok { errorMessage = receipt.message }
            await load()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    @MainActor
    private func rotateLegacy() async {
        guard !rotatingLegacy else { return }
        rotatingLegacy = true
        defer { rotatingLegacy = false }
        do {
            let receipt = try await DaemonClient.shared.rotateAgentCredential()
            guard receipt.ok, let token = receipt.output, !token.isEmpty else {
                errorMessage = receipt.message
                return
            }
            newToken = token
            copied = false
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func symbol(for kind: String) -> String {
        switch kind {
        case "host": return "laptopcomputer"
        case "app": return "app.badge"
        case "skill": return "network"
        case "automation": return "clock.arrow.circlepath"
        default: return "person.badge.key.fill"
        }
    }

    private func activityText(_ principal: TrustedPrincipalDescriptor) -> String {
        if let last = principal.lastUsedAt {
            return "Last used \(Date(timeIntervalSince1970: TimeInterval(last)).formatted(date: .abbreviated, time: .shortened))"
        }
        return principal.active ? "Not used yet" : "Credential revoked"
    }

    private func copyToPasteboard(_ value: String) {
        #if canImport(UIKit)
        UIPasteboard.general.string = value
        #endif
    }
}

private struct PrincipalGrantsView: View {
    let principal: TrustedPrincipalDescriptor

    @State private var capabilities: [DeviceCapabilityDescriptor] = []
    @State private var grants: [PrincipalGrantDescriptor] = []
    @State private var loading = false
    @State private var mutatingCapabilityID: String?
    @State private var bulkProgress: String?
    @State private var errorMessage: String?

    var body: some View {
        List {
            Section {
                Text(principal.displayName).font(.headline)
                Text(principal.principalId)
                    .font(.caption.monospaced())
                    .foregroundStyle(.secondary)
                Text("Only R0 and R1 capabilities can be persistently delegated. R2 always requires a separate owner approval path.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Button {
                    Task { await grantAllDeveloperCapabilities() }
                } label: {
                    Label("Grant all R0 / R1", systemImage: "hammer.fill")
                }
                .disabled(mutatingCapabilityID != nil || loading)
                Button(role: .destructive) {
                    Task { await removeAllGrants() }
                } label: {
                    Label("Remove all grants", systemImage: "key.slash.fill")
                }
                .disabled(mutatingCapabilityID != nil || loading || grants.allSatisfy { !$0.active })
                if let bulkProgress {
                    Text(bulkProgress).font(.caption2.monospaced()).foregroundStyle(.secondary)
                }
            }

            Section("Read access · R0") {
                capabilityRows(risk: "R0")
            }

            Section("Operate access · R1") {
                capabilityRows(risk: "R1")
            }

            if let errorMessage {
                Section("Last error") {
                    Text(errorMessage).font(.caption).foregroundStyle(.red).textSelection(.enabled)
                }
            }
        }
        .navigationTitle("Capability Grants")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                Button { Task { await load() } } label: { Image(systemName: "arrow.clockwise") }
            }
        }
        .overlay {
            if loading && capabilities.isEmpty { ProgressView() }
        }
        .task { await load() }
    }

    @ViewBuilder
    private func capabilityRows(risk: String) -> some View {
        let rows = capabilities.filter { $0.risk == risk && ($0.hardEnabled ?? $0.enabled) }
        if rows.isEmpty {
            Text("No grantable capabilities")
                .font(.caption)
                .foregroundStyle(.secondary)
        } else {
            ForEach(rows) { capability in
                Button {
                    Task { await toggle(capability) }
                } label: {
                    HStack(spacing: 12) {
                        Image(systemName: isGranted(capability.id) ? "checkmark.circle.fill" : "circle")
                            .foregroundStyle(isGranted(capability.id) ? Color.green : Color.secondary)
                        VStack(alignment: .leading, spacing: 3) {
                            Text(capability.title).foregroundStyle(.primary)
                            Text(capability.id)
                                .font(.caption2.monospaced())
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        if mutatingCapabilityID == capability.id { ProgressView() }
                    }
                }
                .buttonStyle(.plain)
                .disabled(mutatingCapabilityID != nil)
            }
        }
    }

    private func isGranted(_ capabilityID: String) -> Bool {
        grants.contains { $0.capabilityId == capabilityID && $0.active }
    }

    @MainActor
    private func load() async {
        guard !loading else { return }
        loading = true
        defer { loading = false }
        do {
            async let catalog = DaemonClient.shared.capabilityCatalog()
            async let grantCatalog = DaemonClient.shared.principalGrants(principalID: principal.principalId)
            let resolvedCatalog = try await catalog
            let resolvedGrants = try await grantCatalog
            capabilities = resolvedCatalog.capabilities
            grants = resolvedGrants.grants
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    @MainActor
    private func toggle(_ capability: DeviceCapabilityDescriptor) async {
        guard mutatingCapabilityID == nil else { return }
        mutatingCapabilityID = capability.id
        defer { mutatingCapabilityID = nil }
        do {
            let receipt: ActionReceipt
            if isGranted(capability.id) {
                receipt = try await DaemonClient.shared.ungrantPrincipal(principalID: principal.principalId, capabilityID: capability.id)
            } else {
                receipt = try await DaemonClient.shared.grantPrincipal(principalID: principal.principalId, capabilityID: capability.id)
            }
            guard receipt.ok else {
                errorMessage = receipt.message
                return
            }
            grants = try await DaemonClient.shared.principalGrants(principalID: principal.principalId).grants
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    @MainActor
    private func grantAllDeveloperCapabilities() async {
        guard mutatingCapabilityID == nil else { return }
        let grantable = capabilities.filter {
            ($0.risk == "R0" || $0.risk == "R1") && ($0.hardEnabled ?? $0.enabled)
        }
        mutatingCapabilityID = "__bulk__"
        errorMessage = nil
        defer {
            mutatingCapabilityID = nil
            bulkProgress = nil
        }
        do {
            var completed = 0
            for capability in grantable where !isGranted(capability.id) {
                bulkProgress = "Granting \(completed + 1) / \(grantable.count)"
                let receipt = try await DaemonClient.shared.grantPrincipal(
                    principalID: principal.principalId,
                    capabilityID: capability.id
                )
                guard receipt.ok else { throw DaemonError.actionFailed(receipt.message) }
                completed += 1
            }
            grants = try await DaemonClient.shared.principalGrants(principalID: principal.principalId).grants
        } catch {
            errorMessage = error.localizedDescription
            grants = (try? await DaemonClient.shared.principalGrants(principalID: principal.principalId).grants) ?? grants
        }
    }

    @MainActor
    private func removeAllGrants() async {
        guard mutatingCapabilityID == nil else { return }
        let active = grants.filter(\.active)
        mutatingCapabilityID = "__bulk__"
        errorMessage = nil
        defer {
            mutatingCapabilityID = nil
            bulkProgress = nil
        }
        do {
            for (index, grant) in active.enumerated() {
                bulkProgress = "Removing \(index + 1) / \(active.count)"
                let receipt = try await DaemonClient.shared.ungrantPrincipal(
                    principalID: principal.principalId,
                    capabilityID: grant.capabilityId
                )
                guard receipt.ok else { throw DaemonError.actionFailed(receipt.message) }
            }
            grants = try await DaemonClient.shared.principalGrants(principalID: principal.principalId).grants
        } catch {
            errorMessage = error.localizedDescription
            grants = (try? await DaemonClient.shared.principalGrants(principalID: principal.principalId).grants) ?? grants
        }
    }
}

private struct PrincipalCreateView: View {
    @Environment(\.dismiss) private var dismiss
    @State private var displayName = ""
    @State private var principalID = ""
    @State private var kind = "host"
    @State private var submitting = false
    @State private var inlineError: String?

    let create: @MainActor (_ principalID: String, _ kind: String, _ displayName: String) async -> Bool

    var body: some View {
        NavigationStack {
            Form {
                Section("Identity") {
                    TextField("Display name", text: $displayName)
                    TextField("Principal ID", text: $principalID)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    Picker("Kind", selection: $kind) {
                        Text("Mac / Host").tag("host")
                        Text("App").tag("app")
                        Text("Network Skill").tag("skill")
                        Text("Automation").tag("automation")
                    }
                }

                Section {
                    Text("Use a stable ID such as host:macbook-pro, app:aibox-main or skill:shopping-research. The credential is generated by RootTools and shown once after creation.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                if let inlineError {
                    Section { Text(inlineError).foregroundStyle(.red).font(.caption) }
                }
            }
            .navigationTitle("Add Principal")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Create") { Task { await submit() } }
                        .disabled(submitting || normalizedDisplayName.isEmpty || !validPrincipalID)
                }
            }
            .disabled(submitting)
        }
    }

    @MainActor
    private func submit() async {
        submitting = true
        inlineError = nil
        defer { submitting = false }
        if await create(normalizedPrincipalID, kind, normalizedDisplayName) {
            dismiss()
        } else {
            inlineError = "RootTools could not create this principal. Review the error on the previous screen."
        }
    }

    private var normalizedDisplayName: String { displayName.trimmingCharacters(in: .whitespacesAndNewlines) }
    private var normalizedPrincipalID: String { principalID.trimmingCharacters(in: .whitespacesAndNewlines) }

    private var validPrincipalID: Bool {
        let value = normalizedPrincipalID
        guard !value.isEmpty, value.count <= 95, !value.contains("..") else { return false }
        return value.allSatisfy { $0.isLetter || $0.isNumber || ".-_:".contains($0) }
    }
}

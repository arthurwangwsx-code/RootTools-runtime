import SwiftUI

struct RemoteAccessView: View {
    @State private var state: RemoteAccessState?
    @State private var principals: [TrustedPrincipalDescriptor] = []
    @State private var selectedPrincipalID = ""
    @State private var durationMinutes = 60
    @State private var loading = false
    @State private var applying = false
    @State private var errorMessage: String?
    @State private var showStartConfirmation = false
    @State private var showStopConfirmation = false

    private let durations = [30, 60, 120, 240, 480]

    var body: some View {
        ScrollView {
            VStack(spacing: 16) {
                statusHero
                transportCard
                sessionCard
                securityCard

                if let errorMessage {
                    Label(errorMessage, systemImage: "exclamationmark.triangle.fill")
                        .font(.caption)
                        .foregroundStyle(.orange)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(14)
                        .background(Color.orange.opacity(0.1), in: RoundedRectangle(cornerRadius: 16))
                }
            }
            .padding(16)
        }
        .background(Color(uiColor: .systemGroupedBackground).ignoresSafeArea())
        .navigationTitle("Remote Access")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                Button { Task { await load() } } label: { Image(systemName: "arrow.clockwise") }
                    .disabled(loading || applying)
            }
        }
        .task { await load() }
        .confirmationDialog(
            "Start a private remote session?",
            isPresented: $showStartConfirmation,
            titleVisibility: .visible
        ) {
            Button("Start Remote Session") { Task { await configure(enabled: true) } }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("RootTools will listen only on the current Tailscale address and will accept only the selected Host Principal until the session expires.")
        }
        .confirmationDialog(
            "Stop remote access now?",
            isPresented: $showStopConfirmation,
            titleVisibility: .visible
        ) {
            Button("Stop Remote Session", role: .destructive) { Task { await configure(enabled: false) } }
            Button("Cancel", role: .cancel) {}
        }
    }

    private var hostPrincipals: [TrustedPrincipalDescriptor] {
        principals.filter { $0.active && $0.kind == "host" }
    }

    private var statusHero: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 14) {
                Image(systemName: state?.enabled == true ? "network.badge.shield.half.filled" : "network.slash")
                    .font(.title2)
                    .frame(width: 50, height: 50)
                    .background(Color.accentColor.opacity(0.16), in: RoundedRectangle(cornerRadius: 15))
                VStack(alignment: .leading, spacing: 4) {
                    Text(state?.enabled == true ? "Remote session is active" : "Remote access is off")
                        .font(.headline)
                    Text(heroSubtitle)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Circle()
                    .fill(state?.transport.listenerActive == true ? Color.green : Color.orange)
                    .frame(width: 10, height: 10)
            }

            if let state {
                HStack(spacing: 8) {
                    RemoteAccessBadge(text: state.transport.available ? "TAILNET READY" : "NO TAILNET", good: state.transport.available)
                    RemoteAccessBadge(text: state.transport.listenerActive ? "LISTENING" : "CLOSED", good: state.transport.listenerActive || !state.enabled)
                    RemoteAccessBadge(text: state.enabled ? "BOUNDED" : "OFF", good: true)
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(18)
        .background(
            LinearGradient(
                colors: [Color.accentColor.opacity(0.18), Color(uiColor: .secondarySystemGroupedBackground)],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            ),
            in: RoundedRectangle(cornerRadius: 22)
        )
    }

    private var heroSubtitle: String {
        guard let state else { return "Checking Tailscale and trusted Host principals" }
        if state.enabled && state.transport.listenerActive { return "Private Tailnet ingress · \(state.principalId)" }
        if state.enabled { return state.transport.listenerError ?? "Waiting for the private transport listener" }
        if state.transport.available { return "Open a bounded session when you want to hand off the phone" }
        return "Connect Tailscale before starting a remote session"
    }

    private var transportCard: some View {
        RemoteAccessSection(title: "Private transport", subtitle: "Never binds the privileged service to Wi‑Fi, cellular, or 0.0.0.0") {
            if let state {
                RemoteAccessRow(title: "Transport", value: "Tailscale")
                RemoteAccessRow(title: "Tailnet address", value: state.transport.bindAddress.isEmpty ? "Unavailable" : state.transport.bindAddress)
                RemoteAccessRow(title: "Remote port", value: "\(state.transport.port)")
                RemoteAccessRow(title: "Listener", value: state.transport.listenerActive ? "Active" : "Closed")
                if state.enabled {
                    RemoteAccessRow(title: "Expires", value: expirationText(state.expiresAt))
                }
                if let listenerError = state.transport.listenerError, !listenerError.isEmpty {
                    Text(listenerError)
                        .font(.caption2)
                        .foregroundStyle(.orange)
                }
            } else {
                ProgressView().frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }

    private var sessionCard: some View {
        RemoteAccessSection(title: "Hand-off session", subtitle: "You explicitly choose who may connect and for how long") {
            if hostPrincipals.isEmpty {
                Label("No active Host Principal is available.", systemImage: "person.crop.circle.badge.exclamationmark")
                    .font(.subheadline)
                Text("Create a trusted Host in Agents first. The credential stays separate from the Owner token and can receive only explicit R0/R1 grants.")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                NavigationLink("Manage Trusted Agents", value: ToolKind.trustedAgents)
                    .buttonStyle(.bordered)
            } else {
                Picker("Trusted Host", selection: $selectedPrincipalID) {
                    ForEach(hostPrincipals) { principal in
                        Text(principal.displayName).tag(principal.principalId)
                    }
                }

                Picker("Session duration", selection: $durationMinutes) {
                    ForEach(durations, id: \.self) { minutes in
                        Text(durationLabel(minutes)).tag(minutes)
                    }
                }

                if state?.enabled == true {
                    RemoteAccessRow(title: "Authorized Principal", value: state?.principalId ?? "—")
                    Button(role: .destructive) { showStopConfirmation = true } label: {
                        HStack {
                            Text("Stop Remote Session").fontWeight(.semibold)
                            Spacer()
                            Image(systemName: "stop.circle.fill")
                        }
                        .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.bordered)
                    .disabled(applying)
                } else {
                    Button { showStartConfirmation = true } label: {
                        HStack {
                            if applying { ProgressView().controlSize(.small) }
                            Text("Start Remote Session").fontWeight(.semibold)
                            Spacer()
                            Image(systemName: "play.circle.fill")
                        }
                        .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(applying || loading || selectedPrincipalID.isEmpty || state?.transport.available != true)
                }
            }
        }
    }

    private var securityCard: some View {
        RemoteAccessSection(title: "Security boundary", subtitle: "Remote transport never becomes remote root") {
            Label("Tailnet address only — no public WAN listener", systemImage: "checkmark.shield.fill")
            Label("Owner token is rejected on remote ingress", systemImage: "checkmark.shield.fill")
            Label("Legacy Agent token is rejected on remote ingress", systemImage: "checkmark.shield.fill")
            Label("Only the selected Named Host Principal is accepted", systemImage: "checkmark.shield.fill")
            Label("Principal grants still limit every R0/R1 capability", systemImage: "checkmark.shield.fill")
            Label("R2 and raw shell are not delegated", systemImage: "lock.shield.fill")
            Text("Closing the session, revoking the Host Principal, losing the Tailnet address, or reaching the expiry time closes the remote listener. UI tasks still obey lock state and thermal guards.")
                .font(.caption2)
                .foregroundStyle(.secondary)
        }
        .font(.caption)
    }

    @MainActor
    private func load() async {
        guard !loading else { return }
        loading = true
        defer { loading = false }
        do {
            async let remote = DaemonClient.shared.remoteAccessState()
            async let catalog = DaemonClient.shared.principalCatalog()
            let (loadedRemote, loadedCatalog) = try await (remote, catalog)
            state = loadedRemote
            principals = loadedCatalog.principals
            if loadedRemote.enabled {
                selectedPrincipalID = loadedRemote.principalId
            } else if !hostPrincipals.contains(where: { $0.principalId == selectedPrincipalID }) {
                selectedPrincipalID = hostPrincipals.first?.principalId ?? ""
            }
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    @MainActor
    private func configure(enabled: Bool) async {
        guard !applying else { return }
        applying = true
        errorMessage = nil
        defer { applying = false }
        do {
            let receipt = try await DaemonClient.shared.configureRemoteAccess(
                enabled: enabled,
                principalID: enabled ? selectedPrincipalID : "",
                durationMinutes: enabled ? durationMinutes : 0
            )
            guard receipt.ok else { throw DaemonError.actionFailed(receipt.message) }
            try? await Task.sleep(nanoseconds: 1_100_000_000)
            await load()
        } catch {
            errorMessage = error.localizedDescription
            await load()
        }
    }

    private func durationLabel(_ minutes: Int) -> String {
        if minutes < 60 { return "\(minutes) minutes" }
        return minutes == 60 ? "1 hour" : "\(minutes / 60) hours"
    }

    private func expirationText(_ epoch: Int64) -> String {
        guard epoch > 0 else { return "—" }
        return Date(timeIntervalSince1970: TimeInterval(epoch)).formatted(date: .omitted, time: .shortened)
    }
}

private struct RemoteAccessSection<Content: View>: View {
    let title: String
    let subtitle: String
    @ViewBuilder let content: Content

    init(title: String, subtitle: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.subtitle = subtitle
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            VStack(alignment: .leading, spacing: 3) {
                Text(title).font(.headline)
                Text(subtitle).font(.caption2).foregroundStyle(.secondary)
            }
            content
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 20))
    }
}

private struct RemoteAccessRow: View {
    let title: String
    let value: String

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(title).foregroundStyle(.secondary)
            Spacer()
            Text(value).multilineTextAlignment(.trailing).textSelection(.enabled)
        }
        .font(.caption)
    }
}

private struct RemoteAccessBadge: View {
    let text: String
    let good: Bool

    var body: some View {
        Text(text)
            .font(.caption2.weight(.semibold))
            .padding(.horizontal, 9)
            .padding(.vertical, 5)
            .background((good ? Color.green : Color.orange).opacity(0.14), in: Capsule())
            .foregroundStyle(good ? Color.green : Color.orange)
    }
}

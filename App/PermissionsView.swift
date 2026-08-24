import SwiftUI

struct PermissionsView: View {
    @State private var records: [TCCPermissionRecord] = []
    @State private var loading = false
    @State private var errorMessage: String?
    @State private var query = ""

    private var filtered: [TCCPermissionRecord] {
        guard !query.isEmpty else { return records }
        return records.filter {
            $0.service.localizedCaseInsensitiveContains(query) ||
            $0.client.localizedCaseInsensitiveContains(query) ||
            $0.authorizationLabel.localizedCaseInsensitiveContains(query)
        }
    }

    var body: some View {
        List {
            Section {
                Text("Read-only view of iOS TCC facts. RootTools does not write or bypass TCC here; these values are independent from RootTools capability policy.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if loading && records.isEmpty {
                Section { ProgressView() }
            } else if let errorMessage, records.isEmpty {
                Section("Unavailable") {
                    Text(errorMessage).font(.caption).foregroundStyle(.red).textSelection(.enabled)
                }
            } else {
                Section("\(filtered.count) records") {
                    ForEach(filtered) { record in
                        VStack(alignment: .leading, spacing: 5) {
                            HStack {
                                Text(shortService(record.service)).font(.subheadline.weight(.semibold))
                                Spacer()
                                Text(record.authorizationLabel)
                                    .font(.caption2.weight(.bold))
                                    .padding(.horizontal, 7).padding(.vertical, 4)
                                    .background(Color.primary.opacity(0.07), in: Capsule())
                            }
                            Text(record.client).font(.caption.monospaced()).foregroundStyle(.secondary)
                            Text("auth=\(record.authValue) reason=\(record.authReason)")
                                .font(.caption2).foregroundStyle(.tertiary)
                        }
                        .padding(.vertical, 3)
                    }
                }
            }
        }
        .searchable(text: $query, prompt: "Service or bundle ID")
        .navigationTitle("System Permissions")
        .navigationBarTitleDisplayMode(.inline)
        .task { await refresh() }
        .refreshable { await refresh() }
    }

    @MainActor
    private func refresh() async {
        guard !loading else { return }
        loading = true
        defer { loading = false }
        do {
            records = try await DaemonClient.shared.tccPermissions().records
                .sorted { lhs, rhs in
                    if lhs.service != rhs.service { return lhs.service < rhs.service }
                    return lhs.client < rhs.client
                }
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func shortService(_ service: String) -> String {
        service.replacingOccurrences(of: "kTCCService", with: "")
    }
}

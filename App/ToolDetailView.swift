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


import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

struct TrustedAgentsView: View {
    @State private var rotating = false
    @State private var showConfirmation = false
    @State private var newToken: String?
    @State private var errorMessage: String?
    @State private var copied = false

    var body: some View {
        List {
            Section("Current trust model") {
                Label("One active Agent credential", systemImage: "person.badge.key.fill")
                Label("Owner/Admin credential is separate", systemImage: "lock.shield.fill")
                Text("Agent credentials can use only enabled capabilities. They cannot change capability policy or self-confirm R2 actions.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Credential lifecycle") {
                Button(role: .destructive) {
                    showConfirmation = true
                } label: {
                    HStack {
                        Label("Rotate Agent Credential", systemImage: "arrow.triangle.2.circlepath")
                        Spacer()
                        if rotating { ProgressView() }
                    }
                }
                .disabled(rotating)

                Text("Rotation is an R2 owner action. The previous Agent token becomes invalid immediately and the new token is shown only in this screen after success.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            if let newToken {
                Section("New credential — copy now") {
                    Text(newToken)
                        .font(.caption.monospaced())
                        .textSelection(.enabled)
                    Button {
                        #if canImport(UIKit)
                        UIPasteboard.general.string = newToken
                        #endif
                        copied = true
                    } label: {
                        Label(copied ? "Copied" : "Copy Credential", systemImage: copied ? "checkmark" : "doc.on.doc")
                    }
                    Text("Leaving this page clears the displayed token. RootTools does not expose the stored Agent token through a read endpoint.")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
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
        .navigationTitle("Trusted Agents")
        .navigationBarTitleDisplayMode(.inline)
        .alert("Rotate Agent credential?", isPresented: $showConfirmation) {
            Button("Cancel", role: .cancel) {}
            Button("Rotate", role: .destructive) { Task { await rotate() } }
        } message: {
            Text("Any Agent using the previous credential will be disconnected immediately.")
        }
        .onDisappear {
            newToken = nil
            copied = false
        }
    }

    @MainActor
    private func rotate() async {
        guard !rotating else { return }
        rotating = true
        newToken = nil
        copied = false
        errorMessage = nil
        defer { rotating = false }
        do {
            let receipt = try await DaemonClient.shared.rotateAgentCredential()
            guard receipt.ok, let output = receipt.output, !output.isEmpty else {
                errorMessage = receipt.message
                return
            }
            newToken = output
        } catch {
            errorMessage = error.localizedDescription
        }
    }
}

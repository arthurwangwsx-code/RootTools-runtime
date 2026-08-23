import SwiftUI

struct ActionsView: View {
    @State private var bundleID = "com.apple.Preferences"
    @State private var processPID = ""
    @State private var fileScope: FileScope = .mobile
    @State private var fileName = "notes.txt"
    @State private var fileContent = ""
    @State private var running = false
    @State private var result = "No privileged action has been executed from this screen."
    @State private var lastAuditID: String?
    @State private var confirmProcessTermination = false

    var body: some View {
        Form {
            Section {
                TextField("Bundle identifier", text: $bundleID)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                HStack {
                    Button("Launch") { Task { await launchApp() } }
                    Spacer()
                    Button("Terminate", role: .destructive) { Task { await terminateApp() } }
                }
            } header: {
                sectionHeader("Application Control", risk: "R1")
            } footer: {
                Text("Uses fixed uiopen/uicache/killall executables. No shell string is exposed to the app.")
            }

            Section {
                TextField("PID", text: $processPID)
                    .keyboardType(.numberPad)
                Button("Send SIGTERM", role: .destructive) {
                    confirmProcessTermination = true
                }
            } header: {
                sectionHeader("Process Control", risk: "R2")
            } footer: {
                Text("UID 0 and critical processes are rejected by the daemon even if the UI requests them.")
            }

            Section {
                Picker("Scope", selection: $fileScope) {
                    ForEach(FileScope.allCases) { scope in Text(scope.title).tag(scope) }
                }
                .pickerStyle(.segmented)

                Text(fileScope.pathHint)
                    .font(.caption2.monospaced())
                    .foregroundStyle(.secondary)

                TextField("File name", text: $fileName)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()

                TextEditor(text: $fileContent)
                    .font(.system(.caption, design: .monospaced))
                    .frame(minHeight: 110)

                HStack {
                    Button("Read") { Task { await readFile() } }
                    Spacer()
                    Button("Write") { Task { await writeFile() } }
                }
            } header: {
                sectionHeader("RootTools File Scope", risk: "R0 / R1")
            } footer: {
                Text("File names cannot contain path traversal. Reads and writes are confined to two fixed RootTools directories.")
            }

            Section("Last Receipt") {
                if running { ProgressView("Executing typed action…") }
                Text(result)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                if let lastAuditID {
                    LabeledContent("Audit ID") {
                        Text(lastAuditID).font(.caption2.monospaced()).textSelection(.enabled)
                    }
                }
            }

            Section("Policy") {
                Label("R3 device-critical actions are not exposed", systemImage: "lock.shield.fill")
                Label("Arbitrary privileged shell is not exposed", systemImage: "terminal.fill")
            }
        }
        .navigationTitle("Controlled Actions")
        .navigationBarTitleDisplayMode(.inline)
        .disabled(running)
        .confirmationDialog(
            "Terminate process?",
            isPresented: $confirmProcessTermination,
            titleVisibility: .visible
        ) {
            Button("Send SIGTERM", role: .destructive) { Task { await terminateProcess() } }
            Button("Cancel", role: .cancel) { }
        } message: {
            Text("This is an R2 operation. The daemon will still reject UID 0 and critical processes.")
        }
    }

    private func sectionHeader(_ title: String, risk: String) -> some View {
        HStack {
            Text(title)
            Spacer()
            Text(risk).font(.caption2.weight(.bold))
        }
    }

    @MainActor
    private func execute(_ operation: () async throws -> ActionReceipt) async {
        guard !running else { return }
        running = true
        defer { running = false }
        do {
            let receipt = try await operation()
            lastAuditID = receipt.auditId
            result = "\(receipt.ok ? "OK" : "DENIED/FAILED") · \(receipt.action)\n\(receipt.message)"
            if let output = receipt.output { fileContent = output }
        } catch {
            lastAuditID = nil
            result = "ERROR\n\(error.localizedDescription)"
        }
    }

    @MainActor private func launchApp() async {
        let id = bundleID.trimmingCharacters(in: .whitespacesAndNewlines)
        await execute { try await DaemonClient.shared.launchApp(bundleID: id) }
    }

    @MainActor private func terminateApp() async {
        let id = bundleID.trimmingCharacters(in: .whitespacesAndNewlines)
        await execute { try await DaemonClient.shared.terminateApp(bundleID: id) }
    }

    @MainActor private func terminateProcess() async {
        guard let pid = Int(processPID) else {
            result = "ERROR\nPID must be a number."
            return
        }
        await execute { try await DaemonClient.shared.terminateProcess(pid: pid) }
    }

    @MainActor private func writeFile() async {
        let name = fileName.trimmingCharacters(in: .whitespacesAndNewlines)
        await execute { try await DaemonClient.shared.writeFile(scope: fileScope, name: name, content: fileContent) }
    }

    @MainActor private func readFile() async {
        let name = fileName.trimmingCharacters(in: .whitespacesAndNewlines)
        await execute { try await DaemonClient.shared.readFile(scope: fileScope, name: name) }
    }
}

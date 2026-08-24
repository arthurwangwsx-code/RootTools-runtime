import SwiftUI

struct ActionsView: View {
    @State private var bundleID = "com.apple.Preferences"
    @State private var processPID = ""
    @State private var fileScope: FileScope = .mobile
    @State private var fileName = "notes.txt"
    @State private var fileContent = ""
    @State private var running = false
    @State private var result = "No privileged action has been executed from this screen."
    @State private var lastRequestID: String?
    @State private var lastAuditID: String?
    @State private var confirmProcessTermination = false
    @State private var automationSummary = "Loading lock-aware automation state…"
    @State private var lastQueuedJobID: String?

    var body: some View {
        Form {
            Section {
                TextField("Bundle identifier", text: $bundleID)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                HStack {
                    Button("Launch") { Task { await launchApp() } }
                    Spacer()
                    Button("Inspect") { Task { await inspectApp() } }
                    Spacer()
                    Button("Terminate", role: .destructive) { Task { await terminateApp() } }
                }
                Button("Queue launch until unlock") { Task { await queueAppLaunch() } }
            } header: {
                sectionHeader("Application Control", risk: "R1")
            } footer: {
                Text("Immediate launch uses uiopen. Deferred launch is persisted by the root daemon and runs only after lock-state policy reports the UI is unlocked and visible.")
            }

            Section("Lock-aware Automation") {
                Text(automationSummary)
                    .font(.system(.caption, design: .monospaced))
                HStack {
                    Button("Refresh") { Task { await loadAutomationState() } }
                    if let jobID = lastQueuedJobID {
                        Spacer()
                        Button("Cancel last queued", role: .destructive) { Task { await cancelAutomation(jobID: jobID) } }
                    }
                }
            }

            Section {
                TextField("PID", text: $processPID)
                    .keyboardType(.numberPad)
                HStack {
                    Button("Inspect") { Task { await inspectProcess() } }
                    Spacer()
                    Button("Send SIGTERM", role: .destructive) {
                        confirmProcessTermination = true
                    }
                }
            } header: {
                sectionHeader("Process Control", risk: "R2")
            } footer: {
                Text("The UI confirms first, then the daemon independently requires confirmed=true and still rejects UID 0 or critical processes.")
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
                if let lastRequestID {
                    LabeledContent("Request ID") {
                        Text(lastRequestID).font(.caption2.monospaced()).textSelection(.enabled)
                    }
                }
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
        .task { await loadAutomationState() }
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
            lastRequestID = receipt.requestId
            lastAuditID = receipt.auditId
            var lines = [
                "\(receipt.ok ? "OK" : "DENIED/FAILED") · \(receipt.capabilityId ?? receipt.action)",
                "risk=\(receipt.risk ?? "—") policy=\(receipt.policy ?? "—") executed=\(receipt.executed.map(String.init) ?? "—") replayed=\(receipt.replayed.map(String.init) ?? "—") revision=\(receipt.revision.map(String.init) ?? "—") result=\(receipt.result ?? "—")",
                receipt.message
            ]
            if let post = receipt.postCondition {
                lines.append("post-condition checked=\(post.checked) passed=\(post.passed) · \(post.detail)")
            }
            result = lines.joined(separator: "\n")
            if receipt.capabilityId == "device.fs.read", let output = receipt.output { fileContent = output }
        } catch {
            lastRequestID = nil
            lastAuditID = nil
            result = "ERROR\n\(error.localizedDescription)"
        }
    }

    @MainActor private func launchApp() async {
        let id = bundleID.trimmingCharacters(in: .whitespacesAndNewlines)
        await execute { try await DaemonClient.shared.launchApp(bundleID: id) }
    }

    @MainActor private func queueAppLaunch() async {
        let id = bundleID.trimmingCharacters(in: .whitespacesAndNewlines)
        await execute { try await DaemonClient.shared.queueAppLaunch(bundleID: id) }
        if let queue = try? await DaemonClient.shared.automationQueue(),
           let pending = queue.jobs.first(where: { $0.kind == "app.launch" && $0.target == id && $0.state == "pending" }) {
            lastQueuedJobID = pending.jobId
        }
        await loadAutomationState()
    }

    @MainActor private func cancelAutomation(jobID: String) async {
        await execute { try await DaemonClient.shared.cancelAutomation(jobID: jobID) }
        lastQueuedJobID = nil
        await loadAutomationState()
    }

    @MainActor private func loadAutomationState() async {
        do {
            let state = try await DaemonClient.shared.automationState()
            automationSummary = "lock=\(state.lockState) screen=\(state.screenState)\nheadless=\(state.headlessExecutionReady) uiReady=\(state.uiExecutionReady) inputReady=\(state.interactiveInputReady)\npending=\(state.queue.pending) completed=\(state.queue.completed) failed=\(state.queue.failed)"
        } catch {
            automationSummary = "Automation state unavailable: \(error.localizedDescription)"
        }
    }

    @MainActor private func inspectApp() async {
        guard !running else { return }
        let id = bundleID.trimmingCharacters(in: .whitespacesAndNewlines)
        running = true
        defer { running = false }
        do {
            let app = try await DaemonClient.shared.inspectApp(bundleID: id)
            lastRequestID = nil
            lastAuditID = nil
            result = "APP INSPECT\nbundle=\(app.bundleID)\nexecutable=\(app.executable)\nrunning=\(app.running) critical=\(app.critical)"
        } catch {
            result = "ERROR\n\(error.localizedDescription)"
        }
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
        await execute { try await DaemonClient.shared.terminateProcess(pid: pid, confirmed: true) }
    }

    @MainActor private func inspectProcess() async {
        guard !running else { return }
        guard let pid = Int(processPID) else {
            result = "ERROR\nPID must be a number."
            return
        }
        running = true
        defer { running = false }
        do {
            let process = try await DaemonClient.shared.inspectProcess(pid: pid)
            lastRequestID = nil
            lastAuditID = nil
            result = "PROCESS INSPECT\npid=\(process.pid) uid=\(process.uid)\ncommand=\(process.command)\nprivileged=\(process.privileged) critical=\(process.critical)"
        } catch {
            result = "ERROR\n\(error.localizedDescription)"
        }
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

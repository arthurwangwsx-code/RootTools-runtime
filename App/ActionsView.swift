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
    @State private var lastProviderID: String?
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
                if let provider = lastProviderID {
                    LabeledContent("Provider") {
                        Text(provider).font(.caption2.monospaced()).textSelection(.enabled)
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
            lastProviderID = receipt.providerId
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
            lastProviderID = nil
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

struct UIAutomationView: View {
    @State private var observation: UIObservation?
    @State private var tapX = ""
    @State private var tapY = ""
    @State private var text = ""
    @State private var swipeStartX = ""
    @State private var swipeStartY = ""
    @State private var swipeEndX = ""
    @State private var swipeEndY = ""
    @State private var swipeDuration = "350"
    @State private var swipeSteps = "8"
    @State private var running = false
    @State private var lastReceipt: ActionReceipt?
    @State private var errorMessage: String?

    var body: some View {
        Form {
            Section("Live UI") {
                if let observation {
                    LabeledContent("Provider", value: observation.providerId)
                    LabeledContent("Lock", value: observation.lockState)
                    LabeledContent("Screen", value: observation.screenState)
                    LabeledContent("UI ready", value: observation.uiExecutionReady ? "Yes" : "Waiting")
                    LabeledContent(
                        "Geometry",
                        value: "\(Int(observation.screen.width)) × \(Int(observation.screen.height)) @ \(String(format: "%.2g", observation.screen.scale))"
                    )
                    LabeledContent("Orientation", value: observation.screen.orientation)
                } else {
                    Text("UI observation is unavailable until the input provider is online.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Button("Refresh observation") { Task { await refreshObservation() } }
            }

            Section("Tap") {
                HStack {
                    TextField("X", text: $tapX).keyboardType(.numberPad)
                    TextField("Y", text: $tapY).keyboardType(.numberPad)
                }
                Button("Queue tap") { Task { await submitTap() } }
                    .disabled(running)
                Text("Coordinates use physical screen pixels. If the device is locked or blanked, the task waits instead of bypassing the lock screen.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Type text") {
                TextField("Text to insert", text: $text, axis: .vertical)
                    .lineLimit(1...4)
                Button("Queue text insertion") { Task { await submitType() } }
                    .disabled(running || text.isEmpty)
                Text("Text is sent through the typed UI provider only after UI readiness. Partial/indeterminate insertion is never automatically retried.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Section("Swipe") {
                HStack {
                    TextField("Start X", text: $swipeStartX).keyboardType(.numberPad)
                    TextField("Start Y", text: $swipeStartY).keyboardType(.numberPad)
                }
                HStack {
                    TextField("End X", text: $swipeEndX).keyboardType(.numberPad)
                    TextField("End Y", text: $swipeEndY).keyboardType(.numberPad)
                }
                HStack {
                    TextField("Duration ms", text: $swipeDuration).keyboardType(.numberPad)
                    TextField("Steps", text: $swipeSteps).keyboardType(.numberPad)
                }
                Button("Queue swipe") { Task { await submitSwipe() } }
                    .disabled(running)
            }

            if let lastReceipt {
                Section("Last command") {
                    LabeledContent("Capability", value: lastReceipt.capabilityId ?? lastReceipt.action)
                    LabeledContent("Result", value: lastReceipt.result ?? "—")
                    if let taskID = lastReceipt.output {
                        LabeledContent("Task") {
                            Text(taskID).font(.caption2.monospaced()).textSelection(.enabled)
                        }
                    }
                    Text(lastReceipt.message).font(.caption).foregroundStyle(.secondary)
                }
            }

            if let errorMessage {
                Section("Last error") {
                    Text(errorMessage).font(.caption).foregroundStyle(.red).textSelection(.enabled)
                }
            }
        }
        .navigationTitle("UI Automation")
        .navigationBarTitleDisplayMode(.inline)
        .task { await refreshObservation() }
    }

    @MainActor
    private func refreshObservation() async {
        do {
            observation = try await DaemonClient.shared.uiObservation()
            errorMessage = nil
            if tapX.isEmpty, let observation { tapX = String(Int(observation.screen.width / 2)) }
            if tapY.isEmpty, let observation { tapY = String(Int(observation.screen.height / 2)) }
        } catch {
            observation = nil
            errorMessage = error.localizedDescription
        }
    }

    @MainActor
    private func run(_ operation: () async throws -> ActionReceipt) async {
        guard !running else { return }
        running = true
        defer { running = false }
        do {
            let receipt = try await operation()
            lastReceipt = receipt
            errorMessage = receipt.ok ? nil : receipt.message
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    @MainActor
    private func submitTap() async {
        guard let x = Int(tapX), let y = Int(tapY) else { errorMessage = "Tap coordinates must be integers."; return }
        await run { try await DaemonClient.shared.queueUITap(x: x, y: y) }
    }

    @MainActor
    private func submitType() async {
        await run { try await DaemonClient.shared.queueUIType(text: text) }
    }

    @MainActor
    private func submitSwipe() async {
        guard let sx = Int(swipeStartX), let sy = Int(swipeStartY), let ex = Int(swipeEndX), let ey = Int(swipeEndY),
              let duration = Int(swipeDuration), let steps = Int(swipeSteps) else {
            errorMessage = "Swipe coordinates, duration and steps must be integers."; return
        }
        await run { try await DaemonClient.shared.queueUISwipe(startX: sx, startY: sy, endX: ex, endY: ey, durationMs: duration, steps: steps) }
    }
}

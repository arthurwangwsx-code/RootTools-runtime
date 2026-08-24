import Foundation

private struct DaemonErrorPayload: Codable {
    var error: String?
}

private struct BundleActionBody: Codable {
    var capabilityId: String
    var bundleID: String
    var requestId: String
    var caller: String
    var confirmed: Bool
}

private struct ProcessActionBody: Codable {
    var capabilityId: String
    var pid: Int
    var requestId: String
    var caller: String
    var confirmed: Bool
}

private struct FileReadBody: Codable {
    var capabilityId: String
    var scope: String
    var name: String
    var requestId: String
    var caller: String
    var confirmed: Bool
}

private struct FileWriteBody: Codable {
    var capabilityId: String
    var scope: String
    var name: String
    var content: String
    var requestId: String
    var caller: String
    var confirmed: Bool
}

private struct CapabilitySetBody: Codable {
    var capabilityId: String
    var enabled: Bool
}

private struct AgentRotateBody: Codable {
    var capabilityId: String
    var requestId: String
    var caller: String
    var confirmed: Bool
}

private struct AutomationCancelBody: Codable {
    var capabilityId: String
    var jobID: String
    var requestId: String
    var caller: String
    var confirmed: Bool
}

enum DaemonError: LocalizedError {
    case invalidResponse
    case http(Int, String?)

    var errorDescription: String? {
        switch self {
        case .invalidResponse:
            return "Invalid response from privileged helper"
        case .http(let code, let message):
            if let message, !message.isEmpty { return "Privileged helper HTTP \(code): \(message)" }
            return "Privileged helper returned HTTP \(code)"
        }
    }
}

final class DaemonClient {
    static let shared = DaemonClient()
    private let baseURL = URL(string: "http://127.0.0.1:45821")!
    private let token = BuildToken.value
    private let encoder = JSONEncoder()
    private let decoder = JSONDecoder()
    private let caller = "roottools-ui"

    private func request(path: String, method: String = "GET", body: Data? = nil) async throws -> Data {
        var request = URLRequest(url: baseURL.appendingPathComponent(path.trimmingCharacters(in: CharacterSet(charactersIn: "/"))))
        request.httpMethod = method
        request.httpBody = body
        request.timeoutInterval = 4
        request.setValue(token, forHTTPHeaderField: "X-RootTools-Token")
        if body != nil { request.setValue("application/json", forHTTPHeaderField: "Content-Type") }

        let (data, response) = try await URLSession.shared.data(for: request)
        guard let http = response as? HTTPURLResponse else { throw DaemonError.invalidResponse }
        guard (200..<300).contains(http.statusCode) else {
            let payload = try? decoder.decode(DaemonErrorPayload.self, from: data)
            throw DaemonError.http(http.statusCode, payload?.error)
        }
        return data
    }

    private func post<T: Encodable, R: Decodable>(path: String, body: T, response: R.Type) async throws -> R {
        let data = try encoder.encode(body)
        let responseData = try await request(path: path, method: "POST", body: data)
        return try decoder.decode(R.self, from: responseData)
    }

    func status() async throws -> DeviceStatus {
        let data = try await request(path: "/v1/status")
        return try decoder.decode(DeviceStatus.self, from: data)
    }

    func text(path: String) async throws -> TextPayload {
        let data = try await request(path: path)
        return try decoder.decode(TextPayload.self, from: data)
    }

    func capabilityCatalog() async throws -> CapabilityCatalog {
        let data = try await request(path: "/v1/capabilities/catalog")
        return try decoder.decode(CapabilityCatalog.self, from: data)
    }

    func lockState() async throws -> DeviceLockState {
        let data = try await request(path: "/v1/device/lock-state")
        return try decoder.decode(DeviceLockState.self, from: data)
    }

    func automationState() async throws -> AutomationState {
        let data = try await request(path: "/v1/automation/state")
        return try decoder.decode(AutomationState.self, from: data)
    }

    func automationQueue() async throws -> AutomationQueuePayload {
        let data = try await request(path: "/v1/automation/queue")
        return try decoder.decode(AutomationQueuePayload.self, from: data)
    }

    func inspectApp(bundleID: String) async throws -> ApplicationInspection {
        let payload: ApplicationInspectionPayload = try await post(
            path: "/v1/inspect/app",
            body: ["bundleID": bundleID],
            response: ApplicationInspectionPayload.self
        )
        return payload.application
    }

    func inspectProcess(pid: Int) async throws -> ProcessInspection {
        struct Body: Encodable { var pid: Int }
        let payload: ProcessInspectionPayload = try await post(
            path: "/v1/inspect/process",
            body: Body(pid: pid),
            response: ProcessInspectionPayload.self
        )
        return payload.process
    }

    func tccPermissions() async throws -> TCCPermissionsPayload {
        let data = try await request(path: "/v1/permissions/tcc")
        return try decoder.decode(TCCPermissionsPayload.self, from: data)
    }

    func setCapabilityEnabled(_ enabled: Bool, capabilityID: String) async throws -> CapabilityCatalog {
        try await post(
            path: "/v1/capabilities/set",
            body: CapabilitySetBody(capabilityId: capabilityID, enabled: enabled),
            response: CapabilityCatalog.self
        )
    }

    func rotateAgentCredential() async throws -> ActionReceipt {
        try await post(
            path: "/v1/action",
            body: AgentRotateBody(
                capabilityId: "device.agent.rotate",
                requestId: UUID().uuidString,
                caller: caller,
                confirmed: true
            ),
            response: ActionReceipt.self
        )
    }

    func launchApp(bundleID: String) async throws -> ActionReceipt {
        try await post(
            path: "/v1/action",
            body: BundleActionBody(
                capabilityId: "device.app.launch", bundleID: bundleID,
                requestId: UUID().uuidString, caller: caller, confirmed: false
            ),
            response: ActionReceipt.self
        )
    }

    func queueAppLaunch(bundleID: String) async throws -> ActionReceipt {
        try await post(
            path: "/v1/action",
            body: BundleActionBody(
                capabilityId: "device.automation.queue-app-launch", bundleID: bundleID,
                requestId: UUID().uuidString, caller: caller, confirmed: false
            ),
            response: ActionReceipt.self
        )
    }

    func cancelAutomation(jobID: String) async throws -> ActionReceipt {
        try await post(
            path: "/v1/action",
            body: AutomationCancelBody(
                capabilityId: "device.automation.cancel", jobID: jobID,
                requestId: UUID().uuidString, caller: caller, confirmed: false
            ),
            response: ActionReceipt.self
        )
    }

    func terminateApp(bundleID: String) async throws -> ActionReceipt {
        try await post(
            path: "/v1/action",
            body: BundleActionBody(
                capabilityId: "device.app.terminate", bundleID: bundleID,
                requestId: UUID().uuidString, caller: caller, confirmed: false
            ),
            response: ActionReceipt.self
        )
    }

    func terminateProcess(pid: Int, confirmed: Bool) async throws -> ActionReceipt {
        try await post(
            path: "/v1/action",
            body: ProcessActionBody(
                capabilityId: "device.process.terminate", pid: pid,
                requestId: UUID().uuidString, caller: caller, confirmed: confirmed
            ),
            response: ActionReceipt.self
        )
    }

    func writeFile(scope: FileScope, name: String, content: String) async throws -> ActionReceipt {
        try await post(
            path: "/v1/action",
            body: FileWriteBody(
                capabilityId: "device.fs.write", scope: scope.rawValue, name: name, content: content,
                requestId: UUID().uuidString, caller: caller, confirmed: false
            ),
            response: ActionReceipt.self
        )
    }

    func readFile(scope: FileScope, name: String) async throws -> ActionReceipt {
        try await post(
            path: "/v1/action",
            body: FileReadBody(
                capabilityId: "device.fs.read", scope: scope.rawValue, name: name,
                requestId: UUID().uuidString, caller: caller, confirmed: false
            ),
            response: ActionReceipt.self
        )
    }
}

@MainActor
final class DeviceStore: ObservableObject {
    @Published var status: DeviceStatus = .unavailable
    @Published var capabilities: [DeviceCapabilityDescriptor] = []
    @Published var capabilityInvariants: CapabilityInvariants?
    @Published var daemonReachable = false
    @Published var lastError: String?
    @Published var lastRefresh: Date?

    func refresh() async {
        do {
            status = try await DaemonClient.shared.status()
            daemonReachable = true
            lastError = nil
            lastRefresh = Date()
            if let catalog = try? await DaemonClient.shared.capabilityCatalog() {
                capabilities = catalog.capabilities
                capabilityInvariants = catalog.invariants
            } else {
                capabilities = []
                capabilityInvariants = nil
            }
        } catch {
            daemonReachable = false
            lastError = error.localizedDescription
            capabilities = []
            capabilityInvariants = nil
        }
    }

    func setCapabilityEnabled(_ enabled: Bool, capabilityID: String) async throws {
        let catalog = try await DaemonClient.shared.setCapabilityEnabled(enabled, capabilityID: capabilityID)
        capabilities = catalog.capabilities
        capabilityInvariants = catalog.invariants
    }
}

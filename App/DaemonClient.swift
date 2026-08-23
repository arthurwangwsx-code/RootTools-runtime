import Foundation

private struct DaemonErrorPayload: Codable {
    var error: String?
}

private struct BundleActionBody: Codable {
    var bundleID: String
}

private struct ProcessActionBody: Codable {
    var pid: Int
}

private struct FileReadBody: Codable {
    var scope: String
    var name: String
}

private struct FileWriteBody: Codable {
    var scope: String
    var name: String
    var content: String
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

    func launchApp(bundleID: String) async throws -> ActionReceipt {
        try await post(path: "/v1/actions/app-launch", body: BundleActionBody(bundleID: bundleID), response: ActionReceipt.self)
    }

    func terminateApp(bundleID: String) async throws -> ActionReceipt {
        try await post(path: "/v1/actions/app-terminate", body: BundleActionBody(bundleID: bundleID), response: ActionReceipt.self)
    }

    func terminateProcess(pid: Int) async throws -> ActionReceipt {
        try await post(path: "/v1/actions/process-terminate", body: ProcessActionBody(pid: pid), response: ActionReceipt.self)
    }

    func writeFile(scope: FileScope, name: String, content: String) async throws -> ActionReceipt {
        try await post(path: "/v1/actions/file-write", body: FileWriteBody(scope: scope.rawValue, name: name, content: content), response: ActionReceipt.self)
    }

    func readFile(scope: FileScope, name: String) async throws -> ActionReceipt {
        try await post(path: "/v1/actions/file-read", body: FileReadBody(scope: scope.rawValue, name: name), response: ActionReceipt.self)
    }
}

@MainActor
final class DeviceStore: ObservableObject {
    @Published var status: DeviceStatus = .unavailable
    @Published var daemonReachable = false
    @Published var lastError: String?
    @Published var lastRefresh: Date?

    func refresh() async {
        do {
            status = try await DaemonClient.shared.status()
            daemonReachable = true
            lastError = nil
            lastRefresh = Date()
        } catch {
            daemonReachable = false
            lastError = error.localizedDescription
        }
    }
}

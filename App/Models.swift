import Foundation

struct DeviceStatus: Codable, Equatable {
    var daemonVersion: String
    var uid: Int
    var machine: String
    var osBuild: String
    var kernel: String
    var cpuCount: Int
    var memoryBytes: UInt64
    var rootFreeBytes: UInt64
    var varFreeBytes: UInt64
    var jailbreakRootless: Bool
    var dopamineRunning: Bool
    var sshReady: Bool
    var fridaReady: Bool
    var zxTouchReady: Bool

    static let unavailable = DeviceStatus(
        daemonVersion: "—", uid: -1, machine: "—", osBuild: "—", kernel: "—",
        cpuCount: 0, memoryBytes: 0, rootFreeBytes: 0, varFreeBytes: 0,
        jailbreakRootless: false, dopamineRunning: false, sshReady: false,
        fridaReady: false, zxTouchReady: false
    )
}

struct TextPayload: Codable {
    var ok: Bool
    var output: String
}

struct ActionReceipt: Codable {
    var ok: Bool
    var action: String
    var message: String
    var auditId: String
    var output: String?
}

enum FileScope: String, CaseIterable, Identifiable {
    case mobile
    case bootstrap

    var id: String { rawValue }

    var title: String {
        switch self {
        case .mobile: return "Mobile"
        case .bootstrap: return "Bootstrap"
        }
    }

    var pathHint: String {
        switch self {
        case .mobile: return "/var/mobile/Library/RootTools/files"
        case .bootstrap: return "/var/jb/etc/roottools"
        }
    }
}

enum ToolKind: String, CaseIterable, Identifiable {
    case runtime
    case apps
    case processes
    case files
    case network
    case diagnostics
    case capabilities
    case audit

    var id: String { rawValue }

    var title: String {
        switch self {
        case .runtime: return "Jailbreak Runtime"
        case .apps: return "Applications"
        case .processes: return "Processes"
        case .files: return "Root Files"
        case .network: return "Network"
        case .diagnostics: return "Diagnostics"
        case .capabilities: return "Capabilities"
        case .audit: return "Audit Log"
        }
    }

    var subtitle: String {
        switch self {
        case .runtime: return "Dopamine · rootless · helper"
        case .apps: return "Installed application inventory"
        case .processes: return "PID · UID · executable"
        case .files: return "Bootstrap and mobile paths"
        case .network: return "Interfaces · routes · listeners"
        case .diagnostics: return "One-tap privileged snapshot"
        case .capabilities: return "R0 · R1 · R2 · R3 policy"
        case .audit: return "Privileged action receipts"
        }
    }

    var symbol: String {
        switch self {
        case .runtime: return "lock.open.fill"
        case .apps: return "square.grid.2x2.fill"
        case .processes: return "waveform.path.ecg.rectangle.fill"
        case .files: return "folder.fill.badge.gearshape"
        case .network: return "network"
        case .diagnostics: return "stethoscope"
        case .capabilities: return "checkmark.shield.fill"
        case .audit: return "list.bullet.rectangle.portrait.fill"
        }
    }

    var endpoint: String {
        switch self {
        case .runtime: return "/v1/runtime"
        case .apps: return "/v1/apps"
        case .processes: return "/v1/processes"
        case .files: return "/v1/files"
        case .network: return "/v1/network"
        case .diagnostics: return "/v1/diagnostics"
        case .capabilities: return "/v1/capabilities"
        case .audit: return "/v1/audit"
        }
    }
}


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
    var lockState: String?
    var deviceLocked: Bool?
    var screenState: String?
    var screenBlanked: Bool?
    var headlessExecutionReady: Bool?
    var uiExecutionReady: Bool?
    var automationPendingCount: Int?

    static let unavailable = DeviceStatus(
        daemonVersion: "—", uid: -1, machine: "—", osBuild: "—", kernel: "—",
        cpuCount: 0, memoryBytes: 0, rootFreeBytes: 0, varFreeBytes: 0,
        jailbreakRootless: false, dopamineRunning: false, sshReady: false,
        fridaReady: false, zxTouchReady: false,
        lockState: nil, deviceLocked: nil, screenState: nil, screenBlanked: nil,
        headlessExecutionReady: nil, uiExecutionReady: nil, automationPendingCount: nil
    )
}

struct DeviceLockState: Codable, Equatable {
    var schemaVersion: Int
    var lockState: String
    var locked: Bool?
    var lockRawState: UInt64
    var lockSource: String
    var screenState: String
    var screenBlanked: Bool?
    var screenRawState: UInt64
    var screenSource: String
    var headlessExecutionReady: Bool
    var uiExecutionReady: Bool
}

struct AutomationQueueSummary: Codable, Equatable {
    var pending: Int
    var completed: Int
    var failed: Int
}

struct AutomationAdapters: Codable, Equatable {
    var ssh: Bool
    var frida: Bool
    var zxtouch: Bool
}

struct AutomationPolicy: Codable, Equatable {
    var bypassDevicePasscode: Bool
    var uiJobsWaitForUnlock: Bool
    var headlessJobsMayRunLocked: Bool
}

struct AutomationState: Codable, Equatable {
    var schemaVersion: Int
    var mode: String
    var lockState: String
    var screenState: String
    var headlessExecutionReady: Bool
    var uiExecutionReady: Bool
    var interactiveInputReady: Bool
    var adapters: AutomationAdapters
    var queue: AutomationQueueSummary
    var policy: AutomationPolicy
}

struct AutomationJob: Codable, Identifiable, Equatable {
    var jobId: String
    var kind: String
    var target: String
    var state: String
    var attemptCount: Int
    var createdAt: Int64
    var updatedAt: Int64
    var result: String?
    var error: String?

    var id: String { jobId }
}

struct AutomationQueuePayload: Codable, Equatable {
    var schemaVersion: Int
    var jobs: [AutomationJob]
    var count: Int
}

struct DeviceTaskDescriptor: Codable, Identifiable, Equatable {
    var taskId: String
    var capabilityId: String
    var kind: String
    var target: String
    var caller: String
    var state: String
    var requiresUI: Bool
    var attemptCount: Int
    var createdAt: Int64
    var updatedAt: Int64
    var result: String?
    var error: String?

    var id: String { taskId }
    var cancellable: Bool { ["queued", "waiting_for_unlock", "retrying"].contains(state) }
}

struct DeviceTaskCatalog: Codable, Equatable {
    var schemaVersion: Int
    var tasks: [DeviceTaskDescriptor]
    var count: Int
}

struct UIObservationScreen: Codable, Equatable {
    var width: Double
    var height: Double
    var scale: Double
    var orientation: String
}

struct UIObservation: Codable, Equatable {
    var schemaVersion: Int
    var providerId: String
    var lockState: String
    var screenState: String
    var uiExecutionReady: Bool
    var screen: UIObservationScreen
}

struct TextPayload: Codable {
    var ok: Bool
    var output: String
}

struct DeviceCapabilityDescriptor: Codable, Identifiable, Equatable {
    var id: String
    var legacyAction: String?
    var title: String
    var risk: String
    var requiresConfirmation: Bool
    var reversible: Bool
    var hardEnabled: Bool?
    var enabled: Bool
}

struct CapabilityInvariants: Codable, Equatable {
    var r3Exposed: Bool
    var rawPrivilegedShellExposed: Bool
}

struct CapabilityCatalog: Codable, Equatable {
    var schemaVersion: Int
    var capabilities: [DeviceCapabilityDescriptor]
    var invariants: CapabilityInvariants
}

struct PermissionPolicyEnabledCounts: Codable, Equatable {
    var R0: Int
    var R1: Int
    var R2: Int
}

struct PermissionPolicyStatus: Codable, Equatable {
    var schemaVersion: Int
    var mode: String
    var developerMode: Bool
    var ownerR2AutoApproval: Bool
    var principalR2PersistentGrants: Bool
    var r3HardBlocked: Bool
    var rawPrivilegedShellExposed: Bool
    var enabled: PermissionPolicyEnabledCounts
    var disabledCount: Int
}

struct PerformanceLoadAverage: Codable, Equatable {
    var available: Bool
    var oneMinute: Double
    var fiveMinute: Double
    var fifteenMinute: Double
}

struct PerformanceMemorySnapshot: Codable, Equatable {
    var available: Bool
    var totalBytes: UInt64
    var freeBytes: UInt64
    var activeBytes: UInt64
    var inactiveBytes: UInt64
    var wiredBytes: UInt64
}

struct PerformanceStorageSnapshot: Codable, Equatable {
    var rootFreeBytes: UInt64
    var varFreeBytes: UInt64
}

struct PerformanceDaemonSnapshot: Codable, Equatable {
    var pid: Int
    var residentBytes: UInt64
}

struct PerformanceProviderSnapshot: Codable, Equatable {
    var ready: Int
    var total: Int
}

struct DevicePerformanceSnapshot: Codable, Equatable {
    var schemaVersion: Int
    var uptimeSeconds: UInt64
    var cpuCount: Int
    var loadAverage: PerformanceLoadAverage
    var memory: PerformanceMemorySnapshot
    var storage: PerformanceStorageSnapshot
    var daemon: PerformanceDaemonSnapshot
    var processCount: Int
    var activeTaskCount: Int
    var providers: PerformanceProviderSnapshot
}

struct RemoteWorkerDisplayState: Codable, Equatable {
    var targetBrightnessPercent: Int
    var strategy: String
    var awakeAssertionSupported: Bool
    var awakeAssertionActive: Bool
}

struct RemoteWorkerBatteryState: Codable, Equatable {
    var available: Bool
    var percent: Int
    var temperatureCentiC: Int
    var temperatureAvailable: Bool
    var externalPowerConnected: Bool
    var charging: Bool
    var instantAmperageMa: Int
    var cycleCount: Int
    var fullChargeCapacityMah: Int
    var designCapacityMah: Int
    var healthPercent: Int
}

struct RemoteWorkerPowerState: Codable, Equatable {
    var systemLoadAvailable: Bool
    var systemLoadMilliwatts: Int
}

struct RemoteWorkerThermalState: Codable, Equatable {
    var paused: Bool
    var pauseCentiC: Int
    var resumeCentiC: Int
}

struct RemoteWorkerChargeGuardState: Codable, Equatable {
    var enabled: Bool
    var available: Bool
    var verified: Bool
    var inhibited: Bool
    var state: String
    var floorPercent: Int
    var ceilingPercent: Int
}

struct RemoteWorkerPolicyState: Codable, Equatable {
    var bypassPasscode: Bool
    var thermalMayReleaseDisplayAssertion: Bool
    var brightnessManagedByClient: Bool
}

struct RemoteWorkerState: Codable, Equatable {
    var schemaVersion: Int
    var enabled: Bool
    var mode: String
    var display: RemoteWorkerDisplayState
    var battery: RemoteWorkerBatteryState
    var power: RemoteWorkerPowerState
    var thermal: RemoteWorkerThermalState
    var chargeGuard: RemoteWorkerChargeGuardState
    var policy: RemoteWorkerPolicyState
}

struct ProviderDescriptor: Codable, Identifiable, Equatable {
    var id: String
    var title: String
    var domain: String
    var implementation: String
    var priority: Int
    var state: String
    var supportsHeadless: Bool
    var requiresUnlock: Bool
    var survivesAppExit: Bool

    var available: Bool { state == "available" }
}

struct ProviderBinding: Codable, Identifiable, Equatable {
    var capabilityId: String
    var providerId: String
    var providerAvailable: Bool
    var id: String { capabilityId }
}

struct ProviderCatalog: Codable, Equatable {
    var schemaVersion: Int
    var providers: [ProviderDescriptor]
    var bindings: [ProviderBinding]
}

struct RuntimePackageFact: Codable, Equatable {
    var known: Bool
    var id: String?
    var version: String?
}

struct FridaProcessFact: Codable, Equatable {
    var running: Bool
    var pid: Int
    var uid: Int
    var command: String
}

struct FridaRuntimePolicy: Codable, Equatable {
    var headlessObservation: Bool
    var scriptExecutionExposed: Bool
    var arbitraryAttachExposed: Bool
}

struct FridaRuntimeStatus: Codable, Equatable {
    var schemaVersion: Int
    var providerId: String
    var state: String
    var port: Int
    var protocolReachable: Bool
    var serverPath: String?
    var process: FridaProcessFact
    var package: RuntimePackageFact
    var policy: FridaRuntimePolicy
}

struct ElleKitComponents: Codable, Equatable {
    var library: Bool
    var loader: Bool
    var injector: Bool
    var pspawn: Bool
    var safeMode: Bool
    var tweakInjectDirectory: Bool
}

struct ElleKitRuntimePolicy: Codable, Equatable {
    var headlessObservation: Bool
    var rawHookAPIExposed: Bool
    var arbitraryInjectionExposed: Bool
}

struct ElleKitRuntimeStatus: Codable, Equatable {
    var schemaVersion: Int
    var providerId: String
    var state: String
    var components: ElleKitComponents
    var package: RuntimePackageFact
    var policy: ElleKitRuntimePolicy
}

struct PackageProviderPolicy: Codable, Equatable {
    var rawShell: Bool
    var arbitraryExecutable: Bool
    var typedPackageOnly: Bool
}

struct PackageProviderPlan: Codable, Equatable {
    var schemaVersion: Int
    var format: String
    var mode: String
    var selectedProviderId: String
    var ready: Bool
    var requiresOwnerConfirmation: Bool
    var fallbackProviderId: String?
    var fallbackReady: Bool
    var policy: PackageProviderPolicy
}

struct StagedPackageDescriptor: Codable, Identifiable, Equatable {
    var packageId: String
    var name: String
    var format: String
    var expectedIdentifier: String
    var totalSize: Int64
    var receivedSize: Int64
    var sha256: String
    var state: String

    var id: String { packageId }
}

struct StagedPackageCatalog: Codable, Equatable {
    var schemaVersion: Int
    var packages: [StagedPackageDescriptor]
    var count: Int
}

struct InstalledPackageDescriptor: Codable, Identifiable, Equatable {
    var packageId: String
    var version: String
    var architecture: String
    var source: String
    var status: String
    var section: String
    var priority: String
    var description: String
    var installedSizeKB: Int64
    var essential: Bool

    var id: String { packageId }
}

struct InstalledPackageCatalog: Codable, Equatable {
    var schemaVersion: Int
    var packages: [InstalledPackageDescriptor]
    var count: Int
}

struct PackageHistoryEvent: Codable, Identifiable, Equatable {
    var sequence: Int64
    var packageId: String
    var identifier: String
    var format: String
    var action: String
    var providerId: String
    var result: String
    var occurredAt: Int64

    var id: Int64 { sequence }
}

struct PackageHistoryPayload: Codable, Equatable {
    var schemaVersion: Int
    var events: [PackageHistoryEvent]
    var count: Int
}

struct SelfUpdateDescriptor: Codable, Identifiable, Equatable {
    var requestId: String
    var packageId: String
    var state: String
    var targetVersion: String
    var result: String?
    var error: String?
    var createdAt: Int64
    var updatedAt: Int64

    var id: String { requestId }
}

struct SelfUpdateStatusPayload: Codable, Equatable {
    var schemaVersion: Int
    var updates: [SelfUpdateDescriptor]
    var count: Int
}

struct TrustedPrincipalDescriptor: Codable, Identifiable, Equatable {
    var principalId: String
    var kind: String
    var displayName: String
    var state: String
    var createdAt: Int64
    var lastUsedAt: Int64?
    var revokedAt: Int64?
    var grantCount: Int

    var id: String { principalId }
    var active: Bool { state == "active" }
}

struct TrustedPrincipalCatalog: Codable, Equatable {
    var schemaVersion: Int
    var principals: [TrustedPrincipalDescriptor]
    var count: Int
}

struct PrincipalGrantDescriptor: Codable, Identifiable, Equatable {
    var capabilityId: String
    var createdAt: Int64
    var expiresAt: Int64?
    var active: Bool

    var id: String { capabilityId }
}

struct PrincipalGrantCatalog: Codable, Equatable {
    var schemaVersion: Int
    var principalId: String
    var grants: [PrincipalGrantDescriptor]
    var count: Int
}

struct ApplicationDescriptor: Codable, Identifiable, Equatable {
    var bundleID: String
    var executable: String
    var displayName: String
    var version: String
    var build: String
    var source: String
    var path: String
    var running: Bool
    var critical: Bool

    var id: String { bundleID }
}

struct ApplicationCatalog: Codable, Equatable {
    var schemaVersion: Int
    var generation: Int
    var applications: [ApplicationDescriptor]
    var count: Int
}

struct ApplicationInspection: Codable, Equatable {
    var bundleID: String
    var executable: String
    var displayName: String?
    var version: String?
    var build: String?
    var source: String?
    var bundlePath: String?
    var running: Bool
    var critical: Bool
}

struct ApplicationInspectionPayload: Codable, Equatable {
    var ok: Bool
    var application: ApplicationInspection
}

struct ProcessDescriptor: Codable, Identifiable, Equatable {
    var pid: Int
    var uid: Int
    var command: String
    var critical: Bool
    var privileged: Bool

    var id: Int { pid }
}

struct ProcessCatalog: Codable, Equatable {
    var schemaVersion: Int
    var generation: Int
    var processes: [ProcessDescriptor]
    var count: Int
}

struct ProcessResourceMetrics: Codable, Equatable {
    var userTimeNs: UInt64
    var systemTimeNs: UInt64
    var residentBytes: UInt64
    var footprintBytes: UInt64
    var diskReadBytes: UInt64
    var diskWriteBytes: UInt64
    var pageins: UInt64
    var idleWakeups: UInt64
    var interruptWakeups: UInt64
}

struct ProcessInspection: Codable, Equatable {
    var pid: Int
    var uid: Int
    var command: String
    var critical: Bool
    var privileged: Bool
    var metricsAvailable: Bool?
    var metrics: ProcessResourceMetrics?
}

struct ProcessInspectionPayload: Codable, Equatable {
    var ok: Bool
    var process: ProcessInspection
}

struct TCCPermissionRecord: Codable, Identifiable, Equatable {
    var service: String
    var client: String
    var authValue: Int
    var authReason: Int
    var lastModified: Int64

    var id: String { "\(service)|\(client)" }

    var authorizationLabel: String {
        switch authValue {
        case 0: return "Denied"
        case 1: return "Unknown"
        case 2: return "Allowed"
        case 3: return "Limited"
        default: return "Value \(authValue)"
        }
    }
}

struct TCCPermissionsPayload: Codable, Equatable {
    var schemaVersion: Int
    var records: [TCCPermissionRecord]
    var count: Int
}

struct ActionPostCondition: Codable {
    var checked: Bool
    var passed: Bool
    var detail: String
}

struct ActionReceipt: Codable {
    var ok: Bool
    var executed: Bool?
    var replayed: Bool?
    var revision: UInt64?
    var requestId: String?
    var capabilityId: String?
    var providerId: String?
    var action: String
    var risk: String?
    var caller: String?
    var target: String?
    var result: String?
    var policy: String?
    var message: String
    var auditId: String
    var postCondition: ActionPostCondition?
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

struct FileScopeDescriptor: Codable, Identifiable, Equatable {
    var id: String
    var root: String
    var read: Bool
    var write: Bool
    var maxReadBytes: Int
    var maxWriteBytes: Int
}

struct FileScopeCatalog: Codable, Equatable {
    var schemaVersion: Int
    var scopes: [FileScopeDescriptor]
}

struct FileEntryDescriptor: Codable, Identifiable, Equatable {
    var name: String
    var kind: String
    var size: UInt64
    var mode: UInt32
    var modifiedAt: Int64

    var id: String { name }
    var isDirectory: Bool { kind == "directory" }
    var isSymlink: Bool { kind == "symlink" }
}

struct FileListPayload: Codable, Equatable {
    var schemaVersion: Int
    var scope: String
    var path: String
    var entries: [FileEntryDescriptor]
    var count: Int
}

enum ToolKind: String, CaseIterable, Identifiable {
    case performance
    case runtime
    case uiAutomation
    case remoteWorker
    case providers
    case packages
    case apps
    case processes
    case files
    case network
    case permissions
    case trustedAgents
    case diagnostics
    case capabilities
    case audit

    var id: String { rawValue }

    var title: String {
        switch self {
        case .performance: return "Performance"
        case .runtime: return "Jailbreak Runtime"
        case .uiAutomation: return "UI Automation"
        case .remoteWorker: return "Remote Worker"
        case .providers: return "Providers"
        case .packages: return "Packages"
        case .apps: return "Applications"
        case .processes: return "Processes"
        case .files: return "Root Files"
        case .network: return "Network"
        case .permissions: return "Permissions"
        case .trustedAgents: return "Trusted Agents"
        case .diagnostics: return "Diagnostics"
        case .capabilities: return "Capabilities"
        case .audit: return "Audit Log"
        }
    }

    var subtitle: String {
        switch self {
        case .performance: return "Load · memory · uptime · daemon resources"
        case .runtime: return "Dopamine · rootless · helper"
        case .uiAutomation: return "Observe · tap · type · swipe · lock-aware"
        case .remoteWorker: return "Always-on UI · low brightness · battery guard"
        case .providers: return "Dopamine · TrollStore · Frida · UI"
        case .packages: return "DEB · IPA · TIPA staging and install"
        case .apps: return "Installed application inventory"
        case .processes: return "PID · UID · executable"
        case .files: return "Bootstrap and mobile paths"
        case .network: return "Interfaces · routes · listeners"
        case .permissions: return "iOS TCC authorization facts"
        case .trustedAgents: return "Agent credential · revoke by rotation"
        case .diagnostics: return "One-tap privileged snapshot"
        case .capabilities: return "R0 · R1 · R2 · R3 policy"
        case .audit: return "Privileged action receipts"
        }
    }

    var symbol: String {
        switch self {
        case .performance: return "gauge.with.dots.needle.67percent"
        case .runtime: return "lock.open.fill"
        case .uiAutomation: return "hand.tap.fill"
        case .remoteWorker: return "iphone.and.arrow.forward"
        case .providers: return "point.3.connected.trianglepath.dotted"
        case .packages: return "shippingbox.fill"
        case .apps: return "square.grid.2x2.fill"
        case .processes: return "waveform.path.ecg.rectangle.fill"
        case .files: return "folder.fill.badge.gearshape"
        case .network: return "network"
        case .permissions: return "hand.raised.fill"
        case .trustedAgents: return "person.badge.key.fill"
        case .diagnostics: return "stethoscope"
        case .capabilities: return "checkmark.shield.fill"
        case .audit: return "list.bullet.rectangle.portrait.fill"
        }
    }

    var endpoint: String {
        switch self {
        case .performance: return "/v1/performance"
        case .runtime: return "/v1/runtime"
        case .uiAutomation: return "/v1/ui/observe"
        case .remoteWorker: return "/v1/remote-worker"
        case .providers: return "/v1/providers/catalog"
        case .packages: return "/v1/packages/catalog"
        case .apps: return "/v1/apps"
        case .processes: return "/v1/processes"
        case .files: return "/v1/files"
        case .network: return "/v1/network"
        case .permissions: return "/v1/permissions/tcc"
        case .trustedAgents: return "/v1/hello"
        case .diagnostics: return "/v1/diagnostics"
        case .capabilities: return "/v1/capabilities"
        case .audit: return "/v1/audit"
        }
    }
}


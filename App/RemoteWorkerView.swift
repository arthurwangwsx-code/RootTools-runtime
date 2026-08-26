import SwiftUI
import UIKit

struct RemoteWorkerView: View {
    @State private var state: RemoteWorkerState?
    @State private var lockState: DeviceLockState?
    @State private var loading = false
    @State private var applying = false
    @State private var errorMessage: String?
    @State private var showApplyConfirmation = false

    @State private var enabled = false
    @State private var dimPercent = 1
    @State private var chargeFloorPercent = 70
    @State private var chargeCeilingPercent = 80
    @State private var thermalPauseCentiC = 4000
    @State private var thermalResumeCentiC = 3700
    @State private var chargeControlEnabled = false

    @AppStorage("roottools.remote-worker.saved-brightness") private var savedBrightness = 0.35
    @AppStorage("roottools.remote-worker.has-saved-brightness") private var hasSavedBrightness = false

    var body: some View {
        ScrollView {
            VStack(spacing: 16) {
                statusHero
                liveMetrics
                configurationCard
                safetyCard

                if let errorMessage {
                    Label(errorMessage, systemImage: "exclamationmark.triangle.fill")
                        .font(.caption)
                        .foregroundStyle(.orange)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(14)
                        .background(Color.orange.opacity(0.1), in: RoundedRectangle(cornerRadius: 16))
                }
            }
            .padding(16)
        }
        .background(Color(uiColor: .systemGroupedBackground).ignoresSafeArea())
        .navigationTitle("Remote Worker")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                Button { Task { await load() } } label: { Image(systemName: "arrow.clockwise") }
                    .disabled(loading || applying)
            }
        }
        .task { await load() }
        .confirmationDialog(
            enabled ? "Apply Remote Worker settings?" : "Disable Remote Worker Mode?",
            isPresented: $showApplyConfirmation,
            titleVisibility: .visible
        ) {
            Button(enabled ? "Apply & Enable" : "Disable", role: enabled ? nil : .destructive) {
                Task { await applyConfiguration() }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text(enabled
                ? "The daemon will prevent idle display sleep while healthy. The physical OLED brightness will be reduced to the selected level."
                : "The always-on assertion and RootTools-owned charge inhibit will be released, and display brightness will be restored.")
        }
    }

    private var statusHero: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 14) {
                Image(systemName: state?.thermal.paused == true ? "thermometer.high" : "iphone.and.arrow.forward")
                    .font(.title2)
                    .frame(width: 50, height: 50)
                    .background(Color.accentColor.opacity(0.16), in: RoundedRectangle(cornerRadius: 15))
                VStack(alignment: .leading, spacing: 4) {
                    Text(heroTitle).font(.headline)
                    Text(heroSubtitle).font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                Circle()
                    .fill(heroHealthy ? Color.green : Color.orange)
                    .frame(width: 10, height: 10)
            }
            if let state {
                HStack(spacing: 8) {
                    RemoteWorkerBadge(text: state.display.awakeAssertionActive ? "AWAKE" : "NO ASSERTION", good: state.display.awakeAssertionActive)
                    RemoteWorkerBadge(text: lockState?.locked == false ? "UNLOCKED" : "LOCKED", good: lockState?.locked == false)
                    RemoteWorkerBadge(text: state.thermal.paused ? "THERMAL PAUSE" : "THERMAL OK", good: !state.thermal.paused)
                }
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(18)
        .background(
            LinearGradient(
                colors: [Color.accentColor.opacity(0.18), Color(uiColor: .secondarySystemGroupedBackground)],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            ),
            in: RoundedRectangle(cornerRadius: 22)
        )
    }

    @ViewBuilder
    private var liveMetrics: some View {
        if let state {
            HStack(spacing: 10) {
                RemoteWorkerMetric(title: "Battery", value: state.battery.available ? "\(state.battery.percent)%" : "—")
                RemoteWorkerMetric(title: "Temperature", value: state.battery.temperatureAvailable ? temperature(state.battery.temperatureCentiC) : "—")
                RemoteWorkerMetric(title: "System Load", value: state.power.systemLoadAvailable ? String(format: "%.2f W", Double(state.power.systemLoadMilliwatts) / 1000) : "—")
            }
        }
    }

    private var configurationCard: some View {
        RemoteWorkerSection(title: "Worker policy", subtitle: "Keep UI execution ready while minimizing OLED and battery stress") {
            Toggle("Remote Worker Mode", isOn: $enabled)

            Divider()
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text("Worker brightness")
                    Spacer()
                    Text("\(dimPercent)%").foregroundStyle(.secondary).monospacedDigit()
                }
                Slider(value: Binding(
                    get: { Double(dimPercent) },
                    set: { dimPercent = Int($0.rounded()) }
                ), in: 1...20, step: 1)
                Text("This changes physical OLED output only. Digital screenshots and UI automation remain full fidelity.")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }

            Divider()
            Toggle("70–80% charge guard", isOn: $chargeControlEnabled)
                .disabled(state?.chargeGuard.available != true)
            HStack(spacing: 12) {
                Stepper("Floor \(chargeFloorPercent)%", value: $chargeFloorPercent, in: 40...79)
                Stepper("Ceiling \(chargeCeilingPercent)%", value: $chargeCeilingPercent, in: max(chargeFloorPercent + 5, 50)...90)
            }
            .font(.caption)
            .onChange(of: chargeFloorPercent) { newValue in
                if chargeCeilingPercent < newValue + 5 { chargeCeilingPercent = min(90, newValue + 5) }
            }

            if let state {
                Text(chargeGuardExplanation(state))
                    .font(.caption2)
                    .foregroundStyle(state.chargeGuard.state == "error" ? Color.orange : Color.secondary)
            }

            Divider()
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Thermal pause")
                    Text("Pause at \(temperature(thermalPauseCentiC)); resume at \(temperature(thermalResumeCentiC))")
                        .font(.caption2).foregroundStyle(.secondary)
                }
                Spacer()
                Stepper("", value: $thermalPauseCentiC, in: max(thermalResumeCentiC + 200, 3000)...4500, step: 100)
                    .labelsHidden()
            }

            Button {
                showApplyConfirmation = true
            } label: {
                HStack {
                    if applying { ProgressView().controlSize(.small) }
                    Text(enabled ? "Apply Remote Worker Settings" : "Disable Remote Worker")
                        .fontWeight(.semibold)
                    Spacer()
                    Image(systemName: "checkmark.circle.fill")
                }
                .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(applying || loading || !configurationIsValid)
        }
    }

    private var safetyCard: some View {
        RemoteWorkerSection(title: "Safety & local visibility", subtitle: "Human access always has a fast escape hatch") {
            if let state {
                RemoteWorkerRow(title: "Battery health", value: state.battery.designCapacityMah > 0 ? "\(state.battery.healthPercent)% · \(state.battery.cycleCount) cycles" : "Unavailable")
                RemoteWorkerRow(title: "External power", value: state.battery.externalPowerConnected ? "Connected" : "Battery")
                RemoteWorkerRow(title: "Charge guard", value: state.chargeGuard.state.replacingOccurrences(of: "_", with: " ").capitalized)
                RemoteWorkerRow(title: "Passcode policy", value: "Never bypassed")
            }
            Divider()
            HStack(spacing: 10) {
                Button("Boost to 35%") { UIScreen.main.brightness = 0.35 }
                    .buttonStyle(.bordered)
                Button("Return to worker level") { applyTargetBrightness() }
                    .buttonStyle(.bordered)
            }
            Text("Thermal protection has priority over availability. If the battery reaches the pause threshold, RootTools releases its display assertion and UI jobs wait instead of forcing the phone to remain awake.")
                .font(.caption2)
                .foregroundStyle(.secondary)
        }
    }

    private var heroTitle: String {
        guard let state else { return "Loading worker state…" }
        if state.thermal.paused { return "Paused for temperature" }
        return state.enabled ? "Remote Worker is active" : "Remote Worker is off"
    }

    private var heroSubtitle: String {
        guard let state else { return "Reading daemon, display and battery state" }
        if state.thermal.paused && !state.battery.temperatureAvailable { return "UI tasks are held because battery temperature is unavailable" }
        if state.thermal.paused { return "UI tasks are held until the device cools" }
        if state.enabled { return "Low-brightness always-on execution · daemon-managed" }
        return "Normal iOS idle and charging behavior"
    }

    private var heroHealthy: Bool {
        guard let state else { return false }
        return !state.enabled || (state.display.awakeAssertionActive && !state.thermal.paused)
    }

    private var configurationIsValid: Bool {
        dimPercent >= 1 && dimPercent <= 20 &&
        chargeFloorPercent >= 40 && chargeCeilingPercent <= 90 && chargeCeilingPercent >= chargeFloorPercent + 5 &&
        thermalResumeCentiC >= 2500 && thermalResumeCentiC <= 3900 &&
        thermalPauseCentiC >= thermalResumeCentiC + 200 && thermalPauseCentiC <= 4500
    }

    @MainActor
    private func load() async {
        guard !loading else { return }
        loading = true
        defer { loading = false }
        do {
            async let worker = DaemonClient.shared.remoteWorkerState()
            async let lock = DaemonClient.shared.lockState()
            let (loadedWorker, loadedLock) = try await (worker, lock)
            state = loadedWorker
            lockState = loadedLock
            enabled = loadedWorker.enabled
            dimPercent = loadedWorker.display.targetBrightnessPercent
            chargeFloorPercent = loadedWorker.chargeGuard.floorPercent
            chargeCeilingPercent = loadedWorker.chargeGuard.ceilingPercent
            thermalPauseCentiC = loadedWorker.thermal.pauseCentiC
            thermalResumeCentiC = loadedWorker.thermal.resumeCentiC
            chargeControlEnabled = loadedWorker.chargeGuard.enabled
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    @MainActor
    private func applyConfiguration() async {
        guard !applying && configurationIsValid else { return }
        applying = true
        errorMessage = nil
        defer { applying = false }
        do {
            let receipt = try await DaemonClient.shared.configureRemoteWorker(
                enabled: enabled,
                dimPercent: dimPercent,
                chargeFloorPercent: chargeFloorPercent,
                chargeCeilingPercent: chargeCeilingPercent,
                thermalPauseCentiC: thermalPauseCentiC,
                thermalResumeCentiC: thermalResumeCentiC,
                chargeControlEnabled: chargeControlEnabled
            )
            guard receipt.ok else { throw DaemonError.actionFailed(receipt.message) }
            if enabled { applyTargetBrightness(captureRestorePoint: true) }
            else { restoreBrightness() }
            await load()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    @MainActor
    private func applyTargetBrightness(captureRestorePoint: Bool = false) {
        if captureRestorePoint && !hasSavedBrightness {
            savedBrightness = max(Double(UIScreen.main.brightness), 0.10)
            hasSavedBrightness = true
        }
        UIScreen.main.brightness = CGFloat(Double(dimPercent) / 100.0)
    }

    @MainActor
    private func restoreBrightness() {
        UIScreen.main.brightness = CGFloat(hasSavedBrightness ? savedBrightness : 0.35)
        hasSavedBrightness = false
    }

    private func temperature(_ centiC: Int) -> String {
        String(format: "%.1f°C", Double(centiC) / 100.0)
    }

    private func chargeGuardExplanation(_ state: RemoteWorkerState) -> String {
        if !chargeControlEnabled { return "Observe only. RootTools will report battery and power telemetry without changing charging behavior." }
        if !state.chargeGuard.available { return "Charging control is unavailable on this runtime; enabling the guard will remain fail-safe and non-destructive." }
        if !state.chargeGuard.verified { return "The private charging control path is available but not yet verified on this battery. Verification occurs on the first reversible inhibit command." }
        if state.chargeGuard.inhibited { return "Charging is currently inhibited; external power may continue feeding the phone while the battery rests." }
        return "Charging control is verified and currently allows charging within the configured hysteresis window."
    }
}

private struct RemoteWorkerSection<Content: View>: View {
    let title: String
    let subtitle: String
    @ViewBuilder let content: Content

    init(title: String, subtitle: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.subtitle = subtitle
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 13) {
            VStack(alignment: .leading, spacing: 3) {
                Text(title).font(.headline)
                Text(subtitle).font(.caption).foregroundStyle(.secondary)
            }
            content
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(16)
        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 20))
    }
}

private struct RemoteWorkerMetric: View {
    let title: String
    let value: String

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(title).font(.caption2).foregroundStyle(.secondary)
            Text(value).font(.caption.weight(.semibold)).monospacedDigit().lineLimit(1).minimumScaleFactor(0.7)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(12)
        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 15))
    }
}

private struct RemoteWorkerBadge: View {
    let text: String
    let good: Bool

    var body: some View {
        HStack(spacing: 5) {
            Circle().fill(good ? Color.green : Color.orange).frame(width: 6, height: 6)
            Text(text).font(.caption2.weight(.semibold))
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 5)
        .background(Color.primary.opacity(0.06), in: Capsule())
    }
}

private struct RemoteWorkerRow: View {
    let title: String
    let value: String

    var body: some View {
        HStack {
            Text(title).foregroundStyle(.secondary)
            Spacer()
            Text(value).font(.system(.caption, design: .monospaced))
        }
        .font(.caption)
    }
}

import SwiftUI

struct DashboardView: View {
    @EnvironmentObject private var store: DeviceStore
    private let columns = [GridItem(.flexible(), spacing: 12), GridItem(.flexible(), spacing: 12)]

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    hero
                    healthStrip
                    NavigationLink {
                        ActionsView()
                    } label: {
                        HStack(spacing: 14) {
                            Image(systemName: "switch.2")
                                .font(.title2)
                                .frame(width: 44, height: 44)
                                .background(Color.orange.opacity(0.16), in: RoundedRectangle(cornerRadius: 13))
                            VStack(alignment: .leading, spacing: 4) {
                                Text("Controlled Actions").font(.subheadline.weight(.semibold)).foregroundStyle(.primary)
                                Text("Typed R1/R2 operations · audited · no raw shell")
                                    .font(.caption2).foregroundStyle(.secondary)
                            }
                            Spacer()
                            Image(systemName: "chevron.right").font(.caption.weight(.bold)).foregroundStyle(.tertiary)
                        }
                        .padding(15)
                        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 18))
                    }
                    .buttonStyle(.plain)
                    LazyVGrid(columns: columns, spacing: 12) {
                        ForEach(ToolKind.allCases) { tool in
                            NavigationLink(value: tool) {
                                ToolCard(tool: tool, status: store.status)
                            }
                            .buttonStyle(.plain)
                        }
                    }
                    capabilityFooter
                }
                .padding(16)
            }
            .background(Color(uiColor: .systemGroupedBackground).ignoresSafeArea())
            .navigationTitle("Root Tools")
            .navigationBarTitleDisplayMode(.inline)
            .navigationDestination(for: ToolKind.self) { ToolDetailView(tool: $0) }
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button {
                        Task { await store.refresh() }
                    } label: {
                        Image(systemName: "arrow.clockwise")
                    }
                }
            }
            .task { await store.refresh() }
        }
    }

    private var hero: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(alignment: .top) {
                ZStack {
                    RoundedRectangle(cornerRadius: 16).fill(.ultraThinMaterial).frame(width: 54, height: 54)
                    Image(systemName: "iphone.gen3.radiowaves.left.and.right").font(.title2)
                }
                VStack(alignment: .leading, spacing: 4) {
                    Text(store.status.machine).font(.headline)
                    Text("iOS build \(store.status.osBuild)").font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                StatusPill(ok: store.daemonReachable, text: store.daemonReachable ? "ROOT ONLINE" : "OFFLINE")
            }

            HStack(spacing: 9) {
                MiniBadge(text: store.status.jailbreakRootless ? "ROOTLESS" : "NO JB", symbol: "checkmark.shield.fill")
                MiniBadge(text: "UID \(store.status.uid)", symbol: "person.badge.key.fill")
                MiniBadge(text: "v\(store.status.daemonVersion)", symbol: "cpu.fill")
            }
        }
        .padding(18)
        .background(
            LinearGradient(colors: [Color.indigo.opacity(0.55), Color.blue.opacity(0.22)], startPoint: .topLeading, endPoint: .bottomTrailing),
            in: RoundedRectangle(cornerRadius: 24)
        )
        .overlay(RoundedRectangle(cornerRadius: 24).stroke(Color.white.opacity(0.08)))
    }

    private var healthStrip: some View {
        HStack(spacing: 10) {
            StatTile(title: "CPU", value: store.status.cpuCount == 0 ? "—" : "\(store.status.cpuCount) cores")
            StatTile(title: "Memory", value: formatBytes(store.status.memoryBytes))
            StatTile(title: "/var free", value: formatBytes(store.status.varFreeBytes))
        }
    }

    private var capabilityFooter: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Privileged adapters").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
            HStack(spacing: 8) {
                CapabilityDot(label: "SSH", ready: store.status.sshReady)
                CapabilityDot(label: "Frida", ready: store.status.fridaReady)
                CapabilityDot(label: "ZXTouch", ready: store.status.zxTouchReady)
                CapabilityDot(label: "Dopamine", ready: store.status.dopamineRunning)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(15)
        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 18))
    }

    private func formatBytes(_ bytes: UInt64) -> String {
        guard bytes > 0 else { return "—" }
        return ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .memory)
    }
}

private struct ToolCard: View {
    let tool: ToolKind
    let status: DeviceStatus

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Image(systemName: tool.symbol)
                .font(.title2)
                .frame(width: 38, height: 38)
                .background(Color.accentColor.opacity(0.18), in: RoundedRectangle(cornerRadius: 12))
            Text(tool.title).font(.subheadline.weight(.semibold)).foregroundStyle(.primary)
            Text(tool.subtitle).font(.caption2).foregroundStyle(.secondary).lineLimit(2)
        }
        .frame(maxWidth: .infinity, minHeight: 118, alignment: .leading)
        .padding(14)
        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 20))
        .overlay(RoundedRectangle(cornerRadius: 20).stroke(Color.white.opacity(0.05)))
    }
}

private struct StatTile: View {
    let title: String
    let value: String
    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            Text(title).font(.caption2).foregroundStyle(.secondary)
            Text(value).font(.caption.weight(.semibold)).lineLimit(1).minimumScaleFactor(0.7)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(12)
        .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 15))
    }
}

private struct StatusPill: View {
    let ok: Bool
    let text: String
    var body: some View {
        HStack(spacing: 5) {
            Circle().fill(ok ? Color.green : Color.red).frame(width: 7, height: 7)
            Text(text).font(.caption2.weight(.bold))
        }
        .padding(.horizontal, 9).padding(.vertical, 6)
        .background(.ultraThinMaterial, in: Capsule())
    }
}

private struct MiniBadge: View {
    let text: String
    let symbol: String
    var body: some View {
        Label(text, systemImage: symbol)
            .font(.caption2.weight(.semibold))
            .padding(.horizontal, 9).padding(.vertical, 6)
            .background(Color.white.opacity(0.08), in: Capsule())
    }
}

private struct CapabilityDot: View {
    let label: String
    let ready: Bool
    var body: some View {
        HStack(spacing: 5) {
            Circle().fill(ready ? Color.green : Color.orange).frame(width: 6, height: 6)
            Text(label).font(.caption2)
        }
        .padding(.horizontal, 8).padding(.vertical, 5)
        .background(Color.primary.opacity(0.06), in: Capsule())
    }
}


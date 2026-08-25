import SwiftUI

@main
struct RootToolsApp: App {
    @StateObject private var store = DeviceStore()

    var body: some Scene {
        WindowGroup {
            RootShellView()
                .environmentObject(store)
        }
    }
}

private enum RootTab: Hashable {
    case overview
    case device
    case tasks
    case agents
    case settings
}

private struct RootShellView: View {
    @EnvironmentObject private var store: DeviceStore
    @State private var selection: RootTab = .overview

    var body: some View {
        TabView(selection: $selection) {
            DashboardView()
                .tabItem { Label("Overview", systemImage: "house.fill") }
                .tag(RootTab.overview)

            DeviceHubView()
                .tabItem { Label("Device", systemImage: "iphone.gen3") }
                .tag(RootTab.device)

            TasksHubView()
                .tabItem { Label("Tasks", systemImage: "bolt.horizontal.circle.fill") }
                .tag(RootTab.tasks)

            AgentsHubView()
                .tabItem { Label("Agents", systemImage: "person.2.fill") }
                .tag(RootTab.agents)

            SettingsHubView()
                .tabItem { Label("Settings", systemImage: "gearshape.fill") }
                .tag(RootTab.settings)
        }
        .task { await store.refresh() }
    }
}


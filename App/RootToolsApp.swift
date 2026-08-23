import SwiftUI

@main
struct RootToolsApp: App {
    @StateObject private var store = DeviceStore()

    var body: some Scene {
        WindowGroup {
            DashboardView()
                .environmentObject(store)
                .preferredColorScheme(.dark)
        }
    }
}


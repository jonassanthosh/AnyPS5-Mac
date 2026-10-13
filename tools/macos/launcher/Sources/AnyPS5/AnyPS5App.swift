import AppKit
import SwiftUI

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }
}

@main
struct AnyPS5App: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var library = Library()

    var body: some Scene {
        WindowGroup("AnyPS5") {
            ContentView().environmentObject(library)
        }
        .commands {
            CommandGroup(replacing: .newItem) {
                Button("Add Games…") { library.chooseGame() }.keyboardShortcut("o")
            }
        }
        Settings {
            SettingsView().environmentObject(library)
        }
    }
}

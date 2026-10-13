import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @EnvironmentObject private var library: Library
    @State private var dropping = false

    var body: some View {
        NavigationSplitView {
            List(selection: $library.selection) {
                ForEach(library.games) { entry in
                    GameRow(entry: entry).tag(entry.id)
                }
            }
            .navigationSplitViewColumnWidth(min: 240, ideal: 280)
            .overlay {
                if library.games.isEmpty {
                    Text("No games yet").foregroundStyle(.secondary)
                }
            }
        } detail: {
            if let entry = library.selected {
                GameDetail(entry: entry)
            } else {
                EmptyLibrary()
            }
        }
        .toolbar {
            ToolbarItem {
                Button {
                    library.rescan()
                } label: {
                    Label("Rescan Folders", systemImage: "arrow.clockwise")
                }
                .help("Look for new games in your game folders")
                .disabled(library.folders.isEmpty || library.scanning)
            }
            ToolbarItem {
                Button {
                    library.chooseGame()
                } label: {
                    Label("Add Games", systemImage: "plus")
                }
                .help("Add a PS5 game folder, or a folder of games")
            }
        }
        .onDrop(of: [.fileURL], isTargeted: $dropping) { providers in
            for provider in providers {
                _ = provider.loadObject(ofClass: URL.self) { url, _ in
                    guard let url else { return }
                    Task { @MainActor in library.add(url) }
                }
            }
            return true
        }
        .overlay {
            if dropping {
                RoundedRectangle(cornerRadius: 12).stroke(Color.accentColor, lineWidth: 3).padding(6)
            }
        }
        .alert("AnyPS5", isPresented: Binding(get: { library.problem != nil }, set: { if !$0 { library.problem = nil } })) {
            Button("OK") { library.problem = nil }
        } message: {
            Text(library.problem ?? "")
        }
        .frame(minWidth: 820, minHeight: 520)
    }
}

struct EmptyLibrary: View {
    @EnvironmentObject private var library: Library

    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: "gamecontroller").font(.system(size: 56)).foregroundStyle(.secondary)
            Text("Add a game to get started").font(.title2)
            Text("Drop a PS5 game folder here, or a folder of games to add every game in it. AnyPS5 remembers the folder and picks up games you add to it later.")
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 420)
            Button("Add Games…") { library.chooseGame() }.controlSize(.large)
        }
        .padding()
    }
}

struct GameIcon: View {
    let image: NSImage?
    let size: CGFloat

    var body: some View {
        Group {
            if let image {
                Image(nsImage: image).resizable().interpolation(.high)
            } else {
                ZStack {
                    Color.secondary.opacity(0.15)
                    Image(systemName: "gamecontroller").font(.system(size: size * 0.4)).foregroundStyle(.secondary)
                }
            }
        }
        .frame(width: size, height: size)
        .clipShape(RoundedRectangle(cornerRadius: size * 0.12))
    }
}

struct GameRow: View {
    @ObservedObject var entry: GameEntry

    var body: some View {
        HStack(spacing: 10) {
            GameIcon(image: entry.info.icon, size: 40)
            VStack(alignment: .leading, spacing: 2) {
                Text(entry.info.name).lineLimit(1)
                Text(summary).font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
        }
        .padding(.vertical, 2)
    }

    private var summary: String {
        switch entry.state {
        case .missing: return "Folder not found"
        case .unusable: return "Can't be prepared"
        case .unprepared: return "Not prepared"
        case .preparing(let phase, let done, let total, _): return Phase.describe(phase, done, total)
        case .ready: return "Ready to play"
        case .running: return "Playing"
        case .failed: return "Needs attention"
        }
    }
}

enum Phase {
    static let weights: [String: (start: Double, span: Double)] = [
        "tools": (0, 0.03), "translate": (0.03, 0.12), "compile": (0.15, 0.8), "link": (0.95, 0.05), "done": (1, 0)
    ]

    static func fraction(_ phase: String, _ done: Int, _ total: Int) -> Double {
        guard let weight = weights[phase] else { return 0 }
        let part = total > 0 ? Double(done) / Double(total) : 0
        return weight.start + weight.span * part
    }

    static func describe(_ phase: String, _ done: Int, _ total: Int) -> String {
        switch phase {
        case "translate": return "Translating module \(min(done + 1, total)) of \(total)"
        case "compile": return total == 0 ? "Nothing to compile" : "Compiling \(done) of \(total)"
        case "link": return "Linking"
        case "done": return "Finishing"
        default: return "Building the translator and libraries"
        }
    }
}

struct GameDetail: View {
    @EnvironmentObject private var library: Library
    @ObservedObject var entry: GameEntry
    @State private var confirmingRemoval = false
    @State private var editingSettings = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                header
                Divider()
                status
                if !entry.output.isEmpty {
                    BuildOutput(lines: entry.output)
                }
            }
            .padding(28)
            .frame(maxWidth: .infinity, alignment: .topLeading)
        }
        .toolbar {
            ToolbarItem {
                Button {
                    editingSettings = true
                } label: {
                    Label("Game Settings", systemImage: "slider.horizontal.3")
                }
                .help("Graphics, performance and build settings for this game")
            }
            ToolbarItem {
                Menu {
                    Button("Game Settings…") { editingSettings = true }
                    Button("Prepare Again") { entry.prepare(force: true) }.disabled(!entry.canPrepare)
                    Button("Open Build Log") { entry.openLog(entry.buildLog) }
                    Button("Open Game Log") { entry.openLog(entry.runLog) }
                    Button("Show Saves and Build Files") { entry.revealBuild() }
                    Button("Show Game Folder") { NSWorkspace.shared.activateFileViewerSelecting([entry.info.root]) }
                    Divider()
                    Button("Remove from Library…", role: .destructive) { confirmingRemoval = true }.disabled(entry.busy)
                } label: {
                    Label("More", systemImage: "ellipsis.circle")
                }
            }
        }
        .sheet(isPresented: $editingSettings) {
            GameSettingsSheet(entry: entry).environmentObject(library)
        }
        .confirmationDialog("Remove \(entry.info.name)?", isPresented: $confirmingRemoval) {
            Button("Remove and Delete Build Files and Saves", role: .destructive) { library.remove(entry, deletingBuild: true) }
            Button("Remove, Keep Files") { library.remove(entry, deletingBuild: false) }
        } message: {
            Text("The game folder itself is never deleted.")
        }
    }

    private var header: some View {
        HStack(alignment: .top, spacing: 20) {
            GameIcon(image: entry.info.icon, size: 128)
            VStack(alignment: .leading, spacing: 6) {
                Text(entry.info.name).font(.largeTitle.weight(.semibold)).textSelection(.enabled)
                Text([entry.info.titleID, entry.info.version.isEmpty ? nil : "Version \(entry.info.version)"].compactMap { $0 }.joined(separator: " · "))
                    .foregroundStyle(.secondary)
                Text(entry.info.root.path).font(.caption).foregroundStyle(.tertiary).lineLimit(2).textSelection(.enabled)
            }
        }
    }

    @ViewBuilder
    private var status: some View {
        switch entry.state {
        case .missing:
            Notice(symbol: "folder.badge.questionmark", title: "Game folder not found", text: "Reconnect the drive or add the folder again.")
            Button("Locate…") { library.chooseGame() }
        case .unusable(let reason):
            Notice(symbol: "lock.fill", title: "This dump can't be prepared", text: reason, tint: .orange)
        case .unprepared:
            Notice(symbol: "hammer", title: "Prepare this game", text: "Preparing translates the game to run natively on this Mac. The first time takes about 30–40 minutes; later updates are much faster.")
            Button {
                entry.prepare()
            } label: {
                Label("Prepare", systemImage: "hammer.fill").frame(minWidth: 120)
            }
            .controlSize(.large)
            .buttonStyle(.borderedProminent)
        case .preparing(let phase, let done, let total, let started):
            VStack(alignment: .leading, spacing: 10) {
                Text("Preparing…").font(.title3.weight(.semibold))
                ProgressView(value: Phase.fraction(phase, done, total))
                TimelineView(.periodic(from: started, by: 1)) { context in
                    Text("\(Phase.describe(phase, done, total)) · \(Elapsed.text(from: started, to: context.date))")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .monospacedDigit()
                }
                Button("Cancel", role: .cancel) { entry.cancel() }
            }
            .frame(maxWidth: 520, alignment: .leading)
        case .ready:
            VStack(alignment: .leading, spacing: 10) {
                Button {
                    entry.play()
                } label: {
                    Label("Play", systemImage: "play.fill").font(.title3).frame(minWidth: 140)
                }
                .controlSize(.large)
                .buttonStyle(.borderedProminent)
                .keyboardShortcut(.defaultAction)
                if entry.outdated {
                    Text("Build settings or AnyPS5 changed since this game was prepared. Play prepares it again first; unchanged code is not recompiled.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: 520, alignment: .leading)
                }
            }
        case .running(let started):
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 16) {
                    ProgressView().controlSize(.small)
                    TimelineView(.periodic(from: started, by: 1)) { context in
                        Text("Playing · \(Elapsed.text(from: started, to: context.date))").monospacedDigit()
                    }
                    Button("Stop", role: .destructive) { entry.stop() }
                }
                Text("The game runs in its own window. Closing that window also ends the game.").font(.callout).foregroundStyle(.secondary)
                Button {
                    entry.captureHangReport()
                } label: {
                    Label(entry.capturing ? "Capturing…" : "Capture Hang Report", systemImage: "stethoscope")
                }
                .disabled(entry.capturing)
                .help("If the game is stuck, records what every thread is doing so the problem can be found")
            }
        case .failed(let message):
            Notice(symbol: "exclamationmark.triangle.fill", title: "Something went wrong", text: message, tint: .orange)
            HStack {
                Button("Try Again") { entry.refresh(); if entry.state == .unprepared { entry.prepare() } }
                Button("Open Game Log") { entry.openLog(entry.runLog) }
                Button("Open Build Log") { entry.openLog(entry.buildLog) }
                Button("Dismiss") { entry.refresh() }
            }
        }
    }
}

struct Notice: View {
    let symbol: String
    let title: String
    let text: String
    var tint: Color = .accentColor

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: symbol).font(.title2).foregroundStyle(tint)
            VStack(alignment: .leading, spacing: 4) {
                Text(title).font(.title3.weight(.semibold))
                Text(text).foregroundStyle(.secondary).textSelection(.enabled).fixedSize(horizontal: false, vertical: true)
            }
        }
        .frame(maxWidth: 560, alignment: .leading)
    }
}

struct BuildOutput: View {
    let lines: [String]

    var body: some View {
        DisclosureGroup("Build output") {
            ScrollViewReader { proxy in
                ScrollView {
                    Text(lines.joined(separator: "\n"))
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(8)
                        .id("end")
                }
                .frame(height: 180)
                .background(Color.secondary.opacity(0.08))
                .clipShape(RoundedRectangle(cornerRadius: 6))
                .onChange(of: lines.count) { proxy.scrollTo("end", anchor: .bottom) }
            }
        }
    }
}

enum Elapsed {
    static func text(from start: Date, to now: Date) -> String {
        let seconds = max(0, Int(now.timeIntervalSince(start)))
        return seconds >= 3600 ? String(format: "%d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60) : String(format: "%d:%02d", seconds / 60, seconds % 60)
    }
}

struct SettingsView: View {
    var body: some View {
        TabView {
            GeneralSettingsView().tabItem { Label("General", systemImage: "gearshape") }
            GameDefaultsView().tabItem { Label("Game Defaults", systemImage: "slider.horizontal.3") }
        }
        .frame(width: 640, height: 620)
    }
}

struct GeneralSettingsView: View {
    @EnvironmentObject private var library: Library

    var body: some View {
        Form {
            LabeledContent("AnyPS5 checkout") {
                HStack {
                    Text(library.repository?.path ?? "Not set").foregroundStyle(library.repository == nil ? .red : .primary).lineLimit(1).truncationMode(.middle)
                    Button("Choose…") { library.chooseRepository() }
                }
            }
            Text("Games are translated with the tools in this checkout. It needs build-arm64 configured and Xcode's command line tools, ninja and cmake installed.")
                .font(.caption)
                .foregroundStyle(.secondary)
            Section("Game folders") {
                if library.folders.isEmpty {
                    Text("None yet. Add a folder of games to have new games appear automatically.").foregroundStyle(.secondary)
                }
                ForEach(library.folders, id: \.self) { folder in
                    HStack {
                        Text(folder.path).lineLimit(1).truncationMode(.middle)
                        Spacer()
                        Button("Remove") { library.removeFolder(folder) }
                    }
                }
                Button("Add Folder…") { library.chooseGame() }
            }
            LabeledContent("Saves and builds") {
                Button("Show in Finder") {
                    try? FileManager.default.createDirectory(at: Paths.titles, withIntermediateDirectories: true)
                    NSWorkspace.shared.activateFileViewerSelecting([Paths.titles])
                }
            }
        }
        .formStyle(.grouped)
    }
}

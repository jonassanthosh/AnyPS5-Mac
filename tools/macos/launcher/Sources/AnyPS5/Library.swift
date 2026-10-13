import AppKit
import Foundation

enum Paths {
    static let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("AnyPS5")
    static let titles = support.appendingPathComponent("Titles")
    static let games = support.appendingPathComponent("games.json")
    static let searchPath = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"

    static func defaultRepository() -> URL? {
        if let configured = Bundle.main.object(forInfoDictionaryKey: "AnyPS5Repository") as? String, isRepository(URL(fileURLWithPath: configured)) {
            return URL(fileURLWithPath: configured)
        }
        var folder = Bundle.main.executableURL?.deletingLastPathComponent()
        while let current = folder, current.path != "/" {
            if isRepository(current) { return current }
            folder = current.deletingLastPathComponent()
        }
        return nil
    }

    static func isRepository(_ url: URL) -> Bool {
        FileManager.default.fileExists(atPath: url.appendingPathComponent("tools/macos/title.py").path)
    }
}

@MainActor
final class Library: ObservableObject {
    @Published private(set) var games: [GameEntry] = []
    @Published var selection: GameEntry.ID?
    @Published var problem: String?
    @Published var repository: URL? {
        didSet { UserDefaults.standard.set(repository?.path, forKey: "repository") }
    }

    init() {
        if let saved = UserDefaults.standard.string(forKey: "repository"), Paths.isRepository(URL(fileURLWithPath: saved)) {
            repository = URL(fileURLWithPath: saved)
        } else {
            repository = Paths.defaultRepository()
        }
        load()
    }

    var selected: GameEntry? {
        games.first { $0.id == selection }
    }

    func add(_ url: URL) {
        do {
            let info = try GameInspector.inspect(url)
            if let existing = games.first(where: { $0.info.titleID == info.titleID }) {
                if existing.info.root != info.root { existing.replace(info) }
                selection = existing.id
            } else {
                let entry = GameEntry(info: info, library: self)
                games.append(entry)
                games.sort { $0.info.name.localizedCaseInsensitiveCompare($1.info.name) == .orderedAscending }
                selection = entry.id
            }
            save()
        } catch {
            problem = error.localizedDescription
        }
    }

    func remove(_ entry: GameEntry, deletingBuild: Bool) {
        entry.cancel()
        entry.stop()
        if deletingBuild { try? FileManager.default.removeItem(at: entry.folder) }
        games.removeAll { $0 === entry }
        if selection == entry.id { selection = games.first?.id }
        save()
    }

    func chooseGame() {
        let panel = NSOpenPanel()
        panel.message = "Choose a PS5 game folder (the folder that contains sce_sys)."
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = true
        if panel.runModal() == .OK { panel.urls.forEach(add) }
    }

    func chooseRepository() {
        let panel = NSOpenPanel()
        panel.message = "Choose your AnyPS5 checkout."
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        if Paths.isRepository(url) {
            repository = url
        } else {
            problem = "\(url.path) is not an AnyPS5 checkout: tools/macos/title.py is missing."
        }
    }

    private func load() {
        guard let data = try? Data(contentsOf: Paths.games),
              let roots = try? JSONDecoder().decode([String].self, from: data) else { return }
        for root in roots {
            if let info = try? GameInspector.inspect(URL(fileURLWithPath: root)) {
                games.append(GameEntry(info: info, library: self))
            } else {
                games.append(GameEntry(missing: URL(fileURLWithPath: root), library: self))
            }
        }
        games.sort { $0.info.name.localizedCaseInsensitiveCompare($1.info.name) == .orderedAscending }
        selection = games.first?.id
    }

    func save() {
        try? FileManager.default.createDirectory(at: Paths.support, withIntermediateDirectories: true)
        let roots = games.map { $0.info.root.path }
        if let data = try? JSONEncoder().encode(roots) { try? data.write(to: Paths.games, options: .atomic) }
    }
}

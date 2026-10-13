import AppKit
import Foundation

enum Paths {
    static let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("AnyPS5")
    static let titles = support.appendingPathComponent("Titles")
    static let games = support.appendingPathComponent("games.json")
    static let settings = support.appendingPathComponent("settings.json")
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
    @Published private(set) var folders: [URL] = []
    @Published private(set) var scanning = false
    private var hidden = Set(UserDefaults.standard.stringArray(forKey: "hidden") ?? [])
    @Published var repository: URL? {
        didSet {
            UserDefaults.standard.set(repository?.path, forKey: "repository")
            catalog = OptionCatalog.load(repository: repository)
        }
    }
    @Published private(set) var catalog = OptionCatalog(groups: [])
    @Published private(set) var defaults = GameSettings.load(Paths.settings)

    init() {
        if let saved = UserDefaults.standard.string(forKey: "repository"), Paths.isRepository(URL(fileURLWithPath: saved)) {
            repository = URL(fileURLWithPath: saved)
        } else {
            repository = Paths.defaultRepository()
        }
        catalog = OptionCatalog.load(repository: repository)
        folders = (UserDefaults.standard.stringArray(forKey: "folders") ?? []).map { URL(fileURLWithPath: $0) }
        load()
        rescan()
    }

    func saveDefaults(_ settings: GameSettings) {
        do {
            try settings.save(Paths.settings)
            defaults = settings
            games.forEach { $0.checkCurrent() }
        } catch {
            problem = "The settings could not be saved: \(error.localizedDescription)"
        }
    }

    var selected: GameEntry? {
        games.first { $0.id == selection }
    }

    func add(_ url: URL) {
        if GameInspector.root(of: url) != nil {
            do {
                let info = try GameInspector.inspect(url)
                hide(info.titleID, false)
                selection = insert(info).id
                save()
            } catch {
                problem = error.localizedDescription
            }
        } else {
            addFolder(url)
        }
    }

    func addFolder(_ url: URL) {
        let folder = url.standardizedFileURL
        if !folders.contains(folder) {
            folders.append(folder)
            UserDefaults.standard.set(folders.map(\.path), forKey: "folders")
        }
        scan([folder], report: true)
    }

    func removeFolder(_ url: URL) {
        folders.removeAll { $0 == url }
        UserDefaults.standard.set(folders.map(\.path), forKey: "folders")
    }

    func rescan() {
        games.forEach { $0.refresh() }
        scan(folders, report: false)
    }

    private func scan(_ roots: [URL], report: Bool) {
        guard !roots.isEmpty else { return }
        scanning = true
        Task.detached(priority: .userInitiated) {
            var found: [GameInfo] = []
            var failures: [String] = []
            for root in roots {
                for game in GameInspector.scan(root) {
                    do {
                        found.append(try GameInspector.inspect(game))
                    } catch {
                        failures.append(error.localizedDescription)
                    }
                }
            }
            await MainActor.run { [found, failures] in
                self.scanning = false
                let before = self.games.count
                found.filter { !self.hidden.contains($0.titleID) }.forEach { _ = self.insert($0) }
                self.save()
                if self.selection == nil { self.selection = self.games.first?.id }
                guard report else { return }
                if found.isEmpty {
                    self.problem = failures.first ?? "No PS5 games were found in \(roots.map(\.lastPathComponent).joined(separator: ", ")). A game folder contains sce_sys/param.json."
                } else if self.games.count == before {
                    self.problem = "Every game in that folder is already in the library."
                }
            }
        }
    }

    private func hide(_ titleID: String, _ hiding: Bool) {
        if hiding { hidden.insert(titleID) } else { hidden.remove(titleID) }
        UserDefaults.standard.set(Array(hidden).sorted(), forKey: "hidden")
    }

    @discardableResult
    private func insert(_ info: GameInfo) -> GameEntry {
        if let existing = games.first(where: { $0.info.titleID == info.titleID }) {
            if existing.info.root != info.root || existing.state == .missing { existing.replace(info) }
            return existing
        }
        let entry = GameEntry(info: info, library: self)
        games.append(entry)
        games.sort { $0.info.name.localizedCaseInsensitiveCompare($1.info.name) == .orderedAscending }
        return entry
    }

    func remove(_ entry: GameEntry, deletingBuild: Bool) {
        entry.cancel()
        entry.stop()
        if deletingBuild { try? FileManager.default.removeItem(at: entry.folder) }
        hide(entry.info.titleID, true)
        games.removeAll { $0 === entry }
        if selection == entry.id { selection = games.first?.id }
        save()
    }

    func chooseGame() {
        let panel = NSOpenPanel()
        panel.message = "Choose a PS5 game folder, or a folder of games to add them all."
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

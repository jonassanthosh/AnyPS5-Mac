import AppKit
import Foundation

struct GameInfo: Equatable {
    let root: URL
    let titleID: String
    let name: String
    let version: String
    let icon: NSImage?
    var problem: String? = nil

    static func == (left: GameInfo, right: GameInfo) -> Bool {
        left.root == right.root && left.titleID == right.titleID
    }
}

enum GameProblem: LocalizedError {
    case notAGame(URL)
    case unreadableParameters(URL)

    var errorDescription: String? {
        switch self {
        case .notAGame(let url):
            return "\(url.lastPathComponent) is not a PS5 game folder: it has no sce_sys/param.json."
        case .unreadableParameters(let url):
            return "The game parameters in \(url.path) could not be read."
        }
    }
}

enum GameInspector {
    static func root(of url: URL) -> URL? {
        for candidate in [url, url.deletingLastPathComponent()] {
            if FileManager.default.fileExists(atPath: candidate.appendingPathComponent("sce_sys/param.json").path) {
                return candidate.standardizedFileURL
            }
        }
        return nil
    }

    static func inspect(_ url: URL) throws -> GameInfo {
        guard let root = root(of: url) else { throw GameProblem.notAGame(url) }
        let parametersURL = root.appendingPathComponent("sce_sys/param.json")
        guard let data = try? Data(contentsOf: parametersURL),
              let parameters = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let titleID = parameters["titleId"] as? String, !titleID.isEmpty else {
            throw GameProblem.unreadableParameters(parametersURL)
        }
        let localized = parameters["localizedParameters"] as? [String: Any] ?? [:]
        let language = localized["defaultLanguage"] as? String ?? "en-US"
        let names = (localized[language] as? [String: Any]) ?? (localized["en-US"] as? [String: Any]) ?? [:]
        let name = names["titleName"] as? String ?? titleID
        let version = parameters["contentVersion"] as? String ?? ""
        let icon = NSImage(contentsOf: root.appendingPathComponent("sce_sys/icon0.png"))
        return GameInfo(root: root, titleID: titleID, name: name, version: version, icon: icon, problem: executableProblem(root))
    }

    static func executableProblem(_ root: URL) -> String? {
        var found = false
        for folder in [root.appendingPathComponent("decrypted"), root] {
            let eboot = folder.appendingPathComponent("eboot.bin")
            if decrypted(eboot) != nil { return nil }
            if FileManager.default.fileExists(atPath: eboot.path) { found = true }
        }
        return found ? "eboot.bin is encrypted and the folder has no decrypted copy of it (such as eboot.bin.esbak or eboot.bin.bak_…). AnyPS5 needs a decrypted dump." : "The folder has no eboot.bin."
    }

    static func decrypted(_ file: URL) -> URL? {
        if isElf(file) { return file }
        let name = file.lastPathComponent
        let siblings = (try? FileManager.default.contentsOfDirectory(at: file.deletingLastPathComponent(), includingPropertiesForKeys: nil)) ?? []
        let backups = siblings.filter { sibling in
            let other = sibling.lastPathComponent
            guard other.hasPrefix(name + ".") else { return false }
            return other.dropFirst(name.count).range(of: #"^\.(esbak|bak|orig|elf|bak_[0-9_]+)$"#, options: .regularExpression) != nil
        }
        return backups.sorted { $0.lastPathComponent < $1.lastPathComponent }.first(where: isElf)
    }

    static func isElf(_ url: URL) -> Bool {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return false }
        defer { try? handle.close() }
        return (try? handle.read(upToCount: 4)) == Data([0x7f, 0x45, 0x4c, 0x46])
    }

    static func scan(_ folder: URL, depth: Int = 4) -> [URL] {
        if FileManager.default.fileExists(atPath: folder.appendingPathComponent("sce_sys/param.json").path) {
            return [folder.standardizedFileURL]
        }
        guard depth > 0, let children = try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.isDirectoryKey, .isPackageKey], options: [.skipsHiddenFiles]) else { return [] }
        var found: [URL] = []
        for child in children.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            let values = try? child.resourceValues(forKeys: [.isDirectoryKey, .isPackageKey])
            guard values?.isDirectory == true, values?.isPackage != true else { continue }
            found += scan(child, depth: depth - 1)
        }
        return found
    }
}

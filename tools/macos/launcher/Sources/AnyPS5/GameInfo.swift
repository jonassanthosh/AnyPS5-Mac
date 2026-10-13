import AppKit
import Foundation

struct GameInfo: Equatable {
    let root: URL
    let titleID: String
    let name: String
    let version: String
    let icon: NSImage?

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
        return GameInfo(root: root, titleID: titleID, name: name, version: version, icon: icon)
    }
}

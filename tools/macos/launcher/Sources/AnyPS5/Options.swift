import Foundation

struct OptionChoice: Decodable, Hashable {
    let value: String
    let title: String
}

struct GameOption: Decodable, Identifiable {
    let id: String
    let title: String
    let help: String
    let `default`: String
    let choices: [OptionChoice]

    func title(of value: String) -> String {
        choices.first { $0.value == value }?.title ?? value
    }
}

struct OptionGroup: Decodable, Identifiable {
    let id: String
    let title: String
    let note: String?
    let options: [GameOption]
}

struct OptionCatalog: Decodable {
    let groups: [OptionGroup]

    static func load(repository: URL?) -> OptionCatalog {
        guard let repository,
              let data = try? Data(contentsOf: repository.appendingPathComponent("tools/macos/options.json")),
              let catalog = try? JSONDecoder().decode(OptionCatalog.self, from: data) else { return OptionCatalog(groups: []) }
        return catalog
    }
}

struct GameSettings: Codable, Equatable {
    var options: [String: String] = [:]
    var environment: String = ""

    static func load(_ url: URL) -> GameSettings {
        guard let data = try? Data(contentsOf: url), let settings = try? JSONDecoder().decode(GameSettings.self, from: data) else { return GameSettings() }
        return settings
    }

    func save(_ url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(self).write(to: url, options: .atomic)
    }
}

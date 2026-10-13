import AppKit
import Foundation

enum GameState: Equatable {
    case missing
    case unprepared
    case preparing(phase: String, done: Int, total: Int, started: Date)
    case ready
    case running(started: Date)
    case failed(String)
}

@MainActor
final class GameEntry: ObservableObject, Identifiable {
    nonisolated let id: String
    @Published private(set) var info: GameInfo
    @Published private(set) var state: GameState = .unprepared
    @Published private(set) var output: [String] = []
    private weak var library: Library?
    private var builder: Process?
    private var game: Process?
    private var stopping = false
    private var pending = ""

    init(info: GameInfo, library: Library) {
        self.id = info.root.path
        self.info = info
        self.library = library
        refresh()
    }

    init(missing root: URL, library: Library) {
        self.id = root.path
        self.info = GameInfo(root: root, titleID: root.lastPathComponent, name: root.lastPathComponent, version: "", icon: nil)
        self.library = library
        self.state = .missing
    }

    var folder: URL { Paths.titles.appendingPathComponent(info.titleID) }
    var runLog: URL { folder.appendingPathComponent("last-run.log") }
    var buildLog: URL { folder.appendingPathComponent("last-build.log") }

    var prepared: Bool {
        let record = folder.appendingPathComponent("title.json")
        guard FileManager.default.isExecutableFile(atPath: folder.appendingPathComponent("title").path),
              let data = try? Data(contentsOf: record),
              let fields = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let game = fields["game"] as? String else { return false }
        return URL(fileURLWithPath: game).standardizedFileURL == info.root
    }

    var busy: Bool {
        switch state {
        case .preparing, .running: return true
        default: return false
        }
    }

    func replace(_ newInfo: GameInfo) {
        guard !busy else { return }
        info = newInfo
        refresh()
    }

    func refresh() {
        guard !busy else { return }
        settle()
    }

    private func settle() {
        if !FileManager.default.fileExists(atPath: info.root.appendingPathComponent("sce_sys/param.json").path) {
            state = .missing
        } else {
            state = prepared ? .ready : .unprepared
        }
    }

    func prepare(force: Bool = false) {
        guard !busy, let script = script() else { return }
        output = []
        pending = ""
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        FileManager.default.createFile(atPath: buildLog.path, contents: nil)
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
        process.arguments = [script.path, "build", info.root.path, "--own-group"] + (force ? ["--force"] : [])
        process.environment = environment()
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        let log = try? FileHandle(forWritingTo: buildLog)
        pipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            guard !data.isEmpty else { return }
            log?.write(data)
            let text = String(decoding: data, as: UTF8.self)
            Task { @MainActor in self?.consume(text) }
        }
        process.terminationHandler = { [weak self] finished in
            let status = finished.terminationStatus
            let reason = finished.terminationReason
            pipe.fileHandleForReading.readabilityHandler = nil
            try? log?.close()
            Task { @MainActor in self?.prepared(status: status, reason: reason) }
        }
        state = .preparing(phase: "tools", done: 0, total: 0, started: Date())
        stopping = false
        do {
            try process.run()
            builder = process
        } catch {
            state = .failed("Could not start the build: \(error.localizedDescription)")
        }
    }

    func cancel() {
        guard let builder, builder.isRunning else { return }
        stopping = true
        kill(-builder.processIdentifier, SIGTERM)
    }

    func play() {
        guard !busy, let script = script() else { return }
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        FileManager.default.createFile(atPath: runLog.path, contents: nil)
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
        process.arguments = [script.path, "run", info.root.path]
        process.environment = environment()
        if let log = try? FileHandle(forWritingTo: runLog) {
            process.standardOutput = log
            process.standardError = log
        }
        process.terminationHandler = { [weak self] finished in
            let status = finished.terminationStatus
            let reason = finished.terminationReason
            Task { @MainActor in self?.played(status: status, reason: reason) }
        }
        stopping = false
        do {
            try process.run()
            game = process
            state = .running(started: Date())
        } catch {
            state = .failed("Could not start the game: \(error.localizedDescription)")
        }
    }

    func stop() {
        guard let game, game.isRunning else { return }
        stopping = true
        game.terminate()
    }

    func revealBuild() {
        NSWorkspace.shared.activateFileViewerSelecting([folder])
    }

    func openLog(_ url: URL) {
        NSWorkspace.shared.open(url)
    }

    private func script() -> URL? {
        guard let repository = library?.repository else {
            state = .failed("Set the AnyPS5 checkout in Settings first.")
            return nil
        }
        return repository.appendingPathComponent("tools/macos/title.py")
    }

    private func environment() -> [String: String] {
        var environment = ProcessInfo.processInfo.environment
        environment["PATH"] = Paths.searchPath + ":" + (environment["PATH"] ?? "")
        environment["PYTHONUNBUFFERED"] = "1"
        return environment
    }

    private func consume(_ text: String) {
        pending += text
        var lines = pending.components(separatedBy: "\n")
        pending = lines.removeLast()
        for line in lines where !line.isEmpty {
            let fields = line.split(separator: " ")
            if fields.count == 4, fields[0] == "progress", let done = Int(fields[2]), let total = Int(fields[3]) {
                if case .preparing(_, _, _, let started) = state {
                    state = .preparing(phase: String(fields[1]), done: done, total: total, started: started)
                }
            } else {
                output.append(line)
            }
        }
        if output.count > 400 { output.removeFirst(output.count - 400) }
    }

    private func prepared(status: Int32, reason: Process.TerminationReason) {
        builder = nil
        if stopping {
            stopping = false
            settle()
            return
        }
        if status == 0 && reason == .exit && prepared {
            state = .ready
            return
        }
        let error = output.last { $0.hasPrefix("error ") }.map { String($0.dropFirst(6)) }
        state = .failed(error ?? "Preparing the game failed (exit status \(status)). See the build log.")
    }

    private func played(status: Int32, reason: Process.TerminationReason) {
        game = nil
        let stopped = stopping
        stopping = false
        if stopped || (reason == .exit && status == 0) {
            settle()
            return
        }
        let how = reason == .uncaughtSignal ? "signal \(status)" : "exit status \(status)"
        state = .failed("The game stopped unexpectedly (\(how)). The run log has the details.")
    }
}

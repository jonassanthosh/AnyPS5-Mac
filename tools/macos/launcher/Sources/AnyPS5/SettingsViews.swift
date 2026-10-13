import AppKit
import SwiftUI

struct OptionsEditor: View {
    let catalog: OptionCatalog
    @Binding var settings: GameSettings
    let inherited: GameSettings?

    var body: some View {
        Form {
            if catalog.groups.isEmpty {
                Text("The options could not be read. Check the AnyPS5 checkout in General settings.").foregroundStyle(.secondary)
            }
            ForEach(catalog.groups) { group in
                Section {
                    ForEach(group.options) { option in
                        OptionPicker(option: option, settings: $settings, inherited: inherited)
                    }
                } header: {
                    Text(group.title)
                } footer: {
                    if let note = group.note { Text(note).font(.caption).foregroundStyle(.secondary) }
                }
            }
            Section {
                TextEditor(text: $settings.environment)
                    .font(.system(.body, design: .monospaced))
                    .frame(minHeight: 72)
            } header: {
                Text("Extra environment variables")
            } footer: {
                Text("One NAME=value per line, applied after the options above. For experimenting with switches that have no option yet.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }
}

struct OptionPicker: View {
    let option: GameOption
    @Binding var settings: GameSettings
    let inherited: GameSettings?

    var body: some View {
        let fallback = inherited?.options[option.id] ?? option.default
        Picker(selection: Binding(get: { settings.options[option.id] ?? "" }, set: { settings.options[option.id] = $0.isEmpty ? nil : $0 })) {
            Text("Default (\(option.title(of: fallback)))").tag("")
            Divider()
            ForEach(option.choices, id: \.value) { choice in
                Text(choice.title).tag(choice.value)
            }
        } label: {
            VStack(alignment: .leading, spacing: 2) {
                Text(option.title)
                Text(option.help).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

struct GameDefaultsView: View {
    @EnvironmentObject private var library: Library
    @State private var draft = GameSettings()
    @State private var loaded = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("These apply to every game unless a game's own settings change them.")
                .foregroundStyle(.secondary)
                .padding([.horizontal, .top])
            OptionsEditor(catalog: library.catalog, settings: $draft, inherited: nil)
        }
        .onAppear {
            draft = library.defaults
            loaded = true
        }
        .onChange(of: draft) {
            if loaded && draft != library.defaults { library.saveDefaults(draft) }
        }
    }
}

struct GameSettingsSheet: View {
    @EnvironmentObject private var library: Library
    @ObservedObject var entry: GameEntry
    @Environment(\.dismiss) private var dismiss
    @State private var draft = GameSettings()

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                GameIcon(image: entry.info.icon, size: 40)
                VStack(alignment: .leading) {
                    Text("Settings for \(entry.info.name)").font(.headline).lineLimit(1)
                    Text("Choices here override the game defaults in AnyPS5 Settings.").font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
            }
            .padding()
            Divider()
            OptionsEditor(catalog: library.catalog, settings: $draft, inherited: library.defaults)
            Divider()
            HStack {
                Button("Use Defaults for Everything") { draft = GameSettings() }
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Save") {
                    entry.save(draft)
                    dismiss()
                }
                .keyboardShortcut(.defaultAction)
                .disabled(entry.busy)
            }
            .padding()
        }
        .frame(width: 620, height: 640)
        .onAppear { draft = entry.settings }
    }
}

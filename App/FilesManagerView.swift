import SwiftUI

struct FilesManagerView: View {
    @State private var scope: FileScope = .mobile
    @State private var scopes: [FileScopeDescriptor] = []
    @State private var currentPath = ""
    @State private var entries: [FileEntryDescriptor] = []
    @State private var query = ""
    @State private var loading = false
    @State private var errorMessage: String?
    @State private var showCreate = false
    @State private var newFileName = "notes.txt"

    private var descriptor: FileScopeDescriptor? {
        scopes.first { $0.id == scope.rawValue }
    }

    private var filteredEntries: [FileEntryDescriptor] {
        let values = query.isEmpty ? entries : entries.filter { $0.name.localizedCaseInsensitiveContains(query) }
        return values.sorted {
            if $0.isDirectory != $1.isDirectory { return $0.isDirectory }
            if $0.isSymlink != $1.isSymlink { return !$0.isSymlink }
            return $0.name.localizedStandardCompare($1.name) == .orderedAscending
        }
    }

    var body: some View {
        List {
            Section {
                Picker("Scope", selection: $scope) {
                    ForEach(FileScope.allCases) { candidate in
                        Text(candidate.title).tag(candidate)
                    }
                }
                .pickerStyle(.segmented)

                VStack(alignment: .leading, spacing: 5) {
                    Text(currentPath.isEmpty ? "/" : "/\(currentPath)")
                        .font(.caption.monospaced())
                        .textSelection(.enabled)
                    if let descriptor {
                        Text(descriptor.root)
                            .font(.caption2.monospaced())
                            .foregroundStyle(.secondary)
                            .textSelection(.enabled)
                    }
                }

                if !currentPath.isEmpty {
                    Button {
                        navigateUp()
                    } label: {
                        Label("Up one level", systemImage: "arrow.up.to.line")
                    }
                }
            } header: {
                Text("Declared RootTools scope")
            } footer: {
                Text("Navigation is confined to the declared scope. RootTools rejects absolute paths, parent traversal and symlink following in the daemon.")
            }

            Section("Contents") {
                if loading && entries.isEmpty {
                    HStack { Spacer(); ProgressView(); Spacer() }
                } else if filteredEntries.isEmpty {
                    Text(query.isEmpty ? "This directory is empty" : "No matching files")
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(filteredEntries) { entry in
                        entryRow(entry)
                    }
                }
            }

            if let descriptor {
                Section("Limits") {
                    LabeledContent("Read") { Text(formatBytes(descriptor.maxReadBytes)) }
                    LabeledContent("Write") { Text(formatBytes(descriptor.maxWriteBytes)) }
                    Label("No symlink following", systemImage: "link.badge.plus")
                    Label("No arbitrary filesystem root", systemImage: "lock.shield.fill")
                }
                .font(.caption)
            }

            if let errorMessage {
                Section("Last error") {
                    Text(errorMessage)
                        .font(.caption)
                        .foregroundStyle(.red)
                        .textSelection(.enabled)
                }
            }
        }
        .navigationTitle("Files")
        .navigationBarTitleDisplayMode(.inline)
        .searchable(text: $query, prompt: "Search this directory")
        .toolbar {
            ToolbarItemGroup(placement: .navigationBarTrailing) {
                Button {
                    newFileName = "notes.txt"
                    showCreate = true
                } label: {
                    Image(systemName: "doc.badge.plus")
                }
                .disabled(descriptor?.write != true || loading)

                Button { Task { await refresh() } } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .disabled(loading)
            }
        }
        .alert("New text file", isPresented: $showCreate) {
            TextField("File name", text: $newFileName)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            Button("Cancel", role: .cancel) {}
            Button("Create") { Task { await createFile() } }
        } message: {
            Text("Creates an empty text file in the current RootTools scope directory. Nested path traversal is not accepted in this name field.")
        }
        .task { await loadInitial() }
        .onChange(of: scope) { _ in
            currentPath = ""
            query = ""
            Task { await refresh() }
        }
    }

    @ViewBuilder
    private func entryRow(_ entry: FileEntryDescriptor) -> some View {
        if entry.isDirectory {
            Button {
                currentPath = joined(currentPath, entry.name)
                query = ""
                Task { await refresh() }
            } label: {
                rowLabel(entry)
            }
            .buttonStyle(.plain)
        } else if entry.isSymlink {
            rowLabel(entry)
                .opacity(0.55)
        } else {
            NavigationLink {
                FileEditorView(
                    scope: scope,
                    path: joined(currentPath, entry.name),
                    entry: entry,
                    maxWriteBytes: descriptor?.maxWriteBytes ?? 16_384
                )
            } label: {
                rowLabel(entry)
            }
        }
    }

    private func rowLabel(_ entry: FileEntryDescriptor) -> some View {
        HStack(spacing: 12) {
            Image(systemName: symbol(entry))
                .font(.body.weight(.semibold))
                .frame(width: 36, height: 36)
                .background(Color.accentColor.opacity(entry.isSymlink ? 0.06 : 0.12), in: RoundedRectangle(cornerRadius: 10))
            VStack(alignment: .leading, spacing: 3) {
                Text(entry.name)
                    .font(.subheadline.weight(entry.isDirectory ? .semibold : .regular))
                    .foregroundStyle(.primary)
                HStack(spacing: 7) {
                    Text(entry.kind.uppercased())
                    Text(modeText(entry.mode))
                    if !entry.isDirectory { Text(formatBytes(entry.size)) }
                }
                .font(.caption2.monospaced())
                .foregroundStyle(.secondary)
                Text(Date(timeIntervalSince1970: TimeInterval(entry.modifiedAt)).formatted(date: .abbreviated, time: .shortened))
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
            }
            Spacer()
            if entry.isSymlink {
                Text("BLOCKED")
                    .font(.caption2.weight(.bold))
                    .foregroundStyle(.secondary)
            }
        }
        .contentShape(Rectangle())
        .padding(.vertical, 3)
    }

    @MainActor
    private func loadInitial() async {
        do {
            scopes = (try await DaemonClient.shared.fileScopes()).scopes
        } catch {
            errorMessage = error.localizedDescription
        }
        await refresh()
    }

    @MainActor
    private func refresh() async {
        guard !loading else { return }
        loading = true
        defer { loading = false }
        do {
            let payload = try await DaemonClient.shared.listFiles(scope: scope, path: currentPath)
            entries = payload.entries
            errorMessage = nil
        } catch {
            entries = []
            errorMessage = error.localizedDescription
        }
    }

    @MainActor
    private func createFile() async {
        let trimmed = newFileName.trimmingCharacters(in: .whitespacesAndNewlines)
        guard validLeafName(trimmed) else {
            errorMessage = "File name must be a simple non-hidden name without slashes or path traversal."
            return
        }
        do {
            let receipt = try await DaemonClient.shared.writeFile(scope: scope, path: joined(currentPath, trimmed), content: "")
            guard receipt.ok else { throw DaemonError.actionFailed(receipt.message) }
            await refresh()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    private func navigateUp() {
        guard let slash = currentPath.lastIndex(of: "/") else {
            currentPath = ""
            Task { await refresh() }
            return
        }
        currentPath = String(currentPath[..<slash])
        Task { await refresh() }
    }

    private func joined(_ base: String, _ leaf: String) -> String {
        base.isEmpty ? leaf : "\(base)/\(leaf)"
    }

    private func validLeafName(_ value: String) -> Bool {
        guard !value.isEmpty, value.count <= 96, !value.hasPrefix("."), !value.contains("..") else { return false }
        return value.allSatisfy { $0.isLetter || $0.isNumber || ".-_".contains($0) }
    }

    private func symbol(_ entry: FileEntryDescriptor) -> String {
        if entry.isDirectory { return "folder.fill" }
        if entry.isSymlink { return "link" }
        return "doc.text"
    }

    private func modeText(_ mode: UInt32) -> String {
        String(format: "%04o", mode)
    }

    private func formatBytes<T: BinaryInteger>(_ value: T) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(value), countStyle: .file)
    }
}

private struct FileEditorView: View {
    let scope: FileScope
    let path: String
    let entry: FileEntryDescriptor
    let maxWriteBytes: Int

    @State private var content = ""
    @State private var original = ""
    @State private var loading = false
    @State private var saving = false
    @State private var errorMessage: String?
    @State private var successMessage: String?

    private var byteCount: Int { content.lengthOfBytes(using: .utf8) }
    private var changed: Bool { content != original }

    var body: some View {
        Form {
            Section("File") {
                Text(path).font(.caption.monospaced()).textSelection(.enabled)
                LabeledContent("Size") { Text(ByteCountFormatter.string(fromByteCount: Int64(entry.size), countStyle: .file)) }
                LabeledContent("Mode") { Text(String(format: "%04o", entry.mode)).font(.caption.monospaced()) }
            }

            Section {
                if loading {
                    HStack { Spacer(); ProgressView(); Spacer() }
                } else {
                    TextEditor(text: $content)
                        .font(.system(.caption, design: .monospaced))
                        .frame(minHeight: 300)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                }
            } header: {
                HStack {
                    Text("UTF-8 text")
                    Spacer()
                    Text("\(byteCount) / \(maxWriteBytes) bytes")
                        .font(.caption2.monospaced())
                        .foregroundStyle(byteCount > maxWriteBytes ? Color.red : Color.secondary)
                }
            } footer: {
                Text("The daemon performs a bounded write, fsync, and read-back verification. Symlinks and paths outside the selected scope are rejected independently of this UI.")
            }

            if let successMessage {
                Section { Label(successMessage, systemImage: "checkmark.circle.fill").foregroundStyle(.green) }
            }
            if let errorMessage {
                Section { Text(errorMessage).font(.caption).foregroundStyle(.red).textSelection(.enabled) }
            }
        }
        .navigationTitle(entry.name)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .navigationBarTrailing) {
                Button("Save") { Task { await save() } }
                    .disabled(loading || saving || !changed || byteCount > maxWriteBytes)
            }
        }
        .task { await load() }
    }

    @MainActor
    private func load() async {
        loading = true
        defer { loading = false }
        do {
            let receipt = try await DaemonClient.shared.readFile(scope: scope, path: path)
            guard receipt.ok else { throw DaemonError.actionFailed(receipt.message) }
            content = receipt.output ?? ""
            original = content
            errorMessage = nil
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    @MainActor
    private func save() async {
        guard byteCount <= maxWriteBytes else { return }
        saving = true
        defer { saving = false }
        do {
            let receipt = try await DaemonClient.shared.writeFile(scope: scope, path: path, content: content)
            guard receipt.ok else { throw DaemonError.actionFailed(receipt.message) }
            original = content
            errorMessage = nil
            successMessage = receipt.message
        } catch {
            successMessage = nil
            errorMessage = error.localizedDescription
        }
    }
}

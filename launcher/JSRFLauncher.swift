//
//  JSRF Unleashed - launcher
//
//  Double-click, and the game runs. The first launch asks where the game's
//  files are; after that it remembers, and only asks again if they have gone.
//
//  What it remembers is a security-scoped bookmark rather than a path, so a
//  folder the user renames or moves still resolves and the question is not
//  asked a second time for no reason. "Ask again" is reserved for the case
//  where the files are genuinely not there any more, which is the only case
//  the user can actually do anything about.
//

import AppKit
import Foundation

// MARK: - where the game files are

enum GameFolder {
    static let bookmarkKey = "gameFolderBookmark"
    static let pathKey     = "gameFolderPath"

    /// A folder is the game if the XBE is in it. Accept the enclosing folder
    /// too: extraction tools drop the disc contents one level down as often
    /// as not, and making the user find the exact level is a question the
    /// launcher can answer itself.
    static func resolve(_ url: URL) -> URL? {
        let fm = FileManager.default
        if fm.fileExists(atPath: url.appendingPathComponent("default.xbe").path) {
            return url
        }
        guard let kids = try? fm.contentsOfDirectory(
            at: url, includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]) else { return nil }
        for kid in kids where (try? kid.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true {
            if fm.fileExists(atPath: kid.appendingPathComponent("default.xbe").path) {
                return kid
            }
        }
        return nil
    }

    /// The remembered folder, or nil if it was never set or is gone.
    static func remembered() -> URL? {
        let d = UserDefaults.standard
        if let data = d.data(forKey: bookmarkKey) {
            var stale = false
            if let url = try? URL(resolvingBookmarkData: data,
                                  options: [.withSecurityScope],
                                  relativeTo: nil,
                                  bookmarkDataIsStale: &stale) {
                _ = url.startAccessingSecurityScopedResource()
                if let game = resolve(url) {
                    if stale { remember(url) }
                    return game
                }
                url.stopAccessingSecurityScopedResource()
            }
        }
        // A bookmark can fail for reasons that have nothing to do with the
        // files (an unsigned rebuild, a different volume). The plain path is
        // the fallback, and it is also what the "Game folder" menu shows.
        if let p = d.string(forKey: pathKey) {
            return resolve(URL(fileURLWithPath: p))
        }
        return nil
    }

    static func remember(_ url: URL) {
        let d = UserDefaults.standard
        d.set(url.path, forKey: pathKey)
        if let data = try? url.bookmarkData(options: [.withSecurityScope],
                                            includingResourceValuesForKeys: nil,
                                            relativeTo: nil) {
            d.set(data, forKey: bookmarkKey)
        }
    }

    static func forget() {
        let d = UserDefaults.standard
        d.removeObject(forKey: bookmarkKey)
        d.removeObject(forKey: pathKey)
    }
}

// MARK: - discs

/// Where an extracted disc is kept, so the same ISO is never unpacked twice.
enum Library {
    /// Where the run logs go.
    ///
    /// Application Support is the correct place for them and the wrong place
    /// to have to read them from: it is a protected location, so anyone
    /// helping to debug this remotely -- which is how most of this port has
    /// been built -- cannot open it, and every crash report has to be copied
    /// out by hand. The logs are diagnostic output about the game, not user
    /// data, so they belong beside the app, in the folder the game already
    /// lives in. Falls back to Application Support if that folder is not
    /// writable (a read-only volume, or the app moved somewhere odd).
    static var logRoot: URL {
        let beside = Bundle.main.bundleURL.deletingLastPathComponent()
        if FileManager.default.isWritableFile(atPath: beside.path) { return beside }
        return root
    }

    static var root: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory,
                                            in: .userDomainMask)[0]
            .appendingPathComponent("JSRF Unleashed")
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base
    }

    static func extractedFolder(for iso: URL) -> URL {
        root.appendingPathComponent(iso.deletingPathExtension().lastPathComponent)
    }

    /// extract-xiso, wherever the user keeps it. Not bundled: it is somebody
    /// else's program and shipping it would make this a redistribution
    /// question rather than a launcher.
    static func extractTool() -> URL? {
        let candidates = [
            "/opt/homebrew/bin/extract-xiso",
            "/usr/local/bin/extract-xiso",
            "/usr/bin/extract-xiso",
        ].map { URL(fileURLWithPath: $0) }
        return candidates.first { FileManager.default.isExecutableFile(atPath: $0.path) }
    }
}

// MARK: - the game

enum Game {
    /// The recompiled binary, beside this launcher inside the bundle.
    static var binary: URL {
        Bundle.main.bundleURL
            .appendingPathComponent("Contents/MacOS/jsrf_recomp")
    }

    /// The flags the port needs. Kept here rather than in a shell script so
    /// there is one place they are true, and overridable from `defaults` for
    /// anyone who wants to experiment without a rebuild:
    ///   defaults write com.jumatt.jsrf-unleashed env -array RECOMP_GL_SCALE=2
    static func environment(gameDir: URL) -> [String: String] {
        var env = ProcessInfo.processInfo.environment
        let base = [
            "RECOMP_USB": "1",
            "RECOMP_GL": "1",
            "RECOMP_PB_EXEC": "1",
            "RECOMP_VBLANK": "1",
            "RECOMP_AC97_READY_NOTRAP": "1",
            "RECOMP_TRACE_BUDGET": "0",
            "RECOMP_GL_READBACK": "0",
            "RECOMP_FPS": "60",
            "RECOMP_ABI_RESTORE": "1",
            // CRI's ADX idle thread runs at idle priority and does real file
            // and decode work; outside the guest lock it raced the threads
            // CRI assumes it cannot preempt, and the music stream stalled
            // (and could corrupt CRI's state). Idle threads take the lock.
            "RECOMP_LOCK_IDLE": "1",
            // WORKAROUND, not a fix. One full-screen black quad -- four
            // vertices covering the viewport, no texture, diffuse RGB
            // zero, alpha around 0.9, alpha-blended -- is drawn over the
            // finished scene and takes the frame from a mean brightness
            // of 80 to 5, which is black to the eye. The HUD is drawn
            // after it and survives, which is exactly what the tutorial
            // looked like. Skipping it makes the level visible again.
            // The cost is that genuine fades between scenes now cut
            // rather than fade. Why the title asks for a near-opaque
            // curtain on a frame it means you to see is still open.
            "RECOMP_GL_CURTAIN": "skip",
            "RECOMP_MUTE": "0",
            // Without this the engine renders offscreen and opens nothing.
            // It is how the diagnostic runs work -- they dump frames to disk
            // and never want a window -- and copying that list into a launcher
            // produced an app that played the soundtrack to an empty screen.
            "RECOMP_WINDOW": "1",
        ]
        // Deliberately absent: RECOMP_GL_SCALE and the window size. The engine
        // reads the resolution from the Video menu's stored choice and sizes
        // its window to match; setting them here would silently overrule the
        // menu, which is the one setting a player can actually reach.

        for (k, v) in base { env[k] = v }
        for entry in UserDefaults.standard.stringArray(forKey: "env") ?? [] {
            let parts = entry.split(separator: "=", maxSplits: 1).map(String.init)
            if parts.count == 2 { env[parts[0]] = parts[1] }
        }
        env["JSRF_GAME_DIR"] = gameDir.path
        // Saves belong to the player, not to the disc they were made from:
        // the game folder may be read-only, may be a mounted image, and is
        // not somewhere anyone expects their progress to be kept.
        let saves = Library.root.appendingPathComponent("save")
        try? FileManager.default.createDirectory(at: saves, withIntermediateDirectories: true)
        env["JSRF_SAVE_DIR"] = saves.path
        return env
    }
}

// MARK: - app

final class Launcher: NSObject, NSApplicationDelegate {
    private var logURL: URL?

    func applicationDidFinishLaunching(_ note: Notification) {
        NSApp.setActivationPolicy(.regular)
        guard FileManager.default.isExecutableFile(atPath: Game.binary.path) else {
            fail("This app is missing the game engine.",
                 "Expected it at:\n\(Game.binary.path)\n\nRebuild the app with build_app.sh.")
            return
        }
        if let folder = GameFolder.remembered() {
            start(folder)
        } else {
            ask(reason: nil)
        }
    }

    // MARK: choosing

    private func ask(reason: String?) {
        if let reason = reason {
            let a = NSAlert()
            a.messageText = "Where is Jet Set Radio Future?"
            a.informativeText = reason
            a.addButton(withTitle: "Choose…")
            a.addButton(withTitle: "Quit")
            if a.runModal() != .alertFirstButtonReturn { NSApp.terminate(nil); return }
        }

        let panel = NSOpenPanel()
        panel.title = "Choose the game"
        panel.message = "Pick the folder of extracted game files, or a disc image (.iso)."
        panel.prompt = "Use This"
        panel.canChooseDirectories = true
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = false
        panel.allowedFileTypes = ["iso", "xiso"]
        panel.treatsFilePackagesAsDirectories = true

        guard panel.runModal() == .OK, let picked = panel.url else {
            NSApp.terminate(nil); return
        }

        var isDir: ObjCBool = false
        FileManager.default.fileExists(atPath: picked.path, isDirectory: &isDir)
        if isDir.boolValue {
            guard let folder = GameFolder.resolve(picked) else {
                ask(reason: "That folder has no default.xbe in it, so it isn't the game's files. "
                          + "Pick the folder the disc was extracted into.")
                return
            }
            GameFolder.remember(picked)
            start(folder)
        } else {
            extract(picked)
        }
    }

    private func extract(_ iso: URL) {
        let dest = Library.extractedFolder(for: iso)
        if let folder = GameFolder.resolve(dest) {   // unpacked on an earlier run
            GameFolder.remember(dest)
            start(folder)
            return
        }
        guard let tool = Library.extractTool() else {
            fail("That's a disc image, and the tool to unpack it isn't installed.",
                 "Install extract-xiso and try again:\n\n    brew install extract-xiso\n\n"
               + "Or unpack the image yourself and pick the resulting folder instead.")
            return
        }

        let progress = NSAlert()
        progress.messageText = "Unpacking \(iso.lastPathComponent)…"
        progress.informativeText = "This happens once. The files go to:\n\(dest.path)"
        let spinner = NSProgressIndicator(frame: NSRect(x: 0, y: 0, width: 300, height: 20))
        spinner.style = .bar
        spinner.isIndeterminate = true
        spinner.startAnimation(nil)
        progress.accessoryView = spinner
        progress.addButton(withTitle: "Cancel")

        try? FileManager.default.createDirectory(at: dest, withIntermediateDirectories: true)
        let p = Process()
        p.executableURL = tool
        p.arguments = ["-d", dest.path, "-x", iso.path]
        p.terminationHandler = { _ in
            DispatchQueue.main.async {
                NSApp.abortModal()
                if let folder = GameFolder.resolve(dest) {
                    GameFolder.remember(dest)
                    self.start(folder)
                } else {
                    try? FileManager.default.removeItem(at: dest)
                    self.fail("That image didn't unpack into a game.",
                              "No default.xbe was found in it. If it's a redump-style image with "
                            + "a video partition, extract it yourself and pick the folder.")
                }
            }
        }
        do { try p.run() } catch {
            fail("Couldn't run extract-xiso.", error.localizedDescription); return
        }
        progress.runModal()
        if p.isRunning { p.terminate(); NSApp.terminate(nil) }
    }

    // MARK: running

    /// Hand this process over to the engine.
    ///
    /// Not a subprocess. macOS decides whether a process may put a window on
    /// screen from the bundle it was launched as, and only the bundle's main
    /// executable qualifies -- anything else started out of Contents/MacOS is
    /// a helper tool, and its request to become a foreground application is
    /// refused without an error anyone can see. The engine then runs happily,
    /// plays its music, and draws nothing: sound has no such rule.
    ///
    /// exec replaces the image but keeps the process, so what carries on
    /// running IS the application the system launched, with its window rights
    /// intact. This is the usual shape of a wrapper that has a question to ask
    /// before the real program starts.
    private func start(_ folder: URL) {
        // Write the log where the game lives.
        //
        // logRoot prefers the folder beside the .app, which is right in
        // principle and silently produced nothing in practice -- no log ever
        // appeared there, so every crash still had to be described from
        // memory rather than read. The game folder is the one directory this
        // process is certain it can reach: it was just opened, its contents
        // were just listed, and the engine is about to chdir into it. Use it
        // first, keep the other two as fallbacks, and record which one won.
        let candidates = [folder, Library.logRoot, Library.root]
        let base = candidates.first {
            FileManager.default.isWritableFile(atPath: $0.path)
        } ?? Library.root
        let log = base.appendingPathComponent("last-run.log")
        logURL = log

        // Keep the previous run's log instead of truncating it.
        //
        // The engine's diagnosis of a crash is written here, and the next
        // launch used to open the same path with "w" -- so the ordinary
        // reaction to a crash, launching again, destroyed the only record of
        // it. That happened twice in one afternoon: a crash reported, the app
        // relaunched, and by the time anyone went looking the backtrace, the
        // register dump and the guest call chain were gone.
        //
        // Each run's log is moved aside under a timestamp before the new one
        // opens, and the five most recent are kept: they live in the player's
        // game folder, and a run's log can reach a few tens of MB.
        let fm = FileManager.default
        let history = base.appendingPathComponent("logs")
        try? fm.createDirectory(at: history, withIntermediateDirectories: true)
        if fm.fileExists(atPath: log.path) {
            let stamp = DateFormatter()
            stamp.dateFormat = "yyyyMMdd-HHmmss"
            let name = "run-" + stamp.string(from: Date()) + ".log"
            try? fm.moveItem(at: log, to: history.appendingPathComponent(name))
        }
        if let kept = try? fm.contentsOfDirectory(atPath: history.path)
                              .filter({ $0.hasPrefix("run-") }).sorted(),
           kept.count > 5 {
            for old in kept.prefix(kept.count - 5) {
                try? fm.removeItem(at: history.appendingPathComponent(old))
            }
        }

        for (k, v) in Game.environment(gameDir: folder) { setenv(k, v, 1) }
        FileManager.default.changeCurrentDirectoryPath(folder.path)

        // The engine writes its diagnosis to stdout and stderr; after exec
        // there is nobody left to collect them, so point them at the log now.
        freopen(log.path, "w", stdout)
        freopen(log.path, "a", stderr)   // append: "w" twice gives two writers
                                         // at offset zero, interleaving the
                                         // start of the log into nonsense

        let exe = Game.binary.path
        var argv: [UnsafeMutablePointer<CChar>?] =
            [strdup(exe), strdup(folder.path), nil]
        execv(exe, &argv)

        // Only reached if exec failed, which leaves this process intact.
        let err = String(cString: strerror(errno))
        fail("The game wouldn't start.", "\(exe)\n\n\(err)")
    }

    private func fail(_ message: String, _ detail: String) {
        NSApp.activate(ignoringOtherApps: true)
        let a = NSAlert()
        a.alertStyle = .warning
        a.messageText = message
        a.informativeText = detail
        a.addButton(withTitle: "Choose Game…")
        a.addButton(withTitle: "Quit")
        if a.runModal() == .alertFirstButtonReturn {
            GameFolder.forget()
            ask(reason: nil)
        } else {
            NSApp.terminate(nil)
        }
    }

}

let app = NSApplication.shared
let delegate = Launcher()
app.delegate = delegate
app.run()

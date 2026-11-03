Pulsar — Installation Instructions
==================================

Pulsar runs as a 64-bit VST3 plugin and as a Standalone application on
Windows, macOS, and Linux.


1. Windows
----------
- VST3: Copy `Pulsar.vst3` to:
    C:\Program Files\Common Files\VST3\
  (or %CommonProgramFiles%\VST3\)
- Standalone: Run `Pulsar.exe` directly from any folder, or copy it to
  your preferred applications directory.


2. macOS (Apple Silicon & Intel)
--------------------------------
Pulsar is a Universal binary supporting both Apple Silicon (arm64) and
Intel (x86_64).

- VST3: Copy `Pulsar.vst3` to:
    ~/Library/Audio/Plug-Ins/VST3/
  (or /Library/Audio/Plug-Ins/VST3/ for all users)
- Standalone: Copy `Pulsar.app` to:
    /Applications/

Gatekeeper instructions for unsigned builds:
Because this is an unsigned community build, macOS Gatekeeper may block it
on first launch. To permit it:

Method A (GUI):
1. In Finder, navigate to the installed plugin or app.
2. Right-click (or Control-click) `Pulsar.app` or `Pulsar.vst3` and choose "Open".
3. Click "Open" in the security prompt.
   Alternatively, go to System Settings → Privacy & Security, scroll down,
   and click "Open Anyway".

Method B (Terminal):
Run the following commands in Terminal to remove the quarantine flag:
  xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/Pulsar.vst3
  xattr -dr com.apple.quarantine /Applications/Pulsar.app


3. Linux (x86_64)
-----------------
- VST3: Copy `Pulsar.vst3` to:
    ~/.vst3/
  (or /usr/lib/vst3/ for system-wide installation)
- Standalone: Run `Pulsar` directly from terminal or your app launcher.
  Make sure it has execute permissions:
    chmod +x Pulsar


First Run & API Key Setup
-------------------------
1. Pulsar uses the Meta AI API to generate and mutate patches in real time.
2. Obtain an API key from the Meta developer portal (api.meta.ai).
3. Open Pulsar, click the Settings gear icon in the bottom-right corner,
   paste your key into the API key field, and click "Test key".
4. Alternatively, export the environment variable before launching your DAW:
    export VIBESYNTH_API_KEY="your-key-here"
   Your key is stored only locally on your machine and is never shared.

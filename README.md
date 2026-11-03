# Pulsar

An AI-native synthesiser plugin that writes, validates, and hot-swaps native sample-rate DSP code on the fly from natural language prompts.

Runs as a 64-bit **VST3** plugin and **Standalone** application on Windows, macOS, and Linux.

---

## What It Is

Pulsar is an expressive synthesiser for electronic musicians and sound designers. Instead of selecting static wavetables or preset algorithms, you describe the sound you want in plain English (e.g. *"huge analog reese into a ladder filter with warm saturation and sub pressure"*). 

The Meta API writes per-sample digital signal processing (DSP) code tailored to your prompt. Pulsar compiles this program to native machine instructions, safety-tests it offline across a multi-note stress matrix, sets its output trim, and hot-swaps it into the realtime audio engine while you play, with a short fade and no memory allocation on the audio thread.

Key capabilities:
- **Natural Language Sound Generation:** Generates original per-sample DSP programs with up to 8 named macro knobs, directly from prompts.
- **Visual Signal Flow & Node Inspector:** Automatically infers signal graphs into an interactive node visualizer. Click any node to inspect parameters, audition live oscilloscopes/frequency response curves, bypass components, remap parameters to macros, or set macro ranges.
- **"Change This Node":** Targeted localized sound editing. Select an individual filter, oscillator, or effect and prompt targeted modifications while leaving the rest of the synthesis chain untouched.
- **Mutate & Audition Strip:** Generate four variations of the current sound at once (Darker, Movement, Brighter, Weirder). Audition each variation instantly with seamless audio swaps, and Keep your favourite or Discard back to the original.
- **Full Undo History:** Instantly step back to previous sound states and macro configurations.
- **Modulation (MOD) Strip:** Two tempo-synced/free LFOs (sine, triangle, saw+, saw-, square, sample & hold, random) and two ADSR envelopes with drag-to-assign modulation to any macro or node argument, visual depth rings, and popover editors.
- **Built-in & User Presets:** 15 factory showcase patches, custom `.pulsar` patch export/import, and generation history.
- **Transparent Code Viewer:** Inspect the generated sample-rate code and macro definitions in a read-only viewer.

---

## Built During the Hackathon

> **Hackathon Integrity Statement:**  
> Everything in this repository except the third-party libraries listed below was written between **October 29 and November 4, 2026**. The repository's initial commit is dated `2026-10-29` (Milestone 1), and the public git commit history documents every single step of design, implementation, and testing. All presets, prompts, and recipes are 100% original.

### Third-Party Components

| Component | Licence | Usage in Pulsar | Upstream Link |
| :--- | :--- | :--- | :--- |
| **JUCE 8** | GNU AGPLv3 (or commercial) | Audio plugin framework, Standalone host, GUI controls, audio graph | [github.com/juce-framework/JUCE](https://github.com/juce-framework/JUCE) |
| **WDL / EEL2** | zlib Licence | Expression Evaluator Library 2 native JIT compiler & bytecode engine | [github.com/justinfrankel/WDL](https://github.com/justinfrankel/WDL) |
| **ysfx GAS Assembly** | zlib Licence | x86_64 assembly glue for EEL2 JIT on Linux and macOS | [github.com/jpcima/ysfx](https://github.com/jpcima/ysfx) |
| **DaisySP** | MIT Licence | Core synthesis primitives (`daisysp_mit`: oscillators, ADSR, decimator, chorus, resonator, Karplus-Strong) | [github.com/electro-smith/DaisySP](https://github.com/electro-smith/DaisySP) |
| **CPM.cmake** | MIT Licence | CMake dependency manager | [github.com/cpm-cmake/CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) |
| **Inter Font** | SIL Open Font License 1.1 | Application UI typography (Regular, Medium, SemiBold) | [github.com/rsms/inter](https://github.com/rsms/inter) |
| **JetBrains Mono** | SIL Open Font License 1.1 | DSP code viewer and numeric display typography | [github.com/JetBrains/JetBrainsMono](https://github.com/JetBrains/JetBrainsMono) |

*Full license texts for all components are provided in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).*

---

## Installation

Pre-built binaries for all platforms are available on the [GitHub Releases](https://github.com/georgethetaco44-bit/Pulsar-VST/releases) page.

### Windows (x64)
1. Download `Pulsar-<version>-Windows.zip`.
2. Extract the archive:
   - **VST3:** Copy `Pulsar.vst3` to your VST3 directory (typically `C:\Program Files\Common Files\VST3\`).
   - **Standalone:** Run `Pulsar.exe` from any folder.

### macOS (Universal: Apple Silicon arm64 & Intel x86_64)
1. Download `Pulsar-<version>-macOS.zip`.
2. Extract the archive:
   - **VST3:** Copy `Pulsar.vst3` to `~/Library/Audio/Plug-Ins/VST3/` (or `/Library/Audio/Plug-Ins/VST3/`).
   - **Standalone:** Copy `Pulsar.app` to `/Applications/`.

#### macOS Gatekeeper Notice (Unsigned Community Build)
Because release binaries are ad-hoc signed for Apple Silicon compatibility rather than signed with an Apple Developer ID certificate, macOS Gatekeeper may present a security prompt on initial launch.

- **Option A (Finder):** Right-click (or Control-click) `Pulsar.app` or `Pulsar.vst3`, select **Open**, and confirm **Open** in the dialog. Alternatively, open **System Settings → Privacy & Security**, scroll to the security section, and click **Open Anyway**.
- **Option B (Terminal):** Clear the quarantine attribute using `xattr`:
  ```bash
  xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/Pulsar.vst3
  xattr -dr com.apple.quarantine /Applications/Pulsar.app
  ```

### Linux (x86_64)
1. Download `Pulsar-<version>-Linux.zip`.
2. Extract the archive:
   - **VST3:** Copy `Pulsar.vst3` to `~/.vst3/` (or `/usr/lib/vst3/`).
   - **Standalone:** Run `Pulsar` directly. Ensure it has executable permissions (`chmod +x Pulsar`).

---

## First Run & API Key Setup

Pulsar generates new DSP architectures via the Meta AI API (`api.meta.ai`).

1. Obtain an API key from the Meta developer portal.
2. Launch Pulsar in your DAW or as Standalone.
3. Click the **Settings** gear at the right end of the tab row (next to the level meter).
4. Paste your API key into the field and click **Test key**.
5. Once verified, click **Save**.

*Environment Variable Alternative:* You can also set the key in your shell environment:
```bash
export VIBESYNTH_API_KEY="your-api-key-here"
```

> **Security Note:** Your API key is stored only on your local machine (`~/.config/Pulsar/Pulsar.settings` on Linux, `~/Library/Application Support/Pulsar/Pulsar.settings` on macOS, or `%APPDATA%\Pulsar\Pulsar.settings` on Windows). It is never written to project files, DAW session states, `.pulsar` presets, or logs.

---

## Using Pulsar

### 1. Generate
Type a description of your sound into the prompt bar at the top (e.g. *"gritty neuro bass with a morphing vowel filter"*) and press **Enter** (or click **Generate**). The status indicator displays the compilation and validation stages in real time.

### 2. Macros
Each patch defines up to 8 named macro controls tailored to its architecture. Turn knobs to shape the sound. Double-click any knob to reset it to its patch default.

### 3. Node View & Inspector
The center view visualizes the live audio processing graph, distinguishing between per-voice processing (**VOICE**) and master bus processing (**MASTER**). Nodes feature live activity meters.
- **Click any node:** Opens the **Node Inspector** below the signal graph.
- **Tweak arguments:** Adjust cutoff frequencies, resonance, drive, or envelope timings in real time.
- **Bypass:** Click **Bypass** to cleanly toggle individual processing nodes in and out of the signal path.
- **Map to macro:** Right-click an argument knob and choose **Map to macro** to drive that parameter from any macro.
- **Set range:** Right-click a macro knob to open **Set range…**. A popover lets you constrain the minimum and maximum parameter range sent to the synthesis code.

### 4. "Change This Node"
Want to swap a standard lowpass for a vowel formant filter or change distortion characteristics without touching the rest of your sound?
1. Click the node you wish to modify to open its Inspector.
2. Type your instruction into the **Change this node…** box (e.g. *"make this filter sweep down quickly with high resonance"*).
3. Press **Enter**. Pulsar asks the model to rewrite only that stage of the DSP graph, preserving your modulation mappings, macros, and other nodes.

### 5. Mutate & Audition Strip
Click the **Mutate** button next to Generate:
1. Pulsar requests four variations at once, one per direction: **Darker**, **Movement**, **Brighter** and **Weirder**.
2. The **Audition Strip** appears with your original and the four variations. Click any card to audition it live while you play.
3. Click **Keep** to make the playing variation your sound, or **Discard** to go back to the original.

### 6. Undo
Click **Undo** at any time to revert back through previous patch iterations, generated sounds, mutations, or preset loads.

### 7. MOD Strip
The **MOD** strip sits directly below the macros:
- **Two LFOs (LFO 1, LFO 2):** Support Sine, Triangle, Saw Up, Saw Down, Square, Sample & Hold, and Random waveforms. Toggle between tempo-synchronised divisions (1/32 to 4 bars, including triplets and dotted notes) or free rates (0.05 Hz to 30 Hz), with optional per-voice retriggering.
- **Two Envelopes (ENV 1, ENV 2):** Fast, exponential ADSR envelopes with visual stage sliders and live tracking.
- **Drag-to-Assign:** Click and drag any modulator tile directly onto any macro knob or inspector parameter to create an assignment. Adjust depth via the outer ring. Click a tile to open its settings popover.

### 8. Presets & Files
- **Presets Tab:** Browse 15 built-in showcase presets.
- **Save / Open:** Save custom sounds as human-readable `.pulsar` JSON files or open previously exported patches.
- **History Tab:** Automatically keeps a record of every successful generation, allowing you to reload previous sounds.

### 9. Code Tab
Click the **Code** tab to inspect the generated DSP code, init routine, and macro specifications.

---

## How It Works

Pulsar couples a high-level language model with a hardened, low-level DSP execution runtime.

```
┌─────────────────┐
│ User Prompt /   │
│ Node "Change"   │
└────────┬────────┘
         │
         ▼
┌─────────────────┐     HTTPS      ┌──────────────────────────────┐
│ Prompt Builder  ├───────────────▶│ Meta API                    │
│ & Recipes       │◀───────────────┤ (EEL2 sample-rate DSP code)  │
└─────────────────┘                └──────────────────────────────┘
         │
         ▼
┌─────────────────┐
│ Code Sanitiser  │  function-name and argument-count lint, slot limits,
│ & Lexicon Lint  │  loop caps (max 1024), bounded memory access
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ EEL2 JIT Engine │  Offline compilation to native machine code (x86_64 / arm64)
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ Offline Safety  │  3 notes x 3 macro settings, 1 s each: NaN/Inf bit checks,
│ & Matrix Test   │  peak, DC and silence checks, CPU guard, automatic trim
└────────┬────────┘
         │
         ▼ (Atomic Pointer Swap)
┌────────────────────────────────────────────────────────┐
│ Audio Thread (Realtime DSP)                            │
│ - Zero memory allocations                              │
│ - Lock-free SPSC queues & atomics only                 │
│ - 5 ms fade out, swap, 5 ms fade in; held notes carry  │
└────────────────────────────────────────────────────────┘
```

---

## Building from Source

### Prerequisites
- **CMake** >= 3.25
- **Ninja** or Make
- **C++20** compatible compiler (GCC >= 11, Clang >= 14, or MSVC 2022)
- **Linux dependencies:** ALSA, FreeType, Fontconfig, X11 development headers (Xrandr, Xinerama, Xcursor, Xcomposite), libcurl (`libcurl4-openssl-dev`).

### Build Instructions
Clone the repository:
```bash
git clone https://github.com/georgethetaco44-bit/Pulsar-VST.git
cd Pulsar-VST
```

#### Linux
```bash
cmake --preset linux-release
cmake --build build/linux-release --parallel
```

#### macOS (Universal Binary)
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"
cmake --build build --config Release --parallel
```

#### Windows (MSVC x64)
```cmd
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

### Running Developer Verification
Pulsar includes an automated test and verification suite:
```bash
tools/check.sh build    # Builds targets
tools/check.sh smoke    # Runs smoke tests (JIT, loop caps, patch parsing)
tools/check.sh strict   # Runs full test suite + strict CPU benchmarking
```

---

## Licence

Pulsar is released under the **GNU Affero General Public License v3** (AGPL-3.0). See [LICENSE](LICENSE) for details.

Third-party dependencies and fonts remain under their respective permissive licences (MIT, zlib, SIL Open Font License 1.1). See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for full notices and licence texts.

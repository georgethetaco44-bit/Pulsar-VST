# Pulsar — Demo Video Script (3 Minutes)

**Total Duration:** 3:00  
**Format:** Screen recording of Pulsar running in a DAW / Standalone with live audio, voiceover narration, and cursor action.  
**Scope:** Only features conceived and built during the hackathon (Oct 29 – Nov 4, 2026).

---

### Shot List & Narration Timeline

#### 0:00 – 0:25 | 1. Introduction & Natural Language Generation (25s)
- **Visual:** Standalone or DAW window open with the clean Pulsar interface. The prompt field is focused.
- **Action:** Type into the prompt bar:  
  `huge analog reese into a ladder filter with warm saturation and sub pressure`  
  Press **Enter**.
- **Visual Details:** Status line pulses cyan: `"Waiting for model"`, advances through parsing, offline safety testing, and compiles. The audio crossfades smoothly into the new patch as a sustained MIDI bass note plays.
- **Voiceover:**  
  *"This is Pulsar—an AI synthesiser that doesn't just load presets or tweak pre-built wavetables. When you describe a sound, it writes real-time sample-rate DSP code on the fly. We type 'huge analog reese into a ladder filter', hit Enter, and in seconds, the Meta API generates native code, tests it offline for safety, and hot-swaps it in seamlessly while we play."*

---

#### 0:25 – 0:50 | 2. The Live Node View (25s)
- **Visual:** Camera zooms slightly into the center signal graph.
- **Action:** Play a riff on the keyboard. Show the split between **VOICE** processing on the left and **MASTER** bus processing on the right.
- **Visual Details:** Oscillators (`osc_saw`, `osc_supersaw`), filter (`flt_ladder`), and shapers (`fx_sat`, `bus_sat`, `bus_mono_low`) display live pulsing waveforms and signal flow lines.
- **Voiceover:**  
  *"Pulsar parses the generated program into an interactive node graph. There's no guesswork about what's inside: you can see every voice oscillator, the ladder filter, and the master bus saturation, complete with live signal activity meters on every node."*

---

#### 0:50 – 1:20 | 3. The Node Inspector: Tweak, Bypass, Macro Map & Set Range (30s)
- **Visual:** Cursor moves down to the bottom panel.
- **Action:**
  1. Click on the `flt_ladder:0` node pill in the graph. The **Node Inspector** opens below, revealing its frequency response curve and Cutoff / Resonance knobs.
  2. Tweak the **Cutoff** knob live while playing a note to hear the filter sweep.
  3. Click **Bypass** to instantly cut the filter out, then click again to re-engage it.
  4. Right-click the **Resonance** knob → select **Map to macro** → assign to **Macro 3**.
  5. Right-click Macro 3 in the macro panel → choose **Set range…**. A small popover appears. Adjust the min and max sliders to constrain the sweep range (e.g. `0.20 – 0.75`).
- **Voiceover:**  
  *"Clicking any node brings up the Node Inspector. Here we have live frequency response curves and parameter controls. We can adjust cutoff, instantly bypass the module, map parameters directly to any of the 8 macro knobs, or open the range popover to constrain exactly how far that macro can travel."*

---

#### 1:20 – 1:45 | 4. "Change This Node" (25s)
- **Visual:** Focus remains on the Inspector's localized change input.
- **Action:**
  1. In the Inspector for `flt_ladder:0`, focus the **Change this node…** field.
  2. Type: `swap this ladder for a vocal formant vowel filter that screams on high notes`
  3. Press **Enter**.
  4. Status displays `"Changing flt_ladder:0: waiting for model"`. The rest of the patch remains uninterrupted. The node updates to `flt_formant`, and playing notes reveals distinct vowel formant movement.
- **Voiceover:**  
  *"Pulsar lets you perform surgical modifications. With 'Change this node', we ask the model to replace our ladder filter with a vocal formant vowel filter. Pulsar rewrites only that specific component, preserving our oscillators, saturation, and modulation untouched."*

---

#### 1:45 – 2:15 | 5. Mutate & 4-Way Audition Strip (30s)
- **Visual:** Top bar near the prompt input.
- **Action:**
  1. Click the **Mutate** button.
  2. The **Audition Strip** appears: the original plus four variations, *Darker*, *Movement*, *Brighter* and *Weirder*.
  3. Play repeated notes while clicking between the cards. Each click swaps the sound with a short fade.
  4. Click the card you like best, then **Keep**.
- **Voiceover:**  
  *"When you want inspiration, hit Mutate. Pulsar immediately generates four parallel musical variations. With the audition strip, you can jump between them in real time while continuing to play your keyboard. Find the one you like, click Keep, and it becomes your new active sound."*

---

#### 2:15 – 2:30 | 6. Instant Undo (15s)
- **Visual:** Top right near the Generate button.
- **Action:**
  1. Click **Undo**.
  2. The synth instantly snaps back to the state before the mutation. The node view, macro values, and parameter ranges restore bit-for-bit.
  3. Play a chord to confirm the sound has reverted.
- **Voiceover:**  
  *"Everything in Pulsar is non-destructive. One click on Undo restores the exact previous DSP program, macro positions, and parameter overrides."*

---

#### 2:30 – 2:45 | 7. The MOD Strip (15s)
- **Visual:** The **MOD** strip directly underneath the macros.
- **Action:**
  1. Show LFO 1, LFO 2, ENV 1, and ENV 2 with live animated playheads.
  2. Drag the **LFO 1** tile directly onto the **Drive** macro knob. A cyan modulation ring appears around the knob.
  3. Click LFO 1's tile to open its settings popover. Toggle shape to **Saw+** and sync division to **1/4**.
- **Voiceover:**  
  *"The dedicated MOD strip gives you two tempo-synced or free LFOs and two ADSR envelopes. Simply drag any modulator tile onto a macro or parameter knob to assign it instantly, with visual modulation depth rings and popover editors."*

---

#### 2:45 – 2:50 | 8. Built-in Preset Showcase (5s)
- **Visual:** Click **Presets** tab.
- **Action:** Select `Scalpel Neuro` from the showcase list and play a brief aggressive bass riff.
- **Voiceover:**  
  *"Pulsar also includes 15 original built-in presets covering modern bass design and expressive synthesis."*

---

#### 2:50 – 3:00 | 9. Hackathon Summary Card (10s)
- **Visual:** Fullscreen summary card matching the README hackathon statement.
  ```
  PULSAR — BUILT FOR THE HACKATHON
  Dates: October 29 – November 4, 2026
  Initial Commit: 2026-10-29 (Milestone 1)

  Built from Scratch:
  • Natural-Language Prompt-to-DSP Generator
  • EEL2 JIT Integration & Safety Validator
  • Realtime Lock-Free Audio Engine (VST3 & Standalone)
  • Dynamic Node Graph Visualizer & Inspector
  • "Change This Node" Localized DSP Rewriter
  • 4-Way Audition & Mutate System
  • MOD Strip (LFOs & Envelopes with Drag-to-Assign)
  • 15 Original Factory Presets & Macro Systems

  Open Source: github.com/georgethetaco44-bit/Pulsar-VST (AGPLv3)
  ```
- **Voiceover:**  
  *"Every line of Pulsar's synthesiser engine, UI, and test infrastructure was built from scratch during this hackathon between October 29th and November 4th, 2026. Download the VST3 or Standalone on GitHub, and start designing sound with AI."*

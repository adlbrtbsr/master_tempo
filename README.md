## MasterTempo

Real-time tempo and beat tracker for Windows built with JUCE 8 and CMake. Captures system audio via WASAPI loopback, estimates tempo, tracks beats, and can output OSC/MIDI.

### Features
- Windows WASAPI loopback capture to analyze system output
- JUCE DSP processing pipeline with band-limited onset detection
- Tempo estimation and beat tracking
- Live UI displaying status, BPM, and beat pulses
- OSC sender for streaming tempo/beat data
- MIDI output: configurable channel, CC for tempo, and beat note

### Requirements
- Windows 10+
- CMake 3.20+
- Visual Studio 2022 (C++ Desktop workload) or compatible MSVC toolchain
- Git (to fetch JUCE via FetchContent)

### Building
This project uses CMake and fetches JUCE at configure time (tag 8.0.3).

```bash
# from the repo root
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The built executable will be at:
`build/master_tempo_artefacts/Release/MasterTempo.exe`

### Running
Launch `MasterTempo.exe`. On first run:
1. Choose whether to use WASAPI loopback or standard device input.
2. If using loopback, select your output device (e.g., Speakers/Głośniki) and apply.
3. Optionally select a MIDI output device and connect.
4. The UI shows detected BPM and beat pulses in real time.

### OSC / MIDI
- OSC: Uses `juce::OSCSender`. Configure target host/port in code (see `src/MainComponent.*`).
- MIDI: Sends a CC for tempo and a note for beat pulses. Defaults: channel 1, CC 20, note 60 (C4). Adjust in `MainComponent`.

### Code Structure
- `CMakeLists.txt` — CMake project; fetches JUCE and defines the GUI app target
- `src/Main.cpp` — JUCE app entry
- `src/MainComponent.h/.cpp` — UI, audio callback, DSP pipeline, OSC/MIDI
- `src/dsp/*` — onset detection, tempo estimation, beat tracking
- `src/win/WASAPILoopback.h` — Windows-only loopback capture utility

### Notes
- Loopback capture requires shared-mode format; the implementation matches the render device mix format.
- JUCE web/cURL are disabled for a smaller binary.

### Development
Open the generated Visual Studio solution if desired:
`build/master_tempo.sln`

Common CMake configs: `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel`.

### License
This project uses JUCE (BSD-3-Clause). See JUCE's license and this repository's license if present.



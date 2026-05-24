# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

QuietZone (ANCApp) — Android spatial Active Noise Cancellation app. Uses phone mic + speaker to generate anti-noise for open-space noise reduction (not headphone-based). Package: `com.anc.app`.

## Build Commands

```bash
# Android APK
./gradlew assembleDebug          # Debug build
./gradlew assembleRelease        # Release build (ProGuard minified)

# Desktop C++ unit tests & benchmarks (requires cmake + make)
cd tests && mkdir -p build && cd build && cmake .. && make
./anc_unit_test
./anc_benchmark

# Python FxLMS simulation
python3 fxlms_simulation.py
```

## Architecture

### Kotlin/Compose Layer (`app/src/main/java/com/anc/app/`)
- `engine/ANCEngine.kt` — JNI bridge to native `anc_engine` library. Defines `ANCConfig`, `ANCStats` data classes and all `external fun` declarations.
- `engine/ANCService.kt` — Foreground service keeping ANC alive in background. Handles START/STOP/UPDATE_CONFIG intents.
- `engine/ANCViewModel.kt` — State machine: `Idle → RequestingPermission → Calibrating → Converging → Running → Error`. Polls native stats at 10Hz via `StateFlow<ANCUIState>`.
- `audio/AudioStreamManager.kt` — Thin wrapper around ANCEngine; auto-detects output device and enables external speaker mode for wired/USB.
- `ui/ANCScreen.kt` — Main Compose screen with all UI components.
- `ui/components/ANCComponents.kt` — UI components (MainControlButton, StatsPanel, SpectrumView, ModeSelector, ParameterPanel). Dark theme via `ANCColors` object.

### C++ Native Layer (`app/src/main/cpp/`)
- `anc_engine.h` — Core header. All algorithm classes in `anc` namespace: `FxLMSFilter` (Leaky FxLMS, NEON SIMD 8-way unrolled), `SecondaryPathEstimator` (offline/online estimation), `SpectrumAnalyzer` (Radix-2 FFT), `AudioProcessor` (orchestrates FF/FB/Hybrid modes).
- `oboe_engine.h` / `oboe_callback.cpp` — `OboeEngine` implements `oboe::AudioStreamCallback`. Separate input/output streams (LowLatency + Exclusive). Lock-free `RingBuffer<float>` passes mic→output. Zero JNI overhead in audio path.
- `ring_buffer.h` — Lock-free SPSC ring buffer. Power-of-2 sizing, acquire/release memory ordering, cache-line aligned atomics.
- `anc_engine.cpp` — JNI glue. Global `OboeEngine*` instance. Functions: init, start, stop, enable, setStepSize, setMode, setOutputGain, setExternalSpeaker, calibrate, getStats, getSpectrum, reset, release.

### Data Flow
```
Mic → Oboe InputStream → RingBuffer → Oboe OutputStream → AudioProcessor::processFrame() → Speaker
                                                        ├→ Stats (atomic) → JNI → ViewModel (10Hz) → Compose UI
                                                        └→ Spectrum (mutex) → JNI → ViewModel → Canvas
```

## Key Technical Constraints

- C++ compiled with `-O3 -ffast-math -fomit-frame-pointer`, NEON SIMD enabled
- Target ABIs: `arm64-v8a`, `armeabi-v7a`
- Oboe 1.8.0 for low-latency audio (AAudio backend, OpenSL ES fallback)
- Audio processing runs entirely in C++ Oboe callbacks — no per-sample JNI calls
- Real-time budget: 128 samples @ 48kHz = 2666.7 µs per frame
- Lock-free atomics for stats cross-thread reads; mutex only for spectrum data

## Testing Notes

- Desktop C++ tests in `tests/` can run without Android device or emulator
- 9 unit tests cover FxLMS convergence, secondary path estimation, all 3 ANC modes, weight clamping, reset
- Benchmarks measure per-sample latency against real-time budget
- Android-side tests not yet implemented (planned for Phase 5)

## Language

Project documentation is in Chinese. Code comments and identifiers are in English.

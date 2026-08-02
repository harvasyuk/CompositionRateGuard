# CompositionRateGuard

This project exists because, somehow, multi-trillion-dollar companies such as Microsoft and NVIDIA still cannot reliably provide smooth 23.976 fps playback on current hardware and drivers.

After several minutes of playback, madVR starts reporting composition rate of **23.000 Hz** instead of **23.976 Hz**. With **Hardware-Accelerated GPU Scheduling (HAGS)** enabled, this results in endless stuttering.

Disabling HAGS avoids the stuttering, even though the composition rate still falls to 23.000 Hz. That is not an ideal solution: buying a current RTX card only to disable one of its Windows scheduling features feels slightly unreasonable.

CompositionRateGuard is an experimental native Windows workaround that tries to keep the DWM composition rate near 23.976 Hz or 47.952 Hz while MPC-HC is active in borderless fullscreen.

This software was vibe-coded, but at least it tries to solve the actual problem without launching another Chromium instance just to display a few buttons and toggles.

## Guard modes

The only UI you will see, after starting the app, is the tray menu. Use it to select one of the following modes:

- **Near-invisible 1 x 1 (default):** continuously redraws one pixel in the bottom-right corner of the MPC-HC monitor with alpha 1/255, the lowest nonzero opacity.
- **Visible test rectangle:** redraws a fully opaque 200 x 40 gradient, making continuous DWM updates easy to verify.

In both modes, the surface is repainted every 16 ms with changing content and synchronized with DWM. It is topmost, click-through, non-activating, absent from Alt+Tab, and active only while MPC-HC is in borderless fullscreen.

Fully transparent surfaces can be optimized away by the Windows graphics stack. The default therefore uses the smallest possible nonzero alpha on a single pixel. The visible mode proves that detection, placement, and continuous rendering are working, but it does not by itself prove that the near-invisible surface prevents the composition-rate fallback.

## Start with Windows

Enable **Start with Windows** in the tray menu to launch the guard automatically when the current user signs in. Disable the same menu item to remove the startup registration.

Move the executable to its permanent location before enabling this option. The startup registration points to the executable's current path.

## Open videos using this app

You can make the guard the Windows default app for selected video extensions using **Open with > Choose another app** and browsing to `CompositionRateGuard.exe`. When Windows passes a video file to the guard, it starts MPC-HC with that file.

The first time, the guard looks for a standard MPC-HC installation. If it cannot find one, it asks you to locate `mpc-hc*.exe` and remembers that executable in `guard.ini`. To change it later, close the guard and edit or remove the `Executable` value in the `[Player]` section.

Only one guard instance runs. Opening more videos forwards them to that instance, so MPC-HC can apply its own single/multiple-player preference. Once all MPC-HC processes have closed, an instance that has opened a video exits automatically. Launching the guard directly with no file keeps the normal tray utility running.

## Build

### Requirements

- Visual Studio with the **Desktop development with C++** workload
- Windows Windows 11 SDK

### Visual Studio

1. Open `CompositionRateGuard.slnx`.
2. Select the `x64` platform.
3. Select either the `Debug` or `Release` configuration.
4. Choose **Build > Build Solution**.

The executable is written to:

```text
x64\Release\CompositionRateGuard.exe
```

The project uses C++17 and links the static MSVC runtime.

## Test

1. Enable HAGS and restart Windows.
2. Start `CompositionRateGuard.exe`.
3. Leave **Near-invisible 1 x 1 (default)** selected.
4. Start MPC-HC and enter borderless fullscreen.
5. Reproduce the condition that normally causes the composition-rate fallback.
6. Check if the composition rate remains near 23.976 Hz (or 47.952 Hz) and never falls to 23.000 Hz (or 47.000 Hz).

The sole success criterion is preventing the composition-rate fallback. Any effect on visible stuttering is secondary.

## Files

Settings and logs are stored in:

```text
%LOCALAPPDATA%\CompositionRateGuard\
```

## Source layout

- `main.cpp` contains only the Windows entry point.
- `application.cpp` coordinates lifecycle, timers, the tray menu, and single-instance handoff.
- `settings.cpp` owns INI persistence and Start with Windows registration.
- `mpc_player.cpp` finds, launches, and detects MPC-HC.
- `overlay.cpp` owns the composition surface and its window procedure.
- `logging.cpp` owns timestamped file logging.
- `app_types.h` and `app_constants.h` contain the shared types and constants.

## Limitations

This is an experimental workaround. DWM, MPO, display drivers, HDR mode, and display topology can all affect whether the overlay changes scheduling behavior.

# Plants vs. Zombies Ultimate — Nintendo Switch port (NativeAOT wrapper)
 
This is a native wrapper / loader that runs the original ARM64 build of Plants vs. Zombies Ultimate on Switch homebrew. It contains no game code and no game assets. It loads the game's own native library (`libLawn.Android.so`) and recreates, natively, the Android/JNI layer the engine expects — a fake JNIEnv/JavaVM, a GLES2 context, an audio device, input, and the Java-side classes the game calls into. The game is a .NET NativeAOT build, so the wrapper also stands in for the parts of Java.Interop that would normally register the game's native methods.
English and Simplified Chinese languages are supported.

## Install & run
 
You need files from `pvz_ultimate_2_1_2_Android` (game version **2.1.2**).
 
Copy the `.nro` to your SD card, then place your game files next to it, in the same folder:
 
```
sdmc:/switch/pvzultimate
├── pvzultimate_nx.nro
├── libLawn.Android.so        <- from your APK: lib/arm64-v8a/
├── libc++_shared.so          <- from your APK: lib/arm64-v8a/
├── libopenmpt.so             <- from your APK: lib/arm64-v8a/
└── assets/                   <- from your APK: the whole assets/ folder
    └── ... (main.rsb, etc.)
```

Optionally, drop a `cursor.png` (up to 64x64, transparency supported) in the same folder to replace the on-screen cursor with your own.

## Controls
 
| Input | Action |
|---|---|
| Touchscreen | Handheld only, always live |
| `+` | Toggle the on-screen cursor |
| `-` | Toggle gyro pointing (tilt/turn the controller to aim) |
| Left stick | Move the cursor |
| `L` / `R` | Recenter the cursor to the middle of the screen (helps gyro aiming) |
| `A` / `ZR` / `ZL` | Tap / confirm (ZL and ZR let you play one-handed) |
| D-pad up / down | Adjust sensitivity of whatever is driving the cursor |
| `L`+`R`+`+` held | Exit |
 
A USB mouse works in both handheld and docked: move to control the cursor, left-click to tap, and use the scroll wheel to change sensitivity. Your stick, mouse and gyro sensitivities are remembered in `pointer.cfg` automatically after in-game adjustment.

## Building
 
Requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the switch-dev group plus these portlibs:
 
```
pacman -S switch-dev
pacman -S switch-mesa switch-libdrm_nouveau switch-libpng switch-zlib
```

## Credits
 
The loader/shim infrastructure (`so_util`, `libc_shim`, `util`) derives from the open-source Switch port of Fruit Ninja by ChanseyIsTheBest, which in turn builds on TheOfficialFloW's Vita/Switch loader lineage — all MIT-licensed. Three things came across from it and all three mattered: bounded futex waits rather than blocking indefinitely, letting libnx allocate thread stacks, and spreading threads across cores. Its GC bridge is Boehm/IL2CPP and does not apply here — NativeAOT's cooperative GC needed different handling.

Icon from @Kosmic on Discord

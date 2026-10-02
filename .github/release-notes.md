Chameleon runs Termux's KWin, or a full Plasma session, on your phone's screen, drawn by the phone's own GPU. No root needed.

> ⚠️ Experimental. Needs an **arm64 phone on Android 10+** and **[Termux from GitHub releases](https://github.com/termux/termux-app/releases)** (not the Play Store version). See [What a phone needs](https://github.com/HarryL0L/Project-Chameleon#what-a-phone-needs).

## Downloads

| File | What it is |
|---|---|
| `chameleon.apk` | The Chameleon app: KWin's screen, plus touch, mouse and keyboard input |
| `{{DEB}}` | The Termux side: the `chameleon` launcher and the shim KWin runs on |
| `kwin_6.7.5_aarch64.deb` | KWin's Wayland compositor (`kwin_wayland`), which isn't in Termux's repo yet |
| `plasma-workspace_6.7.5-1_aarch64.deb` | Plasma's workspace, set up to start a Wayland session on that KWin |
| `layer-shell-qt_6.7.5-1_aarch64.deb` | Fixes Plasma crashing when you right-click the desktop or the panel |
| `kf6-kwindowsystem_6.30.0-1_aarch64.deb` | Fixes the bouncing launch icon on the cursor when you open an app |
| `SHA256SUMS` | Checksums of `chameleon.apk` and the Chameleon `.deb` |

The KDE packages are built from [BullyMaguire-lol/termux-packages `dev/kwin-wayland`](https://github.com/BullyMaguire-lol/termux-packages/tree/dev/kwin-wayland).

## Install

1. Install **Termux** from its GitHub releases. If you have the Play Store version, uninstall it first.
2. Download every file above. Install `chameleon.apk` and open **Chameleon** once.
3. In Termux:

   ```sh
   termux-setup-storage          # once, so Termux can read Downloads
   pkg install x11-repo          # Termux's repository with the KDE packages
   cd /sdcard/Download
   apt install ./{{DEB}} ./kwin_6.7.5_aarch64.deb \
       ./plasma-workspace_6.7.5-1_aarch64.deb ./layer-shell-qt_6.7.5-1_aarch64.deb \
       ./kf6-kwindowsystem_6.30.0-1_aarch64.deb
   ```

   apt fetches everything else these need from Termux's repositories.

4. Start KWin with a terminal:

   ```sh
   pkg install konsole
   chameleon kwin_wayland konsole
   ```

   or a full Plasma desktop:

   ```sh
   pkg install plasma-desktop
   chameleon startplasma-wayland
   ```

5. Switch to the Chameleon app. The ⌨️ button opens the keyboard, and ⚙️ opens the settings (direct touch or trackpad, orientation, keyboard behaviour).

> **Updating Termux:** a later `pkg upgrade` can replace these KDE packages with Termux's own versions, once Termux ships newer ones. If KWin or Plasma stops working after an upgrade, install the debs from the newest Chameleon release again.

## What works

- KWin with its normal DRM backend and no Chameleon-specific patches, rendering through the phone's GPU driver (Mali, Adreno, …).
- Frames go to the app as GPU buffers, with no copies through the CPU.
- Wayland apps render on the GPU too.
- Touch as a touchscreen or a trackpad, plus mouse, hardware keyboards and the on-screen keyboard with an extra-keys row.
- The desktop follows the app's window: rotating the phone or opening the keyboard resizes it at once.
- Coming back from the background shows the current desktop straight away.
- When Plasma turns its screen off, the app says so: tap to wake it.

## Known limits

- arm64 only.
- Opening or closing the keyboard very quickly can show a brief ghost frame while KWin resizes.
- The app can't change the phone's refresh rate. Plasma's display settings show the rate the phone is using.

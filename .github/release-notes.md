Chameleon makes an Android phone look like a Linux PC with a graphics card, so Termux's KWin, or a full Plasma session, runs on the phone's screen and its own GPU. No root needed.

> ⚠️ Experimental. Needs an **arm64 phone on Android 10+** and Termux from **[GitHub](https://github.com/termux/termux-app/releases)** or **[F-Droid](https://f-droid.org/packages/com.termux/)** (not the Play Store version). See [What a phone needs](https://github.com/HarryL0L/Project-Chameleon#what-a-phone-needs).

> ⚠️ **Coming from v0.1.0 or a development build: uninstall the Chameleon app first.** The app's signing key changed with this release, so Android can't update the old app. Its settings start fresh.

## Downloads

| File | What it is |
|---|---|
| `chameleon.apk` | The Chameleon app: the desktop's screen, plus touch, mouse and keyboard input |
| `{{DEB}}` | The Termux side: the `chameleon` launcher and the device layer the compositor runs on |
| `debs-aarch64-….tar` | Plasma 6.8.3 for this release, as Termux packages: KWin's Wayland compositor, a Wayland Plasma session, Xwayland built to draw on the GPU, libinput and the rest of the Plasma set. Termux's own repository doesn't have these yet |
| `SHA256SUMS` | Checksums of `chameleon.apk` and the Chameleon `.deb` |

The KDE packages are built from [BullyMaguire-lol/termux-packages `dev/kwin-wayland`](https://github.com/BullyMaguire-lol/termux-packages/tree/dev/kwin-wayland).

## Install

1. Install **Termux** from GitHub or F-Droid. If you have the Play Store version, uninstall it first.
2. Download every file above. Delete older downloads of these files from `Download` first, if there are any. Install `chameleon.apk` and open **Chameleon** once.
3. In Termux:

   ```sh
   termux-setup-storage          # once, so Termux can read Downloads
   pkg install x11-repo          # Termux's repository with the KDE packages
   pkg upgrade                   # the Plasma packages are built against current Termux
   cd /sdcard/Download
   rm -rf debs                   # a folder left by an earlier install
   tar xf debs-aarch64-*.tar     # makes the folder debs/
   rm -f debs/*-static_* debs/*-cross-tools_*   # only needed for building software
   apt install ./{{DEB}} ./debs/*.deb
   ```

   apt fetches everything else these need from Termux's repositories.

4. Start KWin with a terminal:

   ```sh
   pkg install konsole
   chameleon kwin_wayland konsole
   ```

   or a full Plasma desktop:

   ```sh
   chameleon startplasma-wayland
   ```

5. Switch to the Chameleon app. Tap the Chameleon icon (drag it anywhere) for the keyboard and the settings (direct touch or trackpad, orientation, keyboard behaviour).

> **Updating Termux:** a later `pkg upgrade` can replace these KDE packages with Termux's own versions, once Termux ships newer ones. If KWin or Plasma stops working after an upgrade, install the debs from the newest Chameleon release again.

## New since v0.1.0

- The compositor's side is now a service of its own (`chameleon-kernel`) that looks like a kernel's graphics card and input devices: a render node next to the card, a cursor plane, page flips on the panel's vsync, dumb buffers, the older modesetting calls, and fences on the session's buffers.
- A Vulkan driver for the card, on top of the phone's own: Vulkan programs in Wayland and X11 windows.
- Firefox renders on the GPU, WebGL included. Chromium's GPU process starts.
- Xwayland draws on the GPU (glamor), with an Xwayland built for it.
- The pointer is a layer of its own, so moving it no longer costs frames.
- Typing: Enter and backspace work with Gboard, HeliBoard and AOSP's keyboard; pasted text keeps its lines.
- Clipboard text both ways between Android and the desktop.
- Screen and window recording through KWin's PipeWire screencast, when PipeWire runs in the session.
- The session runs below an init of its own, so a phone's cleaner no longer kills its background services.
- The app says when the desktop's service or compositor is gone or stuck.
- For bug reports: `chameleon --debug-log PROGRAM` records a session into a file, `chameleon --report` shows what the card was asked, `chameleon --probe` tests the graphics stack.

Seen on a Mali-G77 phone on Android 13; the Vulkan parts also on an Adreno 618 on Android 17.

## Experimental in this release

- `CHAMELEON_VULKAN_DRIVER=/path/driver.so`: another GPU driver (Turnip or PanVK built for Android) below Chameleon's Vulkan driver. Needs Termux's `libadrenotools`.

## Known limits

- arm64 only.
- Windows of Vulkan drivers other than Chameleon's don't show.
- Desktop OpenGL on the GPU needs setting up by hand (Mesa's zink on a Vulkan 1.3+ driver below Chameleon's).
- An X11 window can flash black while it is being resized.
- A keyboard's autocorrection can double a word in some apps.
- The app can't change the phone's refresh rate. Plasma's display settings show the rate the phone is using.

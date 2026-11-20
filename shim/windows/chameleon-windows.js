// SPDX-License-Identifier: GPL-2.0-or-later
// A KWin script (loaded by chameleon-windows-session): every app window gets
// a screen of the card to itself, and so a window of the Chameleon app.
//
// The first app window takes the first screen (DSI-1), which is always there.
// For each further one the script asks chameleon-screens for a window of the
// Android app (Open); its connector (DSI-N) gets connected when that window is
// there, and the app window moves onto it, full screen, without a border.
// The Android window carries the app window's title and its app's icon. When
// the app window closes, so does its Android window (Close); when the Android
// window is closed, its connector goes and the app window is asked to close.
// Dialogs and other windows that belong to one stay with it. When the last app
// window has closed and none comes in its place, KWin is asked to quit: the
// session is over. A session started without an app stays (STAY).

const SERVICE = "io.github.harryl0l.Chameleon";
const PATH = "/Screens";
const INTERFACE = SERVICE + ".Screens";
const FIRST = "DSI-1";
const STAY = false; // the session loads a copy with true when it has no app of its own

var owners = {};  // connector name -> the app window whose screen it is
var waiting = []; // {window, name}: a window asked for, its connector not there yet
var asking = [];  // windows whose Open hasn't been answered yet

// The session's end, a moment after the last app window went (an app may
// close one window to open the next).
var over = new QTimer();
over.singleShot = true;
over.interval = 1500;
over.timeout.connect(function () {
    if (!STAY && Object.keys(owners).length === 0 && waiting.length === 0)
        callDBus("org.kde.KWin", "/Session", "org.kde.KWin.Session", "quit");
});

function number(name) {
    return name.substring(name.indexOf("-") + 1);
}

function output(name) {
    const screens = workspace.screens;
    for (var i = 0; i < screens.length; i++) {
        if (screens[i].name === name)
            return screens[i];
    }
    return null;
}

function setTitle(window, name) {
    callDBus(SERVICE, PATH, INTERFACE, "SetTitle", number(name), window.caption);
}

function own(window, name) {
    over.stop();
    owners[name] = window;
    workspace.sendClientToScreen(window, output(name));
    window.noBorder = true;
    window.fullScreen = true;
    setTitle(window, name);
    if (window.desktopFileName) // which app: the session finds its icon
        callDBus(SERVICE, PATH, INTERFACE, "SetApp", number(name), window.desktopFileName);
    window.captionChanged.connect(function () {
        if (owners[name] === window)
            setTitle(window, name);
    });
}

function ownerOf(window) {
    for (var name in owners) {
        if (owners[name] === window)
            return name;
    }
    return null;
}

// An app's own top-level window: not a dialog or a menu of another, not a
// panel or the like.
function getsScreen(window) {
    return window.normalWindow && window.managed && !window.transient && !window.specialWindow;
}

function added(window) {
    const known = asking.indexOf(window) >= 0 || waiting.some(function (w) {
        return w.window === window;
    });
    if (!getsScreen(window) || ownerOf(window) || known)
        return;
    if (!owners[FIRST]) {
        own(window, FIRST);
        return;
    }
    asking.push(window);
    callDBus(SERVICE, PATH, INTERFACE, "Open", function (n) {
        asking.splice(asking.indexOf(window), 1);
        if (n > 0 && window.deleted)
            callDBus(SERVICE, PATH, INTERFACE, "Close", String(n)); // it went meanwhile
        if (!(n > 0) || window.deleted)
            return; // every screen has a window: it stays where KWin put it
        const name = "DSI-" + n;
        if (output(name))
            own(window, name);
        else
            waiting.push({window: window, name: name});
    });
}

function removed(window) {
    waiting = waiting.filter(function (w) {
        if (w.window === window)
            callDBus(SERVICE, PATH, INTERFACE, "Close", number(w.name)); // not wanted any more
        return w.window !== window;
    });
    const name = ownerOf(window);
    if (!name)
        return;
    delete owners[name];
    if (name !== FIRST)
        callDBus(SERVICE, PATH, INTERFACE, "Close", number(name));
    if (Object.keys(owners).length === 0 && waiting.length === 0)
        over.start();
}

function screensChanged() {
    // Connectors that came: the windows waiting for them.
    waiting = waiting.filter(function (w) {
        if (!output(w.name))
            return true;
        own(w.window, w.name);
        return false;
    });
    // Connectors that went (their Android windows were closed): their app windows.
    for (var name in owners) {
        if (name !== FIRST && !output(name)) {
            const window = owners[name];
            delete owners[name];
            window.closeWindow();
        }
    }
    // A screen nobody has or waits for (its app window closed before it came).
    const screens = workspace.screens;
    for (var i = 0; i < screens.length; i++) {
        const name = screens[i].name;
        const awaited = waiting.some(function (w) {
            return w.name === name;
        });
        if (name !== FIRST && !owners[name] && !awaited)
            callDBus(SERVICE, PATH, INTERFACE, "Close", number(name));
    }
}

workspace.windowAdded.connect(added);
workspace.windowRemoved.connect(removed);
workspace.screensChanged.connect(screensChanged);
workspace.windowList().forEach(added);

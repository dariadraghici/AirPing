# AirPing

## Background

The idea for this application originated from a practical problem encountered while serving as PR manager for an electronics event organized by my university's student association. This role involved a large number of meetings scheduled at irregular intervals across different days, which made it difficult to keep track of time using conventional methods. Since I was usually working on other tasks on my laptop at the same time, maintaining focus was challenging, as it required constantly checking the clock to avoid missing the next meeting. This led to the idea of an application that would announce upcoming meetings in a more noticeable way than a standard notification sound or pop-up, namely through small animated planes carrying banners across the screen at the appropriate times, designed to catch attention even while focused on something else.

## What AirPing Does

AirPing is a lightweight Windows desktop utility that lives in the system tray and keeps an eye on your Google Calendar for you. It periodically fetches your upcoming events and, as a meeting approaches, displays an animated banner overlay shaped like a small plane towing a message, gliding across the screen. This visual format is intentionally more noticeable than a standard toast notification and is meant to break through deep focus without being disruptive.

Key behaviors:

- Authenticates with Google Calendar through OAuth 2.0 and securely stores a refresh token locally so you do not need to log in every time.
- Periodically polls the configured calendar for upcoming meetings.
- Computes reminder windows for each meeting at multiple offsets (at the start time, and 5 minutes, 15 minutes, 30 minutes, 1 hour, 3 hours, 5 hours, and 24 hours before it).
- Renders a custom plane-and-banner overlay window for each fired alert, including support for displaying several overlays at once when multiple reminders coincide.
- Distinguishes between regular meetings and bookings made by another person, adjusting the banner text accordingly.
- Keeps track of which alerts have already been fired, so the same reminder is never shown twice.
- Lives entirely in the system tray, with a settings window for entering Google API credentials and the target calendar ID, and a menu for manually refreshing meetings or exiting the application.
- Persists configuration (client ID, client secret, calendar ID, refresh token) in the Windows Registry under the current user.

## Platform Scope

AirPing currently supports only Google Calendar. Integration with other calendar platforms, such as Calendly or Microsoft's ecosystem through Azure, was also considered during development, but these alternatives generally require a paid subscription to access their respective APIs at the level needed for this kind of integration. For this reason, Google Calendar was chosen as the supported platform, as it offers free API access sufficient for the application's needs.

## Languages and Technologies

- **C++** as the core implementation language, using the C++ Standard Library (`std::thread`, `std::mutex`, `std::atomic`, `std::chrono`) for concurrency and timing.
- **Win32 API** for the native Windows application shell: window creation and message loop, system tray icon integration (`Shell_NotifyIcon`), owner-drawn custom UI controls (buttons with shadows and rounded corners), GDI-based rendering for the overlay windows, and DPI awareness handling.
- **WinHTTP** for all outbound HTTPS communication, used to perform the OAuth 2.0 token exchange and to call the Google Calendar REST API.
- **Windows Registry API** for lightweight local persistence of credentials and tokens.
- **Resource compiler (.rc files)** for embedding the application icon and other Windows resources into the final executable.
- **Visual Studio / MSVC toolchain** (`cl.exe`, `rc.exe`) for compilation, with a batch script that automatically locates `vcvarsall.bat` across Visual Studio 2017, 2019, and 2022 installations if the build tools are not already on the system PATH.

## Underlying Theory and Concepts

- **OAuth 2.0 Authorization Code Flow**: the application opens a login flow against Google's OAuth endpoints, exchanges the authorization code for an access token and a refresh token, and later uses the refresh token to silently obtain new access tokens once the original one expires, without requiring the user to log in again.
- **REST API consumption**: meetings are retrieved by calling the Google Calendar API over HTTPS and parsing the returned event data into an internal representation.
- **Multithreading and synchronization**: network calls and meeting calculations run on background threads so that the UI thread is never blocked. Shared state (the list of meetings, the list of already-fired alerts, and the authentication tokens) is protected with mutexes to avoid race conditions between the timer-driven background threads and the main message loop.
- **Event-driven programming**: the application is structured around the classic Win32 message loop and timer messages (`WM_TIMER`), which drive both the periodic calendar polling and the per-second check for meetings about to start.
- **Custom rendering and layered windows**: the plane-and-banner overlays are drawn using GDI primitives rather than standard window controls, in order to achieve the animated, borderless visual effect.
- **Local port binding for the OAuth redirect**: as part of the login flow, the application opens a temporary local socket to receive the authorization response from the browser. Since a fixed port cannot be guaranteed to be free, the operating system is asked to search for and assign an available port at runtime, which is then used to construct the local redirect URI passed to Google's OAuth endpoint.

## How to Run

1. Compile the project by running `build.bat`. The script will automatically locate `cl.exe` and `rc.exe` through `vcvarsall.bat` if the Visual Studio build tools are not already in your PATH, then compile the resources and the source files into `AirPing.exe`.
2. Run the resulting `AirPing.exe`.
3. On first launch, a settings window will prompt you for a **Client ID** and **Client Secret**. These are Google OAuth credentials that I will provide if you want to test the application, since the project is currently restricted to whitelisted test users (see the Testing Mode section below).
4. In the **Calendar ID** field, enter the Gmail address of the Google account whose calendar you want AirPing to monitor, that is, the Gmail account you are connected with as a client.
5. Once these fields are filled in, AirPing will start the OAuth login flow, store the resulting refresh token locally, and begin polling your calendar in the background from the system tray.

## Current Status: Testing Mode

AirPing is currently running in testing mode on the Google Cloud side. Because the OAuth consent screen has not yet been published and verified with Google, the application's Google Cloud project does not have a registered production domain, and only a limited, explicitly whitelisted set of Google accounts can authenticate and use the application.

If you would like to try AirPing, please contact me directly and I will add your Google account as an authorized test user so that the login flow works for you.

## Copyright

Copyright Drăghici Daria-Ioana, June 2026. All rights reserved.

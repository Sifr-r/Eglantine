# Android

## Dependencies

* [Android Studio](https://developer.android.com/studio)
* Android SDK platform 36, NDK `28.2.13676358`, and CMake `3.31.5`, as pinned in [the app build configuration](../../../src/android/app/build.gradle).
* JDK 17 for the Gradle build.
* [Git](https://git-scm.com/download)

## WINDOWS ONLY - Additional Dependencies

* **[Visual Studio 2022 Community](https://visualstudio.microsoft.com/downloads/)** - **Make sure to select "Desktop development with C++" support in the installer. Make sure to update to the latest version if already installed.**
* **[Vulkan SDK](https://vulkan.lunarg.com/sdk/home#windows)** - **Make sure to select Latest SDK.**
  * See the [Windows Vulkan SDK installer](../../../tools/windows/install-vulkan-sdk.ps1).

## Getting the source

Use the Eglantine checkout described in the [general build guide](../Build.md#clone).

## Building

1. Start Android Studio, on the startup dialog select `Open`.
2. Open `src/android` from the repository root.
3. In `Build > Select Build Variant`, select `mainlineRelWithDebInfo`.
4. Build the project with `Build > Make Project` or run it on an Android device with `Run > Run 'app'`.

## Building with Terminal

Install the pinned SDK tools, select JDK 17, and let Android Studio set up the SDK path or
point `ANDROID_HOME` at the installed Android SDK. From `src/android`, run:

```sh
./gradlew :app:assembleMainlineRelWithDebInfo
```

Use `gradlew.bat` on Windows. The Android Gradle plugin writes the APK under
`src/android/app/build/outputs/apk/`.

See the [language and domain ownership ledger](../../knowledge/rust_ffi.md#first-party-language-and-domain-ownership)
for the current ARM64 Android build and verification status.

### Additional Resources

> **Info**
>
> [https://developer.android.com/studio/intro/](https://developer.android.com/studio/intro/)

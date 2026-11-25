// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

import org.gradle.api.tasks.compile.JavaCompile

plugins {
    id("com.android.application")
}

val edenDir = project(":Eden").projectDir

android {
    namespace = "dev.eden.eden_emulator.nativeprototype"
    compileSdk = 36
    ndkVersion = "28.2.13676358"

    defaultConfig {
        applicationId = namespace
        minSdk = 24
        targetSdk = 36
        versionCode = 1
        versionName = "0.1-prototype"

        ndk {
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DEGLANTINE_ANDROID_NATIVE_PROTOTYPE=ON",
                    "-DANDROID_ARM_NEON=true",
                    "-DENABLE_INTEL_XESS=OFF",
                    "-DENABLE_ISPC_KERNELS=OFF",
                    "-DENABLE_MCP_SERVER=OFF",
                    "-DENABLE_SENTRY=OFF",
                    "-DEGLANTINE_RUST_OFFLINE=ON",
                    "-DYUZU_USE_CPM=ON",
                    "-DCPMUTIL_FORCE_BUNDLED=ON",
                    "-DYUZU_DOWNLOAD_ANDROID_VVL=OFF",
                    "-DYUZU_USE_BUNDLED_FFMPEG=ON",
                    "-DYUZU_TESTS=OFF",
                    "-DDYNARMIC_TESTS=OFF",
                )
                targets += listOf("eglantine-android-prototype", "eglantine-android-prototype-sdl")
            }
        }
    }

    buildFeatures {
        buildConfig = false
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    externalNativeBuild {
        cmake {
            version = "3.31.5"
            path = edenDir.resolve("CMakeLists.txt")
        }
    }

    packaging {
        // SDLActivity opens libmain.so from ApplicationInfo.nativeLibraryDir.
        jniLibs.useLegacyPackaging = true
    }

    sourceSets.named("main") {
        java.srcDir(edenDir.resolve("externals/generated/sdl/java"))
    }
}

androidComponents.onVariants { variant ->
    val variantName = variant.name.replaceFirstChar { it.uppercaseChar() }
    tasks.withType<JavaCompile>().configureEach {
        if (name == "compile${variantName}JavaWithJavac") {
            // CMake supplies the pinned upstream SDL activity sources.
            dependsOn("externalNativeBuild${variantName}")
        }
    }
}

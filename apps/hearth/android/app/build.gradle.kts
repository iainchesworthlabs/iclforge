plugins {
    id("com.android.application")
}

// Release-keystore signing, wired to environment variables rather than local.properties, exactly as
// apps/demos/android does it (the same four names): CI decodes a keystore secret to a runner-temp
// file and exports them before assembleRelease. Absent locally and on ordinary CI, where every build
// path degrades to the debug keystore. The app is sideloaded (planning/hearth-android.md, D14), so a
// release key only matters for update-signature continuity.
val releaseKeystorePath = System.getenv("ANDROID_KEYSTORE_PATH")
val releaseKeystorePassword = System.getenv("ANDROID_KEYSTORE_PASSWORD")
val releaseKeyAlias = System.getenv("ANDROID_KEY_ALIAS")
val releaseKeyPassword = System.getenv("ANDROID_KEY_PASSWORD")
val releaseSigningAvailable = !releaseKeystorePath.isNullOrBlank() &&
    !releaseKeystorePassword.isNullOrBlank() &&
    !releaseKeyAlias.isNullOrBlank() &&
    !releaseKeyPassword.isNullOrBlank()

android {
    namespace = "com.iclforge.hearth"
    // As the Shield app: 36 is the newest platform in the SDK repository, 34 the target actually
    // exercised, so the app runs as the Shield's own system image runs it.
    compileSdk = 36

    defaultConfig {
        applicationId = "com.iclforge.hearth"
        // 26 (Oreo): notification channels and the foreground-service start the sink needs, and
        // the floor of AAudio for the pass-through output that comes after this skeleton. Android
        // TV hardware from 2017 on is above it.
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0-alpha.1"

        externalNativeBuild {
            cmake {
                // c++_shared, as the Shield app: the libraries linked into the one shared object
                // are static, and a static STL would duplicate global state if anything else in
                // the process pulled libc++ in too.
                //
                // Everything vcpkg needs (the toolchain chain, the triplet, the "hearth" feature,
                // the repository's overlay ports) is src/main/cpp/CMakeLists.txt's, which is also
                // where it is explained; this file adds only what AGP must be told.
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    // The NDK ships no clang-scan-deps (see apps/demos/android/app/build.gradle.kts).
                    "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF"
                )
                // The one library the app loads. Without it AGP builds every target of the
                // repository root and packages each shared object that comes out, none of which
                // iclforge_hearth_jni needs (it links the sink and the protocol statically): about
                // 5 MB of ac3, ac4, containers and the rest in an app that decodes nothing yet.
                targets += listOf("iclforge_hearth_jni")
            }
        }

        ndk {
            // arm64 is every Android TV box worth the name (the 2017 Shield on). x86_64, for the
            // emulator the instrumented tests of a later phase run on, needs the vcpkg triplet
            // x64-android and is added with them.
            abiFilters += listOf("arm64-v8a")
        }
    }

    // NDK r26.1, pinned as the Shield app pins it: a build failure means something changed, not
    // that a different NDK was picked.
    ndkVersion = "26.1.10909125"

    signingConfigs {
        if (releaseSigningAvailable) {
            create("release") {
                storeFile = file(releaseKeystorePath!!)
                storePassword = releaseKeystorePassword
                keyAlias = releaseKeyAlias
                keyPassword = releaseKeyPassword
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            // The repository asks for 3.28...4.3; 3.31.6 is what the SDK manager has for the 3.x line.
            version = "3.31.6"
        }
    }

    buildTypes {
        debug {
            // AGP's debug default is CMAKE_BUILD_TYPE=Debug (-O0), which for this app is also the
            // build type vcpkg would build every port a second time for. RelWithDebInfo keeps the
            // APK debuggable and uses the release builds of the ports.
            externalNativeBuild {
                cmake {
                    arguments += listOf("-DCMAKE_BUILD_TYPE=RelWithDebInfo")
                }
            }
        }
        release {
            isMinifyEnabled = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = if (releaseSigningAvailable) {
                signingConfigs.getByName("release")
            } else {
                signingConfigs.getByName("debug")
            }
            externalNativeBuild {
                cmake {
                    arguments += listOf("-DCMAKE_BUILD_TYPE=Release")
                }
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

// No dependency of its own: the screens are platform Views and the service, the notification and
// NsdManager are the platform's. Nothing for a lockfile to hold beyond what AGP and Kotlin bring.

// SonarCloud text:S8569 - pin resolved dependency versions so a build is reproducible from the
// committed lockfile. Regenerate with `./gradlew :app:dependencies --write-locks` after changing a
// dependency; a build then fails if the resolution drifts from gradle.lockfile.
dependencyLocking {
    lockAllConfigurations()
}

// This app has no dependency of its own, so what the lockfile holds is AGP's: the lint tool
// (androidLintTool) and the unified test platform's work action, build-time only and not in the APK.
// Their pinned releases carry advisories OSV-Scanner reports, so each is forced to the release that
// fixes it, as apps/demos/android/app/build.gradle.kts forces the same set (and, there, the Netty
// artifacts the emulator tests' own configurations bring, which this app has no tests to resolve
// yet: they are added with its instrumented tests, together with a lock refresh against a device).
configurations.all {
    resolutionStrategy {
        force(
            // The protobuf-java and protobuf-kotlin pair stays matched: GHSA-735f-pc8j-v9w8.
            "com.google.protobuf:protobuf-java:4.36.2",
            "com.google.protobuf:protobuf-kotlin:4.36.2",
            "commons-io:commons-io:2.22.0",                  // GHSA-78wr-2p64-hpwj
            "org.apache.commons:commons-lang3:3.21.0",       // GHSA-j288-q9x7-2f5v
            "org.apache.httpcomponents:httpclient:4.5.14",   // GHSA-7r82-7xv7-xcpj
            // One release of the three so the set stays matched; 1.86 fixes GHSA-9pwp-9qqc-pr26,
            // GHSA-qp49-qgx5-5m26 and GHSA-c3fc-8qff-9hwx (bcprov) and GHSA-wg6q-6289-32hp (bcpkix).
            "org.bouncycastle:bcprov-jdk18on:1.86",
            "org.bouncycastle:bcpkix-jdk18on:1.86",
            "org.bouncycastle:bcutil-jdk18on:1.86"
        )
    }
}

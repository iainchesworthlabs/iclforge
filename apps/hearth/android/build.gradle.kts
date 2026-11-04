// Root build script: nothing lives here directly, only plugin version declarations shared with
// app/build.gradle.kts (the `apply false` pattern avoids applying the Android plugin at the root,
// where there is no android {} block to configure). The version is the Shield app's: one AGP for
// both Android projects of the tree, and one lockfile's worth of build tooling to keep current.
plugins {
    id("com.android.application") version "9.5.0" apply false
}

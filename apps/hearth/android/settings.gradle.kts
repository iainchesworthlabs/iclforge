// Hearth for Android - a standalone Gradle build living beside, not inside, iclforge's CMake build
// (planning/hearth-android.md, D1), as apps/demos/android is. It reaches into the repo root only
// through app/src/main/cpp/CMakeLists.txt's add_subdirectory(); nothing here assumes a Gradle-based
// CMake project structure for the rest of the repo.

pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "hearth-android"
include(":app")

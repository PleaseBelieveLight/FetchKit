pluginManagement {
    repositories {
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        mavenCentral()
        gradlePluginPortal()
    }
}
plugins {
    id("org.gradle.toolchains.foojay-resolver-convention") version "1.0.0"
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "FetchKit"
include(":sdk", ":sample")
project(":sdk").projectDir = file("platform/android/sdk")
project(":sample").projectDir = file("platform/android/sample")

// Measurement-only host; absent from normal builds and SDK dependency graphs.
if (providers.gradleProperty("fetchkit.sizeMode").isPresent) {
    include(":size-host")
    project(":size-host").projectDir = file("tools/size/android-host")
}

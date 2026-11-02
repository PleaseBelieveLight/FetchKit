// Top-level build file where you can add configuration options common to all sub-projects/modules.
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.android.library) apply false
    alias(libs.plugins.kotlin.compose) apply false
}

val fetchkitVersion = providers.fileContents(layout.projectDirectory.file("VERSION")).asText.get().trim()
require(fetchkitVersion.matches(Regex("(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*)(-(alpha|beta|rc)[.](0|[1-9][0-9]*))?"))) {
    "VERSION 必须为 major.minor.patch，可附加 -alpha.N、-beta.N 或 -rc.N"
}
version = fetchkitVersion

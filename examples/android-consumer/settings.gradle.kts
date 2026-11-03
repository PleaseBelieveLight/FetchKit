pluginManagement { repositories { google(); mavenCentral(); gradlePluginPortal() } }
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        maven {
            val previewRepository = settingsDir.resolve("../../build/m5-maven")
            url = providers.gradleProperty("fetchkit.repository").orNull?.let { uri(it) }
                ?: previewRepository.toURI()
            content { includeGroup("com.luofelix.fetchkit") }
        }
        google()
        mavenCentral()
    }
}
rootProject.name = "FetchKitExternalConsumer"

pluginManagement { repositories { google(); mavenCentral(); gradlePluginPortal() } }
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        maven {
            url = uri(providers.gradleProperty("fetchkit.repository").get())
            content { includeGroup("com.luofelix.fetchkit") }
        }
        google()
        mavenCentral()
    }
}
rootProject.name = "FetchKitExternalConsumer"

// ---------------------------------------------------------------------------
//  Состав сборки.
//
//  Хранилища перечислены явно и только те, что нужны: приложение собирается в
//  сети гостиницы не будет, но собирается оно здесь, и лишний источник — это
//  лишняя точка отказа при нестабильном канале.
// ---------------------------------------------------------------------------

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

rootProject.name = "PoolSafetyWatch"
include(":app")

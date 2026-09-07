plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "ru.poolsafety.watch"
    compileSdk = 34

    defaultConfig {
        applicationId = "ru.poolsafety.watch"

        // ПОЧЕМУ 26, А НЕ ВЫШЕ. У заказчика личный телефон новый, но рабочий
        // может оказаться примерно 2018 года. Android 8.0 — нижняя граница, на
        // которой ещё доступна картинка в картинке; ниже её нет вовсе.
        minSdk = 26
        targetSdk = 34

        // versionCode обязан расти с каждой поставкой: Android не поставит
        // обновление поверх, если номер не увеличился, и заказчик остался бы
        // со старой сборкой, думая, что обновился.
        versionCode = 6
        versionName = "1.3.0"
    }

    buildTypes {
        release {
            // Сокращение кода выключено намеренно. Выигрыш в размере тут
            // ничтожен, а сломать отражением что-нибудь в Media3 — легко.
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"),
                          "proguard-rules.pro")

            // ПОДПИСЬ ОТЛАДОЧНАЯ, И ЭТО ОСОЗНАННО. Приложение ставится на
            // телефоны гостиницы сбоку, файлом, а не через магазин — там
            // отдельный ключ не нужен. Неподписанный же APK Android просто не
            // установит, и «собранная поставка» оказалась бы бесполезной.
            //
            // Понадобится магазин — здесь заводится свой ключ, и только тогда.
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        viewBinding = true
        // Единственное, ради чего он включён: BuildConfig.VERSION_NAME читает
        // приложение, когда сравнивает себя с выпуском на GitHub. Версия
        // задаётся один раз, здесь же — второго места для неё нет.
        buildConfig = true
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.constraintlayout:constraintlayout:2.1.4")
    implementation("androidx.lifecycle:lifecycle-service:2.8.4")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.4")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")

    // Просмотр камер. RTSP вынесен в отдельный модуль, без него ExoPlayer
    // адрес rtsp:// не откроет.
    implementation("androidx.media3:media3-exoplayer:1.4.1")
    implementation("androidx.media3:media3-exoplayer-rtsp:1.4.1")
    implementation("androidx.media3:media3-ui:1.4.1")
}

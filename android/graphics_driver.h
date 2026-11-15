// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <android/asset_manager_jni.h>
#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vulkan/vulkan.h>

namespace Ps4Android {
inline PFN_vkGetInstanceProcAddr LoadGraphicsDriver(JNIEnv* env, jobject activity,
        const std::string& native_libraries, const std::string& internal) {
    jclass type = env->GetObjectClass(activity);
    jmethodID get_selection = env->GetMethodID(type, "getGraphicsDriverPath", "()Ljava/lang/String;");
    if (!get_selection || env->ExceptionCheck()) {
        env->ExceptionClear(); env->DeleteLocalRef(type);
        throw std::runtime_error("Graphics driver settings are unavailable");
    }
    auto java_selection = static_cast<jstring>(env->CallObjectMethod(activity, get_selection));
    env->DeleteLocalRef(type);
    if (!java_selection || env->ExceptionCheck()) {
        env->ExceptionClear();
        throw std::runtime_error("Cannot read graphics driver selection");
    }
    const char* value = env->GetStringUTFChars(java_selection, nullptr);
    if (!value) { env->DeleteLocalRef(java_selection); throw std::runtime_error("Cannot read graphics driver path"); }
    const std::string selection(value);
    env->ReleaseStringUTFChars(java_selection, value);
    env->DeleteLocalRef(java_selection);
    void* loader = nullptr;
    if (selection == "system") {
        loader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        puts("Android: selected system Vulkan driver");
    } else {
        std::filesystem::path library;
        if (selection == "bundled") {
            jclass activity_type = env->GetObjectClass(activity);
            jmethodID get_assets = env->GetMethodID(activity_type, "getAssets", "()Landroid/content/res/AssetManager;");
            jobject java_assets = env->CallObjectMethod(activity, get_assets);
            auto* assets = AAssetManager_fromJava(env, java_assets);
            env->DeleteLocalRef(java_assets); env->DeleteLocalRef(activity_type);
            if (!assets) throw std::runtime_error("Android assets are unavailable");
            AAsset* asset = AAssetManager_open(assets, "libvulkan_freedreno.so", AASSET_MODE_STREAMING);
            if (!asset) throw std::runtime_error("Missing packaged Turnip driver");
            const auto folder = std::filesystem::path(internal) / "drivers/bundled";
            std::filesystem::create_directories(folder);
            library = folder / "libvulkan_freedreno.so";
            const std::string temporary = library.string() + ".tmp";
            FILE* file = fopen(temporary.c_str(), "wb");
            if (!file) { AAsset_close(asset); throw std::runtime_error("Cannot stage bundled Turnip"); }
            char bytes[65536]; int count; bool okay = true;
            while ((count = AAsset_read(asset, bytes, sizeof bytes)) > 0) {
                if (fwrite(bytes, 1, count, file) != static_cast<size_t>(count)) { okay = false; break; }
            }
            AAsset_close(asset);
            if (fclose(file) || count < 0 || !okay) {
                std::filesystem::remove(temporary);
                throw std::runtime_error("Bundled Turnip copy failed");
            }
            std::filesystem::rename(temporary, library);
            puts("Android: selected bundled Turnip driver");
        } else {
            library = std::filesystem::canonical(selection);
            const auto root = std::filesystem::canonical(std::filesystem::path(internal) / "drivers/imported").string() + "/";
            if (!library.string().starts_with(root) || !std::filesystem::is_regular_file(library))
                throw std::runtime_error("Imported driver path is outside private driver storage");
            printf("Android: selected imported graphics driver %s\n", library.c_str());
        }
        const std::string directory = library.parent_path().string() + "/";
        const std::string temporary = internal + "/";
        loader = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
            temporary.c_str(), native_libraries.c_str(), directory.c_str(), library.filename().c_str(), nullptr, nullptr);
    }
    if (!loader) throw std::runtime_error("Selected Vulkan driver could not be loaded; use Graphics driver from the app icon to restore bundled Turnip");
    auto proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
    if (!proc) throw std::runtime_error("Selected driver has no vkGetInstanceProcAddr");
    return proc;
}
inline bool OpenGraphicsDriverSettings() {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return false;
    jclass type = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(type, "showGraphicsDriverSettings", "()V");
    if (!method || env->ExceptionCheck()) {
        env->ExceptionClear(); env->DeleteLocalRef(type); env->DeleteLocalRef(activity);
        return false;
    }
    env->CallVoidMethod(activity, method);
    const bool okay = !env->ExceptionCheck();
    if (!okay) env->ExceptionClear();
    env->DeleteLocalRef(type); env->DeleteLocalRef(activity);
    return okay;
}

}

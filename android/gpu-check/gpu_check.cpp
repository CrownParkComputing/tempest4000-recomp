/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Native Android surface/driver check. This APK contains no Tempest game data. */
#define VK_USE_PLATFORM_ANDROID_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <adrenotools/driver.h>
#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
FILE *logfile;
void log(const char *format, ...) {
    char text[1024];
    va_list arguments; va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments); va_end(arguments);
    __android_log_write(ANDROID_LOG_INFO, "TempestGpuCheck", text);
    if (logfile) { fprintf(logfile, "%s\n", text); fflush(logfile); }
}
void check(VkResult result, const char *operation) {
    if (result != VK_SUCCESS) {
        log("STOP: %s returned %d", operation, result);
        throw std::runtime_error(operation);
    }
}
struct State {
    ANativeActivity *activity;
    std::string native_libraries;
    std::thread worker;
    std::atomic<bool> stop{false};
};
std::string native_libraries(ANativeActivity *activity) {
    auto *env = activity->env;
    jclass type = env->GetObjectClass(activity->clazz);
    jmethodID method = env->GetMethodID(type, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    jobject info = env->CallObjectMethod(activity->clazz, method);
    jclass info_type = env->GetObjectClass(info);
    jfieldID field = env->GetFieldID(info_type, "nativeLibraryDir", "Ljava/lang/String;");
    auto string = static_cast<jstring>(env->GetObjectField(info, field));
    const char *bytes = env->GetStringUTFChars(string, nullptr);
    std::string result(bytes);
    env->ReleaseStringUTFChars(string, bytes);
    env->DeleteLocalRef(string); env->DeleteLocalRef(info_type);
    env->DeleteLocalRef(info); env->DeleteLocalRef(type);
    return result;
}
void copy_driver(ANativeActivity *activity) {
    AAsset *asset = AAssetManager_open(activity->assetManager, "libvulkan_freedreno.so", AASSET_MODE_STREAMING);
    if (!asset) throw std::runtime_error("missing Turnip asset");
    std::string path = std::string(activity->internalDataPath) + "/libvulkan_freedreno.so";
    FILE *out = fopen(path.c_str(), "wb");
    if (!out) { AAsset_close(asset); throw std::runtime_error("cannot copy Turnip"); }
    char buffer[65536]; int count;
    bool okay = true;
    while ((count = AAsset_read(asset, buffer, sizeof(buffer))) > 0)
        if (fwrite(buffer, 1, count, out) != static_cast<size_t>(count)) { okay = false; break; }
    AAsset_close(asset);
    if (fclose(out) || count < 0 || !okay) throw std::runtime_error("Turnip copy failed");
}

void run(State &state, ANativeWindow *window) {
    try {
        auto *activity = state.activity;
        const char *directory = activity->externalDataPath ? activity->externalDataPath : activity->internalDataPath;
        std::string log_path = std::string(directory) + "/gpu-check.log";
        logfile = fopen(log_path.c_str(), "w");
        log("Tempest Android GPU check; no game content is included");
        copy_driver(activity);
        const std::string driver_directory = std::string(activity->internalDataPath) + "/";
        void *loader = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
            activity->internalDataPath, state.native_libraries.c_str(), driver_directory.c_str(),
            "libvulkan_freedreno.so", nullptr, nullptr);
        if (!loader) { log("Driver load: %s", dlerror()); throw std::runtime_error("Turnip loader"); }
        auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
        if (!get) throw std::runtime_error("vkGetInstanceProcAddr");
#define INSTANCE_FUNCTION(name) auto name = reinterpret_cast<PFN_##name>(get(instance, #name)); \
        if (!name) throw std::runtime_error(#name)
        VkInstance instance = VK_NULL_HANDLE;
        INSTANCE_FUNCTION(vkCreateInstance);
        INSTANCE_FUNCTION(vkEnumerateInstanceVersion);
        uint32_t api = VK_API_VERSION_1_0;
        check(vkEnumerateInstanceVersion(&api), "vkEnumerateInstanceVersion");
        log("Custom Vulkan loader: %u.%u.%u", VK_VERSION_MAJOR(api), VK_VERSION_MINOR(api), VK_VERSION_PATCH(api));
        const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
        VkApplicationInfo app{.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName="Tempest GPU check", .apiVersion=VK_API_VERSION_1_3};
        VkInstanceCreateInfo create{.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&app,
            .enabledExtensionCount=2, .ppEnabledExtensionNames=extensions};
        check(vkCreateInstance(&create, nullptr, &instance), "vkCreateInstance");
        INSTANCE_FUNCTION(vkDestroyInstance);
        INSTANCE_FUNCTION(vkEnumeratePhysicalDevices);
        INSTANCE_FUNCTION(vkGetPhysicalDeviceProperties);
        INSTANCE_FUNCTION(vkGetPhysicalDeviceFeatures2);
        INSTANCE_FUNCTION(vkGetPhysicalDeviceQueueFamilyProperties);
        INSTANCE_FUNCTION(vkGetPhysicalDeviceSurfaceSupportKHR);
        INSTANCE_FUNCTION(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
        INSTANCE_FUNCTION(vkGetPhysicalDeviceSurfaceFormatsKHR);
        INSTANCE_FUNCTION(vkCreateAndroidSurfaceKHR);
        INSTANCE_FUNCTION(vkDestroySurfaceKHR);
        INSTANCE_FUNCTION(vkCreateDevice);
        INSTANCE_FUNCTION(vkGetDeviceProcAddr);
        uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        if (!count) throw std::runtime_error("no Vulkan GPU");
        std::vector<VkPhysicalDevice> physicals(count);
        check(vkEnumeratePhysicalDevices(instance, &count, physicals.data()), "vkEnumeratePhysicalDevices");
        VkPhysicalDevice physical = physicals.front();
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical, &properties);
        log("GPU: %s; Vulkan %u.%u.%u", properties.deviceName, VK_VERSION_MAJOR(properties.apiVersion),
            VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion));
        VkPhysicalDeviceVulkan13Features f13{.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext=&f13};
        VkPhysicalDeviceFeatures2 features{.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext=&f12};
        vkGetPhysicalDeviceFeatures2(physical, &features);
        log("Features: dynamicRendering=%u synchronization2=%u bufferDeviceAddress=%u timelineSemaphore=%u scalarBlockLayout=%u shaderInt64=%u geometryShader=%u",
            f13.dynamicRendering, f13.synchronization2, f12.bufferDeviceAddress, f12.timelineSemaphore,
            f12.scalarBlockLayout, features.features.shaderInt64, features.features.geometryShader);
        if (properties.apiVersion < VK_API_VERSION_1_3) throw std::runtime_error("Vulkan 1.3 required");
        VkAndroidSurfaceCreateInfoKHR android{.sType=VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window=window};
        VkSurfaceKHR surface;
        check(vkCreateAndroidSurfaceKHR(instance, &android, nullptr, &surface), "vkCreateAndroidSurfaceKHR");
        uint32_t nq=0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> queues(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &nq, queues.data());
        uint32_t family=UINT32_MAX;
        for (uint32_t i=0; i<nq; ++i) {
            VkBool32 present=VK_FALSE;
            check(vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present), "vkGetPhysicalDeviceSurfaceSupportKHR");
            if (present && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { family=i; break; }
        }
        if (family==UINT32_MAX) throw std::runtime_error("no presentation queue");
        float priority=1;
        VkDeviceQueueCreateInfo queue_create{.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex=family, .queueCount=1, .pQueuePriorities=&priority};
        const char *swapchain_extension=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo device_create{.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .queueCreateInfoCount=1, .pQueueCreateInfos=&queue_create, .enabledExtensionCount=1,
            .ppEnabledExtensionNames=&swapchain_extension};
        VkDevice device;
        check(vkCreateDevice(physical, &device_create, nullptr, &device), "vkCreateDevice");
#define DEVICE_FUNCTION(name) auto name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name)); \
        if (!name) throw std::runtime_error(#name)
        DEVICE_FUNCTION(vkDestroyDevice); DEVICE_FUNCTION(vkGetDeviceQueue);
        DEVICE_FUNCTION(vkCreateSwapchainKHR); DEVICE_FUNCTION(vkGetSwapchainImagesKHR);
        DEVICE_FUNCTION(vkDestroySwapchainKHR); DEVICE_FUNCTION(vkAcquireNextImageKHR);
        DEVICE_FUNCTION(vkCreateCommandPool); DEVICE_FUNCTION(vkDestroyCommandPool);
        DEVICE_FUNCTION(vkAllocateCommandBuffers); DEVICE_FUNCTION(vkBeginCommandBuffer);
        DEVICE_FUNCTION(vkEndCommandBuffer); DEVICE_FUNCTION(vkCmdPipelineBarrier);
        DEVICE_FUNCTION(vkCmdClearColorImage); DEVICE_FUNCTION(vkCreateSemaphore);
        DEVICE_FUNCTION(vkDestroySemaphore); DEVICE_FUNCTION(vkQueueSubmit);
        DEVICE_FUNCTION(vkQueuePresentKHR); DEVICE_FUNCTION(vkQueueWaitIdle);
        VkSurfaceCapabilitiesKHR caps;
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        uint32_t nf=0;
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &nf, nullptr), "vkGetPhysicalDeviceSurfaceFormatsKHR");
        if (!nf || !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) throw std::runtime_error("surface format/clear unsupported");
        std::vector<VkSurfaceFormatKHR> formats(nf);
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &nf, formats.data()), "vkGetPhysicalDeviceSurfaceFormatsKHR");
        VkExtent2D extent=caps.currentExtent;
        if (extent.width==UINT32_MAX) extent={static_cast<uint32_t>(ANativeWindow_getWidth(window)), static_cast<uint32_t>(ANativeWindow_getHeight(window))};
        uint32_t images_count=caps.minImageCount+1;
        if (caps.maxImageCount) images_count=std::min(images_count,caps.maxImageCount);
        VkSwapchainCreateInfoKHR swap_create{.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface=surface, .minImageCount=images_count, .imageFormat=formats[0].format,
            .imageColorSpace=formats[0].colorSpace, .imageExtent=extent, .imageArrayLayers=1,
            .imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT, .imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,
            .preTransform=caps.currentTransform, .compositeAlpha=VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
            .presentMode=VK_PRESENT_MODE_FIFO_KHR, .clipped=VK_TRUE};
        if (!(caps.supportedCompositeAlpha & swap_create.compositeAlpha))
            swap_create.compositeAlpha=static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha+1));
        VkSwapchainKHR swapchain;
        check(vkCreateSwapchainKHR(device, &swap_create, nullptr, &swapchain), "vkCreateSwapchainKHR");
        check(vkGetSwapchainImagesKHR(device, swapchain, &images_count, nullptr), "vkGetSwapchainImagesKHR");
        std::vector<VkImage> images(images_count);
        check(vkGetSwapchainImagesKHR(device, swapchain, &images_count, images.data()), "vkGetSwapchainImagesKHR");
        VkCommandPoolCreateInfo pool_create{.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex=family};
        VkCommandPool pool;
        check(vkCreateCommandPool(device, &pool_create, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo allocate{.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=pool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount=1};
        VkCommandBuffer command;
        check(vkAllocateCommandBuffers(device, &allocate, &command), "vkAllocateCommandBuffers");
        VkSemaphoreCreateInfo semaphore_create{.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore acquired, ready;
        check(vkCreateSemaphore(device, &semaphore_create, nullptr, &acquired), "vkCreateSemaphore");
        check(vkCreateSemaphore(device, &semaphore_create, nullptr, &ready), "vkCreateSemaphore");
        uint32_t image;
        check(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &image), "vkAcquireNextImageKHR");
        VkCommandBufferBeginInfo begin{.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier barrier{.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout=VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED, .image=images[image], .subresourceRange=range};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        const VkClearColorValue color{{0.06f, 0.015f, 0.16f, 1.0f}};
        vkCmdClearColorImage(command, images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=0;
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
        VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
        VkPipelineStageFlags wait_stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount=1, .pWaitSemaphores=&acquired,
            .pWaitDstStageMask=&wait_stage, .commandBufferCount=1, .pCommandBuffers=&command,
            .signalSemaphoreCount=1, .pSignalSemaphores=&ready};
        check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        VkPresentInfoKHR present{.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount=1,
            .pWaitSemaphores=&ready, .swapchainCount=1, .pSwapchains=&swapchain, .pImageIndices=&image};
        check(vkQueuePresentKHR(queue, &present), "vkQueuePresentKHR");
        check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
        log("PASS: Turnip Vulkan 1.3+, Android surface and %ux%u swapchain presentation", extent.width, extent.height);
        // Keep the presented diagnostic image visible until the activity closes.
        while (!state.stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        vkDestroySemaphore(device, ready, nullptr); vkDestroySemaphore(device, acquired, nullptr);
        vkDestroyCommandPool(device, pool, nullptr); vkDestroySwapchainKHR(device, swapchain, nullptr);
        vkDestroyDevice(device, nullptr); vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
    } catch (const std::exception &e) { log("FAIL: %s", e.what()); }
    if (logfile) { fclose(logfile); logfile=nullptr; }
    ANativeWindow_release(window);
}
void created(ANativeActivity *activity, ANativeWindow *window) {
    auto &state=*static_cast<State *>(activity->instance);
    if (state.worker.joinable()) return;
    ANativeWindow_acquire(window);
    state.stop=false;
    state.worker=std::thread([&state,window] { run(state,window); });
}
void destroyed(ANativeActivity *activity, ANativeWindow *) {
    auto &state=*static_cast<State *>(activity->instance);
    state.stop=true;
    if (state.worker.joinable()) state.worker.join();
}
void finish(ANativeActivity *activity) {
    destroyed(activity,nullptr);
    delete static_cast<State *>(activity->instance);
    activity->instance=nullptr;
}
}
extern "C" void ANativeActivity_onCreate(ANativeActivity *activity, void *, size_t) {
    activity->instance=new State{activity,native_libraries(activity),{},false};
    activity->callbacks->onNativeWindowCreated=created;
    activity->callbacks->onNativeWindowDestroyed=destroyed;
    activity->callbacks->onDestroy=finish;
}

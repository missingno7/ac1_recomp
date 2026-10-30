/* Android-only loader bootstrap. All Vulkan dispatch stays in this namespace.
 * Keep loaded driver/hook handles alive for the process lifetime. */
#include <dlfcn.h>
#include <vulkan/vulkan.h>
#ifdef AC1_HAS_TURNIP
#include <adrenotools/driver.h>
#endif

extern void dxvkAndroidSetInstanceProcAddr(PFN_vkGetInstanceProcAddr proc);
static void *vulkan_library;
#ifdef AC1_HAS_TURNIP
static void *adrenotools_library;
#endif
static char selected_vulkan_driver[16];

static int verify_vulkan_driver(PFN_vkGetInstanceProcAddr proc, int turnip) {
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)proc(NULL, "vkCreateInstance");
    if (!create) { snprintf(bridge_error,sizeof(bridge_error),"Vulkan loader has no vkCreateInstance"); return 0; }
    VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="AC1 Vulkan bootstrap",.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = create(&ci,NULL,&instance);
    if (r) { snprintf(bridge_error,sizeof(bridge_error),"Vulkan %s vkCreateInstance failed: %d",turnip?"Turnip":"system",r); return 0; }
    PFN_vkDestroyInstance destroy=(PFN_vkDestroyInstance)proc(instance,"vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices enumerate=(PFN_vkEnumeratePhysicalDevices)proc(instance,"vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties2 properties=(PFN_vkGetPhysicalDeviceProperties2)proc(instance,"vkGetPhysicalDeviceProperties2");
    PFN_vkEnumerateDeviceExtensionProperties extensions=(PFN_vkEnumerateDeviceExtensionProperties)proc(instance,"vkEnumerateDeviceExtensionProperties");
    int ok=0;
    if (!destroy||!enumerate||!properties||!extensions) {
        snprintf(bridge_error,sizeof(bridge_error),"Vulkan loader lacks required instance entry points");
    } else {
        uint32_t count=0;
        r=enumerate(instance,&count,NULL);
        if (r||!count) snprintf(bridge_error,sizeof(bridge_error),"Vulkan %s enumeration failed: result=%d devices=%u",turnip?"Turnip":"system",r,count);
        else {
            VkPhysicalDevice *devices=calloc(count,sizeof(*devices));
            if (!devices) snprintf(bridge_error,sizeof(bridge_error),"Vulkan bootstrap device allocation failed");
            else {
                r=enumerate(instance,&count,devices);
                if(r) snprintf(bridge_error,sizeof(bridge_error),"Vulkan physical device list failed: %d",r);
                for(uint32_t i=0;!r&&i<count&&!ok;++i) {
                    VkPhysicalDeviceDriverProperties driver={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
                    VkPhysicalDeviceProperties2 p={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&driver};
                    properties(devices[i],&p);
                    __android_log_print(ANDROID_LOG_INFO,AC1_LOG_TAG,"Vulkan requested=%s device=%s driver=%s info=%s id=%u",turnip?"turnip":"system",p.properties.deviceName,driver.driverName,driver.driverInfo,driver.driverID);
                    fprintf(stderr,"AC1 Vulkan: requested=%s device=%s driver=%s info=%s id=%u\n",turnip?"turnip":"system",p.properties.deviceName,driver.driverName,driver.driverInfo,driver.driverID);
                    if(turnip&&driver.driverID!=VK_DRIVER_ID_MESA_TURNIP) {
                        snprintf(bridge_error,sizeof(bridge_error),"Turnip requested but loader returned driver %s (id=%u); refusing silent system fallback",driver.driverName,driver.driverID); continue;
                    }
                    if(p.properties.apiVersion<VK_API_VERSION_1_3||p.properties.limits.maxPushConstantsSize<256) {
                        snprintf(bridge_error,sizeof(bridge_error),"Vulkan driver requires API >=1.3 and push constants >=256; got api=%x push=%u",p.properties.apiVersion,p.properties.limits.maxPushConstantsSize);continue;
                    }
                    uint32_t n=0;
                    VkResult er=extensions(devices[i],NULL,&n,NULL);
                    VkExtensionProperties *ext=er?NULL:calloc(n,sizeof(*ext));
                    if(!ext) { snprintf(bridge_error,sizeof(bridge_error),"Vulkan extension enumeration/allocation failed: %d",er);continue; }
                    er=extensions(devices[i],NULL,&n,ext);
                    const char *required[]={"VK_KHR_maintenance5","VK_KHR_maintenance6","VK_KHR_load_store_op_none"};
                    ok=er==VK_SUCCESS;
                    for(size_t k=0;ok&&k<sizeof(required)/sizeof(required[0]);++k) {
                        int found=0;for(uint32_t j=0;j<n;++j)if(!strcmp(ext[j].extensionName,required[k]))found=1;
                        if(!found) { snprintf(bridge_error,sizeof(bridge_error),"DXVK 3.1 driver missing mandatory extension %s",required[k]);ok=0; }
                    }
                    if(er)snprintf(bridge_error,sizeof(bridge_error),"Vulkan extension list failed: %d",er);
                    free(ext);
                }
                free(devices);
            }
        }
    }
    if(destroy)destroy(instance,NULL);
    return ok;
}

static int configure_vulkan_driver(void) {
    const char *choice=getenv("AC1_VULKAN_DRIVER");
#ifdef AC1_HAS_TURNIP
    if(!choice||!*choice)choice="turnip";
#else
    if(!choice||!*choice)choice="system";
#endif
    if(strcmp(choice,"system")&&strcmp(choice,"turnip")) { snprintf(bridge_error,sizeof(bridge_error),"Unknown AC1_VULKAN_DRIVER=%.80s (expected turnip or system)",choice);return 0; }
    if(selected_vulkan_driver[0]) {
        if(!strcmp(choice,selected_vulkan_driver))return 1;
        snprintf(bridge_error,sizeof(bridge_error),"Restart the app to change Vulkan driver (%s -> %s)",selected_vulkan_driver,choice);return 0;
    }
    int turnip=!strcmp(choice,"turnip");
    if(turnip) {
#ifdef AC1_HAS_TURNIP
        Dl_info info;
        if(!dladdr((void*)&configure_vulkan_driver,&info)||!info.dli_fname||strstr(info.dli_fname,".apk!")) {
            snprintf(bridge_error,sizeof(bridge_error),"Cannot locate extracted native libraries; Turnip requires useLegacyPackaging=true");return 0;
        }
        char directory[4096],library[4096];
        if(snprintf(directory,sizeof(directory),"%s",info.dli_fname)>=(int)sizeof(directory)) {snprintf(bridge_error,sizeof(bridge_error),"Native library path exceeds 4095 bytes");return 0;}
        char *slash=strrchr(directory,'/');
        if(!slash) {snprintf(bridge_error,sizeof(bridge_error),"Native library directory is not absolute");return 0;}
        slash[1]='\0';
        if(snprintf(library,sizeof(library),"%slibadrenotools.so",directory)>=(int)sizeof(library)) {snprintf(bridge_error,sizeof(bridge_error),"adrenotools library path is too long");return 0;}
        adrenotools_library=dlopen(library,RTLD_NOW|RTLD_LOCAL);
        if(!adrenotools_library) {snprintf(bridge_error,sizeof(bridge_error),"Cannot load libadrenotools: %.350s",dlerror());return 0;}
        typedef void *(*OpenDriver)(int,int,const char*,const char*,const char*,const char*,const char*,void**);
        OpenDriver open_driver=(OpenDriver)dlsym(adrenotools_library,"adrenotools_open_libvulkan");
        if(!open_driver) {snprintf(bridge_error,sizeof(bridge_error),"libadrenotools lacks adrenotools_open_libvulkan");return 0;}
        if(snprintf(library,sizeof(library),"%slibvulkan_freedreno.so",directory)>=(int)sizeof(library)||access(library,R_OK)!=0) {snprintf(bridge_error,sizeof(bridge_error),"Packaged Turnip libvulkan_freedreno.so is missing/unreadable: %s",strerror(errno));return 0;}
        dlerror();
        vulkan_library=open_driver(RTLD_NOW|RTLD_LOCAL,ADRENOTOOLS_DRIVER_CUSTOM,NULL,directory,directory,"libvulkan_freedreno.so",NULL,NULL);
        if(!vulkan_library) {const char *error=dlerror();snprintf(bridge_error,sizeof(bridge_error),"adrenotools Turnip loader failed: %.350s",error?error:"no dlerror; inspect hook libraries and linker namespace");return 0;}
#else
        snprintf(bridge_error,sizeof(bridge_error),"This ABI has no packaged Turnip provider");return 0;
#endif
    } else {
        vulkan_library=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
        if(!vulkan_library) {snprintf(bridge_error,sizeof(bridge_error),"System Vulkan loader failed: %.350s",dlerror());return 0;}
    }
    PFN_vkGetInstanceProcAddr proc=(PFN_vkGetInstanceProcAddr)dlsym(vulkan_library,"vkGetInstanceProcAddr");
    if(!proc) {snprintf(bridge_error,sizeof(bridge_error),"Selected Vulkan loader lacks vkGetInstanceProcAddr");return 0;}
    if(!verify_vulkan_driver(proc,turnip))return 0;
    dxvkAndroidSetInstanceProcAddr(proc);
    snprintf(selected_vulkan_driver,sizeof(selected_vulkan_driver),"%s",choice);
    return 1;
}

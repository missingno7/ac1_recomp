// Android surface/mode policy adapted from codepdbh/nfsu2-recompiled (MIT).
// Copyright (c) codepdbh. Upstream DXVK 3.1 WsiDriver integration is AC1 code.
#include <android/native_window.h>
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include "src/wsi/wsi_platform.h"
#include "android_resolution.h"

namespace {
std::mutex windowMutex;
ANativeWindow* currentWindow = nullptr;
uintptr_t windowGeneration = 0x1000;
HWND currentHandle = nullptr;
uint32_t screenWidth=1280, screenHeight=720;
uint32_t renderWidth=0, renderHeight=0, bufferWidth=0, bufferHeight=0;
std::atomic<PFN_vkGetInstanceProcAddr> instanceProcAddr{nullptr};
HMONITOR monitor() { return reinterpret_cast<HMONITOR>(uintptr_t{1}); }
void setBufferSize(uint32_t w, uint32_t h) {
  if (currentWindow) {
    int result=ANativeWindow_setBuffersGeometry(currentWindow,w,h,0);
    if (!result) { bufferWidth=w; bufferHeight=h; }
    else __android_log_print(ANDROID_LOG_ERROR,"DXVK","ANativeWindow_setBuffersGeometry(%u,%u) failed: %d",w,h,result);
  }
}
}
extern "C" void dxvkAndroidSetInstanceProcAddr(PFN_vkGetInstanceProcAddr entry) { instanceProcAddr.store(entry); }
extern "C" PFN_vkGetInstanceProcAddr dxvkAndroidGetInstanceProcAddr() { return instanceProcAddr.load(); }
extern "C" void dxvkAndroidSetRenderSize(uint32_t w,uint32_t h) {
  std::lock_guard<std::mutex> lock(windowMutex); renderWidth=w; renderHeight=h;
  if(currentWindow&&!bufferWidth&&w&&h&&w<screenWidth&&h<screenHeight) setBufferSize(w,h);
}
extern "C" HWND dxvkAndroidGetWindowHandle() { std::lock_guard<std::mutex> lock(windowMutex); return currentHandle; }
extern "C" void dxvkAndroidSetWindow(ANativeWindow* window) {
  std::lock_guard<std::mutex> lock(windowMutex);
  if(window==currentWindow) return;
  if(window) { ANativeWindow_acquire(window); screenWidth=ANativeWindow_getWidth(window); screenHeight=ANativeWindow_getHeight(window); }
  if(currentWindow) ANativeWindow_release(currentWindow);
  currentWindow=window; bufferWidth=bufferHeight=0;
  if(window&&renderWidth&&renderHeight&&renderWidth<screenWidth&&renderHeight<screenHeight) setBufferSize(renderWidth,renderHeight);
  currentHandle=window?reinterpret_cast<HWND>(++windowGeneration):nullptr;
}

namespace dxvk::wsi {
class AndroidDriver final : public WsiDriver {
public:
  std::vector<const char*> getInstanceExtensions() override { return {VK_KHR_SURFACE_EXTENSION_NAME,VK_KHR_ANDROID_SURFACE_EXTENSION_NAME}; }
  HMONITOR getDefaultMonitor() override { return monitor(); }
  HMONITOR enumMonitors(uint32_t i) override { return i?nullptr:monitor(); }
  HMONITOR enumMonitors(const LUID*[],uint32_t,uint32_t i) override { return enumMonitors(i); }
  bool getDisplayName(HMONITOR h,WCHAR (&name)[32]) override {
    if(h!=monitor())return false;
    constexpr wchar_t display[]=L"\\\\.\\DISPLAY1";
    std::fill(std::begin(name),std::end(name),0); std::copy(std::begin(display),std::end(display),name); return true;
  }
  bool getDesktopCoordinates(HMONITOR h,RECT* rect) override {
    if(h!=monitor()||!rect)return false;
    std::lock_guard<std::mutex> lock(windowMutex); *rect={0,0,LONG(screenWidth),LONG(screenHeight)}; return true;
  }
  bool getCurrentDisplayMode(HMONITOR h,WsiMode* mode) override {
    if(h!=monitor()||!mode)return false;
    std::lock_guard<std::mutex> lock(windowMutex); *mode={screenWidth,screenHeight,{60,1},32,false}; return true;
  }
  bool getDesktopDisplayMode(HMONITOR h,WsiMode* mode) override { return getCurrentDisplayMode(h,mode); }
  bool getDisplayMode(HMONITOR h,uint32_t i,WsiMode* mode) override {
    if(h!=monitor()||!mode)return false;
    std::lock_guard<std::mutex> lock(windowMutex);
    if(!renderWidth||!renderHeight) {
      static constexpr AndroidRenderMode pcModes[]={{640,480},{800,600},{1024,768},{1280,960},{1280,1024},{1600,1200}};
      if(i>6)return false;
      auto s=i==6?AndroidRenderMode{screenWidth,screenHeight}:pcModes[i];
      *mode={s.width,s.height,{60,1},32,false}; return true;
    }
    if(i>=6)return false;
    auto s=androidRenderModes(screenWidth,screenHeight,renderWidth,renderHeight)[i];
    *mode={s.width,s.height,{60,1},32,false}; return true;
  }
  WsiEdidData getMonitorEdid(HMONITOR) override { return {}; }
  void getWindowSize(HWND w,uint32_t* width,uint32_t* height) override {
    std::lock_guard<std::mutex> lock(windowMutex); bool valid=currentWindow&&w==currentHandle;
    if(width)*width=valid?(bufferWidth?bufferWidth:screenWidth):0;
    if(height)*height=valid?(bufferHeight?bufferHeight:screenHeight):0;
  }
  void resizeWindow(HWND w,DxvkWindowState*,uint32_t width,uint32_t height) override {
    std::lock_guard<std::mutex> lock(windowMutex); if(currentWindow&&w==currentHandle)setBufferSize(width,height);
  }
  void saveWindowState(HWND,DxvkWindowState*,bool) override {}
  void restoreWindowState(HWND,DxvkWindowState*,bool) override {}
  bool setWindowMode(HMONITOR h,HWND w,DxvkWindowState*,const WsiMode&) override { return h==monitor()&&isWindow(w); }
  bool enterFullscreenMode(HMONITOR h,HWND w,DxvkWindowState*,bool) override { return h==monitor()&&isWindow(w); }
  bool leaveFullscreenMode(HWND w,DxvkWindowState*) override { return isWindow(w); }
  bool restoreDisplayMode() override { return true; }
  HMONITOR getWindowMonitor(HWND w) override { return isWindow(w)?monitor():nullptr; }
  bool isWindow(HWND w) override { std::lock_guard<std::mutex> lock(windowMutex);return currentWindow&&w==currentHandle; }
  bool isMinimized(HWND w) override { return !isWindow(w); }
  bool isOccluded(HWND w) override { return !isWindow(w); }
  void updateFullscreenWindow(HMONITOR,HWND,bool) override {}
  VkResult createSurface(HWND w,PFN_vkGetInstanceProcAddr proc,VkInstance instance,VkSurfaceKHR* surface) override {
    std::lock_guard<std::mutex> lock(windowMutex);
    if(!currentWindow||w!=currentHandle)return VK_ERROR_SURFACE_LOST_KHR;
    auto create=reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(proc(instance,"vkCreateAndroidSurfaceKHR"));
    if(!create)return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};info.window=currentWindow;
    return create(instance,&info,nullptr,surface);
  }
};
static bool createAndroidDriver(WsiDriver** driver) { *driver=new AndroidDriver; return true; }
WsiBootstrap AndroidWSI={"Android",createAndroidDriver};
}

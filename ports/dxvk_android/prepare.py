"""Prepare pinned upstream DXVK 3.1 for Android; preserve Vulkan requirements."""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess

PIN = "70d7508c01201ed3d4bfb33da42ba834eafe3857"
SUBMODULES = {
    "include/native/directx": "9df86f2341616ef1888ae59919feaa6d4fad693d",
    "include/vulkan": "8864cdc896bbc2a9b6eb36b3218fc9ef57908d77",
    "include/spirv": "04f10f650d514df88b76d25e83db360142c7b174",
    "subprojects/dxbc-spirv": "37a97745bddaf56d717253b0e4565904ce5eb06c",
    "subprojects/libdisplay-info": "275e6459c7ab1ddd4b125f28d0440716e4888078",
    "subprojects/dxbc-spirv/submodules/spirv_headers": "c8ad050fcb29e42a2f57d9f59e97488f465c436d",
}


def revision(path):
    return subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()


def verify(source):
    if revision(source) != PIN:
        raise SystemExit("Unexpected official DXVK source revision")
    for directory, pin in SUBMODULES.items():
        path = source / directory
        # Connected GitHub snapshots support sandboxes unable to run git's
        # shell-based submodule helper. Check each file against its Git blob.
        snapshot = path / "AC1_SOURCE_SNAPSHOT.json"
        if snapshot.is_file():
            manifest = json.loads(snapshot.read_text())
            if manifest["revision"] != pin:
                raise SystemExit("Unexpected snapshot revision: " + directory)
            for name, expected in manifest["files"].items():
                data = (path / name).read_bytes().replace(b"\r\n", b"\n")
                if not any(hashlib.sha1(b"blob " + str(len(b)).encode() + b"\0" + b).hexdigest() == expected
                           for b in (data, data.rstrip(b"\n"))):
                    raise SystemExit("Pinned source blob mismatch: " + str(path / name))
        elif not (path / ".git").exists() or revision(path) != pin:
            raise SystemExit("Missing/unexpected submodule: " + directory + "; run git submodule update --init --recursive")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    verify(args.source)
    args.output.mkdir(parents=True, exist_ok=True)
    # Fresh timestamps: a file whose patch was removed must still look newer than
    # objects built from the patched copy, so the next build recompiles it.
    for directory in ("src", "include", "subprojects"):
        shutil.copytree(args.source / directory, args.output / directory, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns(".git"), copy_function=shutil.copy)
    shutil.copy2(args.source / "LICENSE", args.output / "LICENSE")

    def replace(path, old, new):
        file = args.output / path
        content = file.read_text()
        if old not in content:
            raise SystemExit("Patch mismatch: " + path)
        file.write_text(content.replace(old, new), encoding="utf-8", newline="\n")

    replace("src/wsi/wsi_platform.h", "#if defined(DXVK_WSI_WIN32)",
            "#if defined(DXVK_WSI_ANDROID)\n  extern WsiBootstrap AndroidWSI;\n#endif\n#if defined(DXVK_WSI_WIN32)")
    replace("src/wsi/wsi_platform.cpp", "  static const WsiBootstrap *wsiBootstrap[] = {",
            "  static const WsiBootstrap *wsiBootstrap[] = {\n#if defined(DXVK_WSI_ANDROID)\n    &AndroidWSI,\n#endif")
    # Preserve loader identity for every instance/device/surface entry point.
    replace("src/vulkan/vulkan_loader.cpp", "  LibraryLoader::LibraryLoader() {\n    std::tie(m_library, m_getInstanceProcAddr) = loadVulkanLibrary();\n  }",
            '  extern "C" PFN_vkGetInstanceProcAddr dxvkAndroidGetInstanceProcAddr();\n\n'
            '  LibraryLoader::LibraryLoader() {\n'
            '    m_getInstanceProcAddr = dxvkAndroidGetInstanceProcAddr();\n'
            '    if (!m_getInstanceProcAddr)\n'
            '      throw DxvkError("Android Vulkan loader was not initialized before DXVK");\n  }')
    # Backport of upstream 0847af3: the allocation pool is an unlocked free list.
    # Swapchain images are imported on the game thread while other DXVK threads
    # allocate and free under the allocator lock; the 2026-10-31 phone crash came
    # from that race handing out one pool slot twice.
    lock = "    std::lock_guard lock(m_mutex);\n\n"
    create = "    Rc<DxvkResourceAllocation> allocation = m_allocationPool.create(this, nullptr);\n"
    for head in ("importInfo) {\n", "imageHandle) {\n"):
        replace("src/dxvk/dxvk_memory.cpp", head + create, head + lock + create)
    sparse = "sparsePageTable,\n    const DxvkAllocationInfo&   allocationInfo) {\n"
    replace("src/dxvk/dxvk_memory.cpp", sparse + "    auto allocation", sparse + lock + "    auto allocation")
    replace("src/util/log/log.cpp", '#include "log.h"', '#include "log.h"\n#include <android/log.h>')
    replace("src/util/log/log.cpp", "std::cerr << adjusted;",
            'std::cerr << adjusted;\n'
            '        if (level >= LogLevel::Warn)\n'
            '          __android_log_print(level >= LogLevel::Error ? ANDROID_LOG_ERROR : ANDROID_LOG_WARN, "DXVK", "%s", adjusted.c_str());')
    # Android reports VK_SUBOPTIMAL_KHR on every present while the identity
    # pre-transform differs from the rotated landscape surface, so the presenter
    # rebuilt its swapchain once per frame. The compositor applies the rotation;
    # keep the swapchain while the surface extent still matches its images.
    keep = ("#ifdef DXVK_WSI_ANDROID\n"
            "    {status} = keepTransformedSwapchain({status}, m_vki, m_device->adapter()->handle(), m_surface, m_images);\n"
            "#endif\n")
    replace("src/dxvk/dxvk_presenter.cpp", "  VkResult Presenter::checkSwapChainStatus() {",
            "#ifdef DXVK_WSI_ANDROID\n"
            "  static VkResult keepTransformedSwapchain(VkResult status, const Rc<vk::InstanceFn>& vki,\n"
            "      VkPhysicalDevice adapter, VkSurfaceKHR surface, const std::vector<Rc<DxvkImage>>& images) {\n"
            "    VkSurfaceCapabilitiesKHR caps = { };\n"
            "    if (status != VK_SUBOPTIMAL_KHR || images.empty()\n"
            "     || vki->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(adapter, surface, &caps))\n"
            "      return status;\n"
            "    VkExtent3D extent = images.front()->info().extent;\n"
            "    return caps.currentExtent.width == extent.width && caps.currentExtent.height == extent.height\n"
            "      ? VK_SUCCESS : status;\n"
            "  }\n"
            "#endif\n\n\n"
            "  VkResult Presenter::checkSwapChainStatus() {")
    present = "    VkResult status = m_vkd->vkQueuePresentKHR(\n      m_device->queues().graphics.queueHandle, &info);\n"
    replace("src/dxvk/dxvk_presenter.cpp", present, present + keep.format(status="status"))
    acquired = "    // This is a normal occurence, but may be useful for\n"
    replace("src/dxvk/dxvk_presenter.cpp", acquired, keep.format(status="m_acquireStatus") + acquired)
    (args.output / "version.h").write_text('#define DXVK_VERSION "v3.1-android"\n', encoding="ascii")
    (args.output / "buildenv.h").write_text('#define DXVK_COMPILER "clang"\n#define DXVK_COMPILER_VERSION "NDK"\n#define DXVK_TARGET "Android"\n', encoding="ascii")
    (args.output / "PINNED_REVISION").write_text(PIN + "\n", encoding="ascii")
    (args.output / "ANDROID_PREPARE_SHA256").write_text(hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest() + "\n", encoding="ascii")
    print("Prepared official DXVK 3.1 " + PIN)


if __name__ == "__main__":
    main()

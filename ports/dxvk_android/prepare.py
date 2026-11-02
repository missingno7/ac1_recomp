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


QUERY_STATS_H = r'''#pragma once

// AC1 opt-in query wait accounting (AC1_DXVK_QUERY_STATS=1). AC1 spins on
// occlusion GetData(FLUSH); every pending poll is attributed to the first
// unfinished step after Issue(END), and waits are timed from the first poll.
// Clock reads happen only on step changes.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <ctime>

namespace dxvk::ac1 {

  enum QueryWait : uint32_t {
    WaitBegun,       // the game polled a query it has not ended
    WaitCsEnd,       // the CS thread has not recorded the end
    WaitNotFlushed,  // no flush containing the end was emitted
    WaitCsFlush,     // the flush was emitted, the CS thread has not run it
    WaitQueued,      // handed to the submit thread, not yet vkQueueSubmit
    WaitGpu,         // submitted, the GPU has not completed that submission
    WaitVk,          // submission complete, Vulkan result still not ready
    WaitCount
  };

  struct QueryStats {
    uint64_t calls, other, cached, ready, readyFirst, readyEarly, waited, pollFlushes;
    uint64_t polls[WaitCount];
    uint64_t beforePoll, stage[WaitCount], latency, maxLatency;
    uint64_t windowStart;
  };

  inline bool queryStatsEnabled() {
    static const bool enabled = [] {
      const char* value = std::getenv("AC1_DXVK_QUERY_STATS");
      return value && *value == '1';
    }();
    return enabled;
  }

  inline uint64_t queryNow() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
  }

  inline QueryStats& queryStats() {
    static QueryStats stats = { };
    return stats;
  }

  // Command lists handed to and submitted by DXVK's submit thread (FIFO), and
  // for each D3D9 flush id (mod 64) the hand-off count that includes it.
  inline std::atomic<uint64_t> queryEnqueued = { 0ull };
  inline std::atomic<uint64_t> querySubmitted = { 0ull };
  inline std::atomic<uint64_t> queryHandoff[64] = { };

}
'''


def query_stats(replace, output):
    """Opt-in counters that classify why each AC1 occlusion poll is pending."""
    (output / "src/util/ac1_query_stats.h").write_text(QUERY_STATS_H, encoding="utf-8", newline="\n")
    q_h, q_cpp, d_h, d_cpp, queue = ("src/d3d9/d3d9_query.h", "src/d3d9/d3d9_query.cpp",
                                     "src/d3d9/d3d9_device.h", "src/d3d9/d3d9_device.cpp",
                                     "src/dxvk/dxvk_queue.cpp")
    replace(queue, '#include "dxvk_device.h"', '#include "dxvk_device.h"\n#include "../util/ac1_query_stats.h"')
    replace(queue, "    m_submitQueue.push(std::move(entry));\n    m_appendCond.notify_all();\n  }\n\n\n  void DxvkSubmissionQueue::present(",
            "    m_submitQueue.push(std::move(entry));\n"
            "    if (unlikely(ac1::queryStatsEnabled()))\n"
            "      ac1::queryEnqueued.fetch_add(1, std::memory_order_relaxed);\n"
            "    m_appendCond.notify_all();\n  }\n\n\n  void DxvkSubmissionQueue::present(")
    replace(queue, "          entry.timelines = m_timelines;\n",
            "          entry.timelines = m_timelines;\n"
            "          if (unlikely(ac1::queryStatsEnabled()))\n"
            "            ac1::querySubmitted.fetch_add(1, std::memory_order_release);\n")
    replace(q_h, '#include "d3d9_device_child.h"', '#include "d3d9_device_child.h"\n#include "../util/ac1_query_stats.h"')
    replace(q_h, "    void NotifyStall() {",
            "    void Ac1NoteEnd(uint64_t submission) {\n"
            "      m_ac1EndSubmission = submission;\n"
            "      m_ac1Stage = ac1::WaitCsEnd;\n"
            "      m_ac1Polled = false;\n"
            "      m_ac1EndNs = ac1::queryNow();\n"
            "    }\n\n"
            "    void NotifyStall() {")
    replace(q_h, "    D3D9_QUERY_DATA m_dataCache;\n",
            "    D3D9_QUERY_DATA m_dataCache;\n\n"
            "    uint64_t m_ac1EndSubmission = 0;\n"
            "    uint64_t m_ac1EndNs = 0;\n"
            "    uint64_t m_ac1Marks[ac1::WaitCount] = { };\n"
            "    uint32_t m_ac1Stage = ac1::WaitCsEnd;\n"
            "    bool     m_ac1Polled = false;\n\n"
            "    void Ac1Account(HRESULT hr, uint64_t submissionBefore);\n")
    replace(d_h, "    void ConsiderFlush(GpuFlushType FlushType);\n",
            "    void ConsiderFlush(GpuFlushType FlushType);\n\n"
            "    uint64_t Ac1SubmissionId() const { return m_submissionId; }\n"
            "    uint64_t Ac1CsFlushed() const { return m_ac1CsFlushed.load(std::memory_order_acquire); }\n"
            "    uint64_t Ac1GpuDone() const { return m_submissionFence->value(); }\n")
    replace(d_h, "    uint64_t                        m_submissionId = 0ull;\n",
            "    uint64_t                        m_submissionId = 0ull;\n"
            "    std::atomic<uint64_t>           m_ac1CsFlushed = { 0ull };\n")
    replace(d_cpp, "      cStagingBufferAllocated = m_stagingMemorySignaled\n    ] (DxvkContext* ctx) {",
            "      cStagingBufferAllocated = m_stagingMemorySignaled,\n"
            "      cAc1CsFlushed = &m_ac1CsFlushed\n    ] (DxvkContext* ctx) {")
    replace(d_cpp, "      ctx->flushCommandList(nullptr, cSubmissionStatus);\n    });",
            "      ctx->flushCommandList(nullptr, cSubmissionStatus);\n"
            "      if (unlikely(ac1::queryStatsEnabled())) {\n"
            "        ac1::queryHandoff[cSubmissionId & 63u].store(\n"
            "          ac1::queryEnqueued.load(std::memory_order_relaxed), std::memory_order_relaxed);\n"
            "        cAc1CsFlushed->store(cSubmissionId, std::memory_order_release);\n"
            "      }\n    });")
    replace(d_cpp, "    pQuery->NotifyEnd();\n",
            "    if (unlikely(ac1::queryStatsEnabled()))\n"
            "      pQuery->Ac1NoteEnd(m_submissionId + 1);\n\n"
            "    pQuery->NotifyEnd();\n")
    # GetData: count every call, attribute pending polls, report every ~2 s.
    replace(q_cpp, "    if (m_state == D3D9_VK_QUERY_CACHED) {\n      // Query data was already retrieved once.",
            "    const bool ac1Stats = ac1::queryStatsEnabled();\n"
            "    const uint64_t ac1Submission = ac1Stats ? m_parent->Ac1SubmissionId() : 0;\n\n"
            "    if (m_state == D3D9_VK_QUERY_CACHED) {\n"
            "      if (unlikely(ac1Stats))\n"
            "        Ac1Account(D3D_OK, ac1Submission);\n"
            "      // Query data was already retrieved once.")
    replace(q_cpp, "      m_parent->ConsiderFlush(GpuFlushType::ImplicitSynchronization);\n    }\n\n    return hr;\n  }",
            "      m_parent->ConsiderFlush(GpuFlushType::ImplicitSynchronization);\n    }\n\n"
            "    if (unlikely(ac1Stats))\n"
            "      Ac1Account(hr, ac1Submission);\n\n"
            "    return hr;\n  }\n\n\n"
            + AC1_ACCOUNT)


AC1_ACCOUNT = r'''  void D3D9Query::Ac1Account(HRESULT hr, uint64_t submissionBefore) {
    auto& s = ac1::queryStats();
    s.calls++;
    if (m_queryType != D3DQUERYTYPE_OCCLUSION) {
      s.other++;
      return;
    }
    s.pollFlushes += m_parent->Ac1SubmissionId() != submissionBefore;

    if (hr == S_FALSE) {
      uint32_t stage = ac1::WaitVk;
      if (m_state == D3D9_VK_QUERY_BEGUN)
        stage = ac1::WaitBegun;
      else if (m_resetCtr != 0u)
        stage = ac1::WaitCsEnd;
      else if (submissionBefore < m_ac1EndSubmission)
        stage = ac1::WaitNotFlushed;
      else if (m_parent->Ac1CsFlushed() < m_ac1EndSubmission)
        stage = ac1::WaitCsFlush;
      else if (ac1::querySubmitted.load(std::memory_order_acquire)
             < ac1::queryHandoff[m_ac1EndSubmission & 63u].load(std::memory_order_relaxed))
        stage = ac1::WaitQueued;
      else if (m_parent->Ac1GpuDone() < m_ac1EndSubmission)
        stage = ac1::WaitGpu;
      s.polls[stage]++;

      if (stage != ac1::WaitBegun) {
        if (!m_ac1Polled || stage > m_ac1Stage) {
          uint64_t now = ac1::queryNow();
          if (!m_ac1Polled)
            m_ac1Marks[0] = now;
          for (uint32_t i = m_ac1Stage; i < stage; i++)
            m_ac1Marks[i + 1] = now;
          m_ac1Stage = stage;
        }
        m_ac1Polled = true;
      }
    } else if (hr == D3D_OK && m_ac1EndNs) {
      // First successful read after an end.
      uint64_t now = ac1::queryNow();
      s.ready++;
      // Ready before DXVK's fence for the submission holding the end: a gate
      // on that fence would have delayed this result.
      s.readyEarly += m_parent->Ac1GpuDone() < m_ac1EndSubmission;
      if (!m_ac1Polled) {
        s.readyFirst++;
      } else {
        // m_ac1Marks[i + 1]: first poll that saw step i finished; m_ac1Marks[0]
        // is the first poll. Time before it is the game's own work, not a wait.
        for (uint32_t i = m_ac1Stage; i < ac1::WaitCount - 1; i++)
          m_ac1Marks[i + 1] = now;
        s.waited++;
        s.beforePoll += m_ac1Marks[0] - m_ac1EndNs;
        for (uint32_t i = ac1::WaitCsEnd; i < ac1::WaitVk; i++)
          s.stage[i] += m_ac1Marks[i + 1] - m_ac1Marks[0];
        s.latency += now - m_ac1Marks[0];
        s.maxLatency = std::max(s.maxLatency, now - m_ac1Marks[0]);
      }
      m_ac1EndNs = 0;
    } else if (hr == D3D_OK) {
      s.cached++;
    }

    if ((s.calls & 4095u) == 0u) {
      uint64_t now = ac1::queryNow();
      if (!s.windowStart)
        s.windowStart = now;
      if (now - s.windowStart >= 2000000000ull) {
        double secs = double(now - s.windowStart) * 1e-9;
        double w = s.waited ? 1e-3 / double(s.waited) : 0.0;
        Logger::warn(str::format("ac1-query ", secs, "s: calls/s ", uint64_t(s.calls / secs),
          " non-occlusion/s ", uint64_t(s.other / secs),
          " ready/s ", uint64_t(s.ready / secs), " firstpoll-ready/s ", uint64_t(s.readyFirst / secs),
          " before-fence/s ", uint64_t(s.readyEarly / secs),
          " waited/s ", uint64_t(s.waited / secs), " cached/s ", uint64_t(s.cached / secs),
          " poll-flush/s ", uint64_t(s.pollFlushes / secs),
          " | pending polls/s begun ", uint64_t(s.polls[ac1::WaitBegun] / secs),
          " cs-end ", uint64_t(s.polls[ac1::WaitCsEnd] / secs),
          " unflushed ", uint64_t(s.polls[ac1::WaitNotFlushed] / secs),
          " cs-flush ", uint64_t(s.polls[ac1::WaitCsFlush] / secs),
          " queued ", uint64_t(s.polls[ac1::WaitQueued] / secs),
          " gpu ", uint64_t(s.polls[ac1::WaitGpu] / secs),
          " vk ", uint64_t(s.polls[ac1::WaitVk] / secs),
          " | waited avg us from first GetData: cs-end ", uint64_t(double(s.stage[ac1::WaitCsEnd]) * w),
          " flushed ", uint64_t(double(s.stage[ac1::WaitNotFlushed]) * w),
          " cs-flushed ", uint64_t(double(s.stage[ac1::WaitCsFlush]) * w),
          " submitted ", uint64_t(double(s.stage[ac1::WaitQueued]) * w),
          " gpu-done ", uint64_t(double(s.stage[ac1::WaitGpu]) * w),
          " ready ", uint64_t(double(s.latency) * w),
          " max ", s.maxLatency / 1000u, " | spin ms/s ", uint64_t(double(s.latency) * 1e-6 / secs),
          " | end-to-first-GetData avg us (game work) ", uint64_t(double(s.beforePoll) * w)));
        s = ac1::QueryStats();
        s.windowStart = now;
      }
    }
  }
'''


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
    query_stats(replace, args.output)
    (args.output / "version.h").write_text('#define DXVK_VERSION "v3.1-android"\n', encoding="ascii")
    (args.output / "buildenv.h").write_text('#define DXVK_COMPILER "clang"\n#define DXVK_COMPILER_VERSION "NDK"\n#define DXVK_TARGET "Android"\n', encoding="ascii")
    (args.output / "PINNED_REVISION").write_text(PIN + "\n", encoding="ascii")
    (args.output / "ANDROID_PREPARE_SHA256").write_text(hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest() + "\n", encoding="ascii")
    print("Prepared official DXVK 3.1 " + PIN)


if __name__ == "__main__":
    main()

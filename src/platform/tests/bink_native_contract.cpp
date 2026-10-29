// Standalone provider/guest-memory/PCM contract and FFmpeg-reference comparison.
// Build/run instructions: ports/ffmpeg/README.md. All inputs/outputs stay private.
#include "../ac1_bink_native.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

struct Sink {
#ifdef _WIN32
    void *native_audio = nullptr;
    ~Sink() { ac1_bink_audio_destroy(native_audio); }
#endif
    std::array<uint8_t, 0x10000> memory{};
    uint64_t now = 1000000;
    std::map<uint32_t, std::vector<int16_t>> pcm;
    std::map<uint32_t, uint64_t> frames;
    std::map<uint32_t, uint32_t> rates, channels;
    int controls[4]{};
    bool reject_write = false, reject_audio = false;
    static int write(void *opaque, uint32_t address, const void *data, size_t size) {
        auto &s = *static_cast<Sink *>(opaque);
        if (s.reject_write || uint64_t(address) + size > s.memory.size()) return 0;
        std::memcpy(s.memory.data() + address, data, size); return 1;
    }
    static uint64_t clock(void *opaque) { return static_cast<Sink *>(opaque)->now; }
    static int audio(void *opaque, uint32_t, uint32_t id, const int16_t *pcm, size_t n,
                     uint32_t rate, uint32_t channels, uint64_t pts) {
        auto &s = *static_cast<Sink *>(opaque);
        if (s.reject_audio || !rate || !channels || pts != s.frames[id] * 1000000 / rate) return 0;
        if (s.rates.count(id) && (s.rates[id] != rate || s.channels[id] != channels)) return 0;
#ifdef _WIN32
        if (s.native_audio && !ac1_bink_audio_submit(s.native_audio, 0x1000, id, pcm, n, rate, channels, pts)) return 0;
#endif
        s.rates[id] = rate; s.channels[id] = channels;
        s.frames[id] += n;
        s.pcm[id].insert(s.pcm[id].end(), pcm, pcm + n * channels); return 1;
    }
    static void control(void *opaque, uint32_t, int event) {
        auto &s = *static_cast<Sink *>(opaque);
#ifdef _WIN32
        ac1_bink_audio_control(s.native_audio, 0x1000, event);
#endif
        if (event >= 1 && event <= 3) ++s.controls[event];
        if (event == AC1_BINK_AUDIO_STOP) s.frames.clear();
    }
    uint32_t word(size_t a) const {
        return uint32_t(memory[a]) | uint32_t(memory[a+1]) << 8 |
               uint32_t(memory[a+2]) << 16 | uint32_t(memory[a+3]) << 24;
    }
};
static int file_read(void *opaque, uint8_t *buffer, int bytes) {
    auto *f = static_cast<FILE *>(opaque);
    size_t got = std::fread(buffer,1,size_t(bytes),f);
    return std::ferror(f) ? -1 : int(got);
}
static int64_t file_seek(void *opaque, int64_t offset, int whence) {
    auto *f = static_cast<FILE *>(opaque);
    if (offset < LONG_MIN || offset > LONG_MAX || std::fseek(f,long(offset),whence)) return -1;
    return std::ftell(f);
}
static uint64_t hash(const std::vector<uint8_t> &data) {
    uint64_t h = UINT64_C(14695981039346656037);
    for (auto c : data) { h ^= c; h *= UINT64_C(1099511628211); }
    return h;
}
static void require(bool ok, const char *message, ac1_bink_provider *b) {
    if (!ok) throw std::runtime_error(std::string(message) + ": " + ac1_bink_error(b));
}
int main(int argc, char **argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: bink_native_contract VIDEO REFERENCE.bgra FRAMES PCM_OUTPUT_PREFIX\n");
        return 2;
    }
    Sink sink;
#ifdef _WIN32
    if (std::getenv("AC1_BINK_TEST_AUDIO")) {
        char error[192]; sink.native_audio = ac1_bink_audio_create(error,sizeof error);
        if (!sink.native_audio) { std::fprintf(stderr,"FAIL DirectSound sink: %s\n",error); return 1; }
        std::printf("native DirectSound streaming sink enabled\n");
    }
#endif
    ac1_bink_callbacks cb{&sink, Sink::write, Sink::clock, Sink::audio, Sink::control};
    std::unique_ptr<ac1_bink_provider, decltype(&ac1_bink_destroy)> owner(ac1_bink_create(&cb), ac1_bink_destroy);
    auto *b = owner.get();
    if (!b) return 2;
    constexpr uint32_t rec = 0x1000, info = 0x2000;
    try {
        uint32_t track = 0;
        require(ac1_bink_set_memory(b, 0x9e8d90, 0x9e90d0) == 1, "guest alloc metadata", b);
        require(ac1_bink_set_sound_tracks(b, 1, &track) == 1, "select AC1 track ID 0", b);
        require(ac1_bink_open(b, rec, argv[1], 0x00800000) == -1, "reject file-handle open", b);
        require(ac1_bink_open(b, rec, "https://example.invalid/test.bik", 0x4000) == -1, "reject network input", b);
        std::unique_ptr<FILE, decltype(&std::fclose)> input(std::fopen(argv[1],"rb"),std::fclose);
        require(bool(input), "open callback IO test input", b);
        require(std::fseek(input.get(),0,SEEK_END) == 0, "measure callback input", b);
        long size = std::ftell(input.get());
        require(size > 0 && std::fseek(input.get(),0,SEEK_SET) == 0, "rewind callback input", b);
        ac1_bink_input io{input.get(),uint64_t(size),file_read,file_seek};
        require(ac1_bink_open_input(b, rec, &io, 0x4000) == 1, "open AC1 movie through callback IO", b);
        uint32_t w = sink.word(rec), h = sink.word(rec+4), count = sink.word(rec+8);
        int n = std::atoi(argv[3]);
        require(n >= 2 && uint32_t(n) <= count, "frame count in range", b);
        require(w == 1280 && h == 720 && sink.word(rec+0xc) == 1 && sink.word(rec+0x10) == 0,
                "BINK prefix and counters", b);
        uint32_t fps = sink.word(rec+0x14), div = sink.word(rec+0x18);
        require(fps > 0 && div > 0, "real frame rate (not recomp-kit counter aliases)", b);
        require(ac1_bink_get_frame_buffers_info(b, rec, info) == 1 && sink.word(info) == 2 &&
                sink.word(info+4) == w && sink.word(info+8) == h && sink.word(info+0x14) == 0 &&
                sink.word(info+0x18) == 1 && sink.word(info+0x1c) == 0 && sink.word(info+0x48) == 1,
                "BINKFRAMEBUFFERS layout", b);
        uint32_t pitch = w * 4; // AC1 takes D3DLOCKED_RECT.Pitch; 1280 BGRA uses 5120.
        std::vector<uint8_t> image(size_t(pitch) * h), ref(image.size());
        std::ifstream reference(argv[2], std::ios::binary);
        require(bool(reference), "open reference BGRA", b);
        double total_sse = 0;
        uint64_t first_hash = 0;
        for (int frame = 1; frame <= n; ++frame) {
            require(sink.word(rec+0xc) == uint32_t(frame), "1-based FrameNum", b);
            require(ac1_bink_do_frame(b, rec) == 1, "DoFrame", b);
            size_t audio_size = sink.pcm[0].size();
            require(ac1_bink_do_frame(b, rec) == 1 && sink.pcm[0].size() == audio_size,
                    "repeated DoFrame retains frame and PCM", b);
            require(sink.word(rec+0x10) == uint32_t(frame-1), "LastFrameNum changes on advance", b);
            require(ac1_bink_copy_to_buffer(b, rec, image.data(), image.size(), int32_t(pitch), h, 0, 0, 0x80000005) == 1,
                    "CopyToBuffer AC1 BGRA flags/pitch", b);
            reference.read(reinterpret_cast<char *>(ref.data()), std::streamsize(ref.size()));
            require(size_t(reference.gcount()) == ref.size(), "complete reference frame", b);
            double sse = 0;
            for (size_t i = 0; i < image.size(); ++i) {
                int delta = int(image[i]) - ref[i]; sse += double(delta * delta);
                if (i % 4 == 3) require(image[i] == 255, "opaque BINKSURFACE32A alpha", b);
            }
            double psnr = sse ? 10 * std::log10(65025.0 * double(image.size()) / sse) : INFINITY;
            require(psnr >= 45, "BGRA PSNR at least 45 dB", b);
            total_sse += sse;
            std::printf("frame=%d provider=%016llx reference=%016llx psnr=%.3f\n", frame,
                (unsigned long long)hash(image), (unsigned long long)hash(ref), psnr);
            if (frame == 1) first_hash = hash(image);
            if (frame == 1) {
                // Bounds checks must leave the WHOLE destination untouched.
                std::vector<uint8_t> guard(image.size(), 0xa5), before = guard;
                require(ac1_bink_copy_to_buffer(b, rec, guard.data(), guard.size()-1, int32_t(pitch), h, 0, 0, 5) == -1 && guard == before,
                        "reject short span atomically", b);
                require(ac1_bink_copy_to_buffer(b, rec, guard.data(), guard.size(), -1, h, 0, 0, 5) == -1 && guard == before,
                        "reject negative pitch", b);
                require(ac1_bink_copy_to_buffer(b, rec, guard.data(), guard.size(), int32_t(pitch), h, UINT32_MAX, 0, 5) == -1 && guard == before,
                        "reject overflowing rectangle", b);
                require(ac1_bink_copy_to_buffer(b, rec, guard.data(), guard.size(), int32_t(pitch), h, 0, 0, 3) == -1 && guard == before,
                        "diagnose unsupported surface", b);
                uint32_t padded_pitch = pitch + 32;
                std::vector<uint8_t> padded(size_t(padded_pitch)*(h+2), 0xa5);
                require(ac1_bink_copy_to_buffer(b, rec, padded.data(), padded.size(), int32_t(padded_pitch), h+2, 2, 1, 5) == 1,
                        "padded pitch and XY offset", b);
                for (size_t i=0;i<padded.size();++i) {
                    size_t row = i/padded_pitch, col = i%padded_pitch;
                    uint8_t expected = row >= 1 && row <= h && col >= 8 && col < pitch+8
                        ? image[(row-1)*pitch+col-8] : 0xa5;
                    require(padded[i] == expected, "padding/offset pixels preserved", b);
                }
            }
            if (frame < n) {
                require(ac1_bink_next_frame(b, rec) == 1, "NextFrame", b);
                require(sink.word(rec+0x10) == uint32_t(frame), "LastFrameNum after NextFrame", b);
                require(ac1_bink_copy_to_buffer(b, rec, ref.data(), ref.size(), int32_t(pitch), h, 0, 0, 5) == 1 && ref == image,
                        "display buffer survives NextFrame", b);
                if (frame == 2) {
                    sink.now = 1000000 + uint64_t(2)*1000000*div/fps - 1;
                    require(ac1_bink_wait(b,rec) == 1, "wait before boundary", b);
                    require(ac1_bink_pause(b,rec,1) == 1 && sink.word(rec+0xdc) == 1, "pause record", b);
                    sink.now += 9000000;
                    require(ac1_bink_wait(b,rec) == 1 && ac1_bink_should_skip(b,rec) == 0, "paused clock", b);
                    require(ac1_bink_pause(b,rec,0) == 1 && sink.word(rec+0xdc) == 0, "resume record", b);
                    require(ac1_bink_wait(b,rec) == 1, "preserved remaining wait", b);
                    ++sink.now;
                    require(ac1_bink_wait(b,rec) == 0, "wait ends at exact boundary", b);
                }
                sink.now += uint64_t(div)*1000000/fps;
            }
        }
        for (const auto &entry : sink.pcm) if (!entry.second.empty()) {
            require(entry.first == 0, "only selected audio track delivered", b);
            std::string name = std::string(argv[4])+".track"+std::to_string(entry.first)+".s16";
            std::ofstream pcm(name, std::ios::binary);
            pcm.write(reinterpret_cast<const char *>(entry.second.data()), std::streamsize(entry.second.size()*2));
            require(bool(pcm), "write private PCM evidence", b);
            std::printf("audio track=%u rate=%u channels=%u samples=%zu\n", entry.first, sink.rates[entry.first], sink.channels[entry.first], entry.second.size());
        }
        bool has_audio = !sink.pcm[0].empty();
        sink.now += uint64_t(count)*1000000*div/fps;
        require(ac1_bink_should_skip(b,rec) == int(uint32_t(n) < count), "late-frame skip predicate", b);
        // Silent LoadingScreen also exercises the real last-frame -> frame-1 loop.
        if (!has_audio) {
            for (uint32_t frame = uint32_t(n)+1; frame <= count; ++frame) {
                require(ac1_bink_next_frame(b,rec) == 1 && ac1_bink_do_frame(b,rec) == 1, "decode through end", b);
            }
            require(ac1_bink_next_frame(b,rec) == 1 && sink.word(rec+0xc) == 1 && ac1_bink_do_frame(b,rec) == 1,
                    "Bink loops at last frame", b);
            require(ac1_bink_copy_to_buffer(b,rec,image.data(),image.size(),int32_t(pitch),h,0,0,5) == 1 && hash(image) == first_hash,
                    "loop decodes original first frame", b);
        }
        require(ac1_bink_close(b,rec) == 1 && ac1_bink_do_frame(b,rec) == -1, "close releases record", b);
        if (has_audio) {
            // Verify multi-track IDs and per-track volume before the first packet.
            sink.pcm.clear(); sink.frames.clear();
            uint32_t tracks[] = {0,1,2,3};
            require(ac1_bink_set_sound_tracks(b,4,tracks) == 1 && ac1_bink_open(b,rec,argv[1],0x4000) == 1,
                    "reopen with four selected IDs", b);
            require(ac1_bink_set_volume(b,rec,0,0) == 1 && ac1_bink_do_frame(b,rec) == 1, "muted DoFrame", b);
            require(sink.pcm.size() == 4 && !sink.pcm[0].empty() &&
                    std::all_of(sink.pcm[0].begin(),sink.pcm[0].end(),[](int16_t v){return v==0;}), "independent track mute", b);
            require(ac1_bink_close(b,rec) == 1, "close multitrack", b);
            sink.reject_audio = true;
            require(ac1_bink_open(b,rec,argv[1],0x4000) == 1 && ac1_bink_do_frame(b,rec) == -1,
                    "sink failure is diagnosed", b);
            require(std::strstr(ac1_bink_error(b),"audio sink refused") != nullptr && ac1_bink_close(b,rec) == 1,
                    "failed players remain closable", b);
        }
        sink.reject_write = true;
        require(ac1_bink_open(b,rec,argv[1],0x4000) == -1, "failed guest allocation is diagnosed", b);
        std::printf("PASS %dx%u frames=%d aggregate_psnr=%.3f audio=%s controls=pause:%d/resume:%d/stop:%d\n",
                    int(w), h, n, total_sse ? 10*std::log10(65025.0*double(image.size())*n/total_sse) : INFINITY,
                    has_audio ? "track0+multitrack+mute+rejection" : "silent+loop", sink.controls[1], sink.controls[2], sink.controls[3]);
        return 0;
    } catch (const std::exception &e) { std::fprintf(stderr,"FAIL %s\n",e.what()); return 1; }
}

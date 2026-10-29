/* Adapted from recomp-kit dx/bink.cpp, copyright (c) 2026 veritr1x.
 * MIT License
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "ac1_bink_native.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {
struct Decoder {
    AVCodecContext *codec = nullptr;
    AVFrame *frame = nullptr;
    uint32_t id = 0;
    int stream = -1, volume = 32768;
    uint64_t samples = 0;
    ~Decoder() { av_frame_free(&frame); avcodec_free_context(&codec); }
};
struct Player {
    AVFormatContext *input = nullptr;
    AVIOContext *custom_io = nullptr;
    ac1_bink_input source{};
    Decoder video;
    std::vector<std::unique_ptr<Decoder>> audio;
    SwsContext *scale = nullptr;
    std::vector<uint8_t> bgra;
    std::array<uint8_t, AC1_BINK_RECORD_BYTES> record{};
    uint32_t address = 0, count = 0, current = 1, last = 0, decoded = 0, flags = 0;
    AVRational fps{};
    uint64_t origin = 0, paused_at = 0;
    bool started = false, paused = false, failed = false, eof = false;
    bool rewind = false, have_frame = false;
    ~Player() {
        sws_freeContext(scale); avformat_close_input(&input);
        if (custom_io) { av_freep(&custom_io->buffer); avio_context_free(&custom_io); }
    }
};
void put32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
}
struct ac1_bink_provider {
    ac1_bink_callbacks cb{};
    std::map<uint32_t, std::unique_ptr<Player>> players;
    std::vector<uint32_t> tracks{0};
    uint32_t guest_alloc = 0, guest_free = 0;
    char error[512]{};
};
namespace {
int fail(ac1_bink_provider *b, const char *op, int code = 0, Player *p = nullptr) {
    char detail[AV_ERROR_MAX_STRING_SIZE]{};
    if (code) av_strerror(code, detail, sizeof detail);
    std::snprintf(b->error, sizeof b->error, "%s%s%s", op, code ? ": " : "", detail);
    if (p) {
        size_t used = std::strlen(b->error);
        std::snprintf(b->error + used, sizeof b->error - used, "; record=0x%08x", p->address);
    }
    if (p) p->failed = true;
    return -1;
}
Player *find(ac1_bink_provider *b, uint32_t a) {
    b->error[0] = 0;
    auto it = b->players.find(a);
    if (it == b->players.end()) {
        std::snprintf(b->error, sizeof b->error, "unknown BINK record 0x%08x", a);
        return nullptr;
    }
    if (it->second->failed) {
        std::snprintf(b->error, sizeof b->error, "BINK record 0x%08x previously failed; close required", a);
        return nullptr;
    }
    return it->second.get();
}
int write_record(ac1_bink_provider *b, Player &p) {
    put32(p.record.data(), uint32_t(p.video.codec->width));
    put32(p.record.data() + 4, uint32_t(p.video.codec->height));
    put32(p.record.data() + 8, p.count);
    put32(p.record.data() + 0xc, p.current);
    put32(p.record.data() + 0x10, p.last);
    put32(p.record.data() + 0x14, uint32_t(p.fps.num));
    put32(p.record.data() + 0x18, uint32_t(p.fps.den));
    put32(p.record.data() + 0x20, p.flags);
    put32(p.record.data() + 0xdc, p.paused);
    return b->cb.write_guest(b->cb.opaque, p.address, p.record.data(), p.record.size())
        ? 1 : fail(b, "write BINK record outside guest memory", 0, &p);
}
bool decoder_open(ac1_bink_provider *b, Player &p, Decoder &d, int index) {
    AVStream *s = p.input->streams[index];
    const AVCodec *codec = avcodec_find_decoder(s->codecpar->codec_id);
    if (!codec) return fail(b, "FFmpeg Bink decoder unavailable", 0, &p), false;
    d.codec = avcodec_alloc_context3(codec);
    d.frame = av_frame_alloc(); d.stream = index; d.id = uint32_t(s->id);
    if (!d.codec || !d.frame) return fail(b, "allocate decoder", AVERROR(ENOMEM), &p), false;
    int rc = avcodec_parameters_to_context(d.codec, s->codecpar);
    if (rc >= 0) {
        d.codec->thread_count = 1;
        rc = avcodec_open2(d.codec, codec, nullptr);
    }
    return rc < 0 ? (fail(b, "open Bink decoder", rc, &p), false) : true;
}
bool audio_packet(ac1_bink_provider *b, Player &p, Decoder &d, const AVPacket *pkt) {
    int rc = avcodec_send_packet(d.codec, pkt);
    if (rc < 0) return fail(b, "send Bink audio packet", rc, &p), false;
    while ((rc = avcodec_receive_frame(d.codec, d.frame)) >= 0) {
        AVFrame &f = *d.frame;
        int channels = f.ch_layout.nb_channels;
        if ((f.format != AV_SAMPLE_FMT_FLT && f.format != AV_SAMPLE_FMT_FLTP) ||
            channels < 1 || channels > 2 || f.sample_rate <= 0 ||
            channels != d.codec->ch_layout.nb_channels || f.sample_rate != d.codec->sample_rate)
            return fail(b, "unsupported Bink PCM format/rate/channel change", 0, &p), false;
        std::vector<int16_t> pcm(size_t(f.nb_samples) * size_t(channels));
        for (int i = 0; i < f.nb_samples; ++i) for (int ch = 0; ch < channels; ++ch) {
            const auto *src = reinterpret_cast<const float *>(f.extended_data[f.format == AV_SAMPLE_FMT_FLTP ? ch : 0]);
            float value = src[f.format == AV_SAMPLE_FMT_FLTP ? i : i * channels + ch];
            if (!std::isfinite(value)) value = 0;
            int sample = int(std::lround(std::clamp(value, -1.0f, 1.0f) * d.volume));
            pcm[size_t(i) * channels + ch] = int16_t(std::clamp(sample, -32768, 32767));
        }
        uint64_t pts = d.samples * 1000000 / uint32_t(f.sample_rate);
        if (b->cb.submit_pcm && !b->cb.submit_pcm(b->cb.opaque, p.address, d.id, pcm.data(),
                size_t(f.nb_samples), uint32_t(f.sample_rate), uint32_t(channels), pts))
            return fail(b, "audio sink refused complete PCM chunk", 0, &p), false;
        d.samples += uint32_t(f.nb_samples);
        av_frame_unref(d.frame);
    }
    return rc == AVERROR(EAGAIN) || rc == AVERROR_EOF || (fail(b, "receive Bink audio", rc, &p), false);
}
uint64_t due(const Player &p, uint32_t frame) {
    return uint64_t(av_rescale_rnd(int64_t(frame) * 1000000, p.fps.den, p.fps.num, AV_ROUND_DOWN));
}
void control(ac1_bink_provider *b, Player &p, int event) {
    if (b->cb.audio_control) b->cb.audio_control(b->cb.opaque, p.address, event);
}
int source_read(void *opaque, uint8_t *buffer, int bytes) {
    auto &p = *static_cast<Player *>(opaque);
    int rc = p.source.read(p.source.opaque, buffer, bytes);
    return rc > 0 ? rc : (rc == 0 ? AVERROR_EOF : AVERROR(EIO));
}
int64_t source_seek(void *opaque, int64_t offset, int whence) {
    auto &p = *static_cast<Player *>(opaque);
    if ((whence & ~AVSEEK_FORCE) == AVSEEK_SIZE) return int64_t(p.source.size);
    int64_t rc = p.source.seek(p.source.opaque, offset, whence & ~AVSEEK_FORCE);
    return rc < 0 ? AVERROR(EIO) : rc;
}
}
extern "C" {
ac1_bink_provider *ac1_bink_create(const ac1_bink_callbacks *cb) {
    if (!cb || !cb->write_guest || !cb->monotonic_us) return nullptr;
    auto *b = new (std::nothrow) ac1_bink_provider;
    if (b) b->cb = *cb;
    return b;
}
void ac1_bink_destroy(ac1_bink_provider *b) {
    if (!b) return;
    for (auto &entry : b->players) control(b, *entry.second, AC1_BINK_AUDIO_STOP);
    delete b;
}
const char *ac1_bink_error(const ac1_bink_provider *b) { return b ? b->error : "null Bink provider"; }
int ac1_bink_set_sound_tracks(ac1_bink_provider *b, uint32_t n, const uint32_t *ids) {
    b->error[0] = 0;
    if (n > 8 || (n && !ids)) return fail(b, "BinkSetSoundTrack invalid count/list (limit 8)");
    std::vector<uint32_t> selected;
    for (uint32_t i = 0; i < n; ++i) {
        if (std::find(selected.begin(), selected.end(), ids[i]) != selected.end())
            return fail(b, "BinkSetSoundTrack duplicate ID");
        selected.push_back(ids[i]);
    }
    b->tracks = std::move(selected);
    return 1;
}
int ac1_bink_set_memory(ac1_bink_provider *b, uint32_t a, uint32_t f) {
    b->error[0] = 0;
    if ((!a) != (!f)) return fail(b, "BinkSetMemory requires a matched callback pair");
    b->guest_alloc = a; b->guest_free = f; return 1;
}
static int bink_open_impl(ac1_bink_provider *b, uint32_t address, const char *path,
                          const ac1_bink_input *source, uint32_t flags) {
    b->error[0] = 0;
    if (!address || address > UINT32_MAX - AC1_BINK_RECORD_BYTES ||
        (!source && (!path || !*path)) || (source && (!source->read || !source->seek || source->size > INT64_MAX)))
        return fail(b, "BinkOpen invalid record/path");
    if (flags & ~uint32_t(AC1_BINK_OPEN_TRACKS)) {
        std::snprintf(b->error, sizeof b->error, "BinkOpen record=0x%08x unsupported flags=0x%08x (only 0/0x4000 supported)", address, flags);
        return -1;
    }
    if (b->players.count(address)) return fail(b, "BinkOpen duplicate guest record");
    auto p = std::make_unique<Player>(); p->address = address; p->flags = flags;
    const std::vector<uint32_t> default_track{0};
    const auto &selected_tracks = flags & AC1_BINK_OPEN_TRACKS ? b->tracks : default_track;
    const AVInputFormat *format = av_find_input_format("bink");
    if (!format) return fail(b, "FFmpeg bink demuxer unavailable");
    // Explicit file protocol prevents URLs/network protocols, even with a full FFmpeg build.
    AVFormatContext *input = avformat_alloc_context();
    if (!input) return fail(b, "allocate input", AVERROR(ENOMEM));
    p->input = input;
    int rc = 0;
    if (source) {
        p->source = *source;
        auto *buffer = static_cast<uint8_t *>(av_malloc(65536));
        if (!buffer) return fail(b, "allocate rooted Bink IO", AVERROR(ENOMEM));
        p->custom_io = avio_alloc_context(buffer, 65536, 0, p.get(), source_read, nullptr, source_seek);
        if (!p->custom_io) { av_free(buffer); return fail(b, "allocate rooted Bink IO context", AVERROR(ENOMEM)); }
        input->pb = p->custom_io;
    } else {
        AVDictionary *options = nullptr;
        av_dict_set(&options, "protocol_whitelist", "file", 0);
        rc = avio_open2(&input->pb, path, AVIO_FLAG_READ, nullptr, &options);
        av_dict_free(&options);
    }
    if (rc < 0) return fail(b, "open read-only Bink file", rc, p.get());
    input->flags |= AVFMT_FLAG_CUSTOM_IO;
    // Open with explicit custom IO; close it on every exit via a small guard below.
    AVIOContext *io = input->pb;
    rc = avformat_open_input(&p->input, nullptr, format, nullptr);
    if (rc < 0) { if (!source) avio_closep(&io); return fail(b, "read Bink header", rc, p.get()); }
    // Transfer ordinary file IO ownership back to avformat_close_input.
    if (!source) p->input->flags &= ~AVFMT_FLAG_CUSTOM_IO;
    for (unsigned i = 0; i < p->input->nb_streams; ++i) {
        AVStream *s = p->input->streams[i];
        if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && p->video.stream < 0) {
            if (s->codecpar->codec_id != AV_CODEC_ID_BINKVIDEO)
                return fail(b, "unsupported Bink video codec", 0, p.get());
            if (!decoder_open(b, *p, p->video, int(i))) return -1;
            p->fps = s->avg_frame_rate;
            if (p->fps.num <= 0 || p->fps.den <= 0) p->fps = s->r_frame_rate;
            // Bink demuxer time_base is the exact header frame duration.
            if (p->fps.num <= 0 || p->fps.den <= 0) p->fps = av_inv_q(s->time_base);
            int64_t count = s->nb_frames > 0 ? s->nb_frames : s->duration;
            if (count <= 0 || count > UINT32_MAX) return fail(b, "invalid Bink frame count", 0, p.get());
            p->count = uint32_t(count);
        } else if (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                   std::find(selected_tracks.begin(), selected_tracks.end(), uint32_t(s->id)) != selected_tracks.end()) {
            if (s->codecpar->codec_id != AV_CODEC_ID_BINKAUDIO_DCT && s->codecpar->codec_id != AV_CODEC_ID_BINKAUDIO_RDFT)
                return fail(b, "unsupported Bink audio codec", 0, p.get());
            auto d = std::make_unique<Decoder>();
            if (!decoder_open(b, *p, *d, int(i))) return -1;
            p->audio.push_back(std::move(d));
        }
    }
    if (p->video.stream < 0 || p->fps.num <= 0 || p->fps.den <= 0 ||
        p->video.codec->width <= 0 || p->video.codec->height <= 0 ||
        av_image_check_size(unsigned(p->video.codec->width), unsigned(p->video.codec->height), 0, nullptr) < 0)
        return fail(b, "invalid Bink dimensions/video/rate", 0, p.get());
    if (write_record(b, *p) < 0) return -1;
    b->players.emplace(address, std::move(p)); return 1;
}
int ac1_bink_open(ac1_bink_provider *b, uint32_t a, const char *path, uint32_t flags) {
    return bink_open_impl(b, a, path, nullptr, flags);
}
int ac1_bink_open_input(ac1_bink_provider *b, uint32_t a, const ac1_bink_input *source, uint32_t flags) {
    if (!source) return fail(b, "null rooted Bink input");
    return bink_open_impl(b, a, nullptr, source, flags);
}
int ac1_bink_close(ac1_bink_provider *b, uint32_t a) {
    b->error[0] = 0;
    auto it = b->players.find(a);
    if (it == b->players.end()) return fail(b, "BinkClose unknown record");
    control(b, *it->second, AC1_BINK_AUDIO_STOP); b->players.erase(it); return 1;
}
int ac1_bink_do_frame(ac1_bink_provider *b, uint32_t a) {
    Player *p = find(b, a); if (!p) return -1;
    if (p->decoded == p->current && !p->rewind) return 1;
    if (p->paused) return fail(b, "BinkDoFrame while paused");
    if (p->rewind) {
        int rc = av_seek_frame(p->input, p->video.stream, 0, AVSEEK_FLAG_BACKWARD);
        if (rc < 0) return fail(b, "loop Bink seek", rc, p);
        avcodec_flush_buffers(p->video.codec);
        for (auto &d : p->audio) { avcodec_flush_buffers(d->codec); d->samples = 0; }
        control(b, *p, AC1_BINK_AUDIO_STOP);
        p->rewind = false; p->eof = false;
    }
    if (!p->started) { p->origin = b->cb.monotonic_us(b->cb.opaque); p->started = true; }
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return fail(b, "allocate Bink packet", AVERROR(ENOMEM), p);
    int result = -1;
    for (;;) {
        int rc = avcodec_receive_frame(p->video.codec, p->video.frame);
        if (rc >= 0) {
            AVFrame &f = *p->video.frame;
            if (f.format != AV_PIX_FMT_YUV420P || f.width != p->video.codec->width || f.height != p->video.codec->height) {
                fail(b, "unsupported Bink frame format/dimension change (requires YUV420P)", 0, p); break;
            }
            p->scale = sws_getCachedContext(p->scale, f.width, f.height, AV_PIX_FMT_YUV420P,
                    f.width, f.height, AV_PIX_FMT_BGRA, SWS_BICUBIC, nullptr, nullptr, nullptr);
            if (!p->scale) { fail(b, "create BGRA converter", 0, p); break; }
            p->bgra.resize(size_t(f.width) * size_t(f.height) * 4);
            uint8_t *dst[4] = {p->bgra.data(), nullptr, nullptr, nullptr};
            int pitch[4] = {f.width * 4, 0, 0, 0};
            rc = sws_scale(p->scale, f.data, f.linesize, 0, f.height, dst, pitch);
            if (rc != f.height) { fail(b, "incomplete BGRA conversion", rc < 0 ? rc : 0, p); break; }
            p->have_frame = true; p->decoded = p->current;
            result = write_record(b, *p); break;
        }
        if (rc != AVERROR(EAGAIN)) { fail(b, "Bink video ended before expected frame", rc, p); break; }
        if (p->eof) { fail(b, "Bink EOF without expected frame", 0, p); break; }
        rc = av_read_frame(p->input, pkt);
        if (rc == AVERROR_EOF) {
            p->eof = true;
            bool ok = true;
            for (auto &d : p->audio) if (!audio_packet(b, *p, *d, nullptr)) { ok = false; break; }
            if (!ok) break;
            rc = avcodec_send_packet(p->video.codec, nullptr);
            if (rc < 0) { fail(b, "flush Bink video", rc, p); break; }
            continue;
        }
        if (rc < 0) { fail(b, "demux Bink packet", rc, p); break; }
        bool ok = true;
        if (pkt->stream_index == p->video.stream) {
            rc = avcodec_send_packet(p->video.codec, pkt);
            if (rc < 0) { fail(b, "send Bink video packet", rc, p); ok = false; }
        } else for (auto &d : p->audio) if (pkt->stream_index == d->stream) {
            ok = audio_packet(b, *p, *d, pkt); break;
        }
        av_packet_unref(pkt);
        if (!ok) break;
    }
    av_packet_free(&pkt); return result;
}
int ac1_bink_next_frame(ac1_bink_provider *b, uint32_t a) {
    Player *p = find(b, a); if (!p) return -1;
    if (p->decoded != p->current) return fail(b, "BinkNextFrame requires DoFrame first");
    // The pinned DLL sets public LastFrameNum in its advance helper at
    // 0x1800a209, independently of the decoder's private idempotence counter.
    p->last = p->current;
    if (p->current == p->count) {
        p->current = 1; p->rewind = true; p->origin += due(*p, p->count);
    } else ++p->current;
    return write_record(b, *p);
}
int ac1_bink_wait(ac1_bink_provider *b, uint32_t a) {
    Player *p = find(b, a); if (!p) return -1;
    uint64_t now = b->cb.monotonic_us(b->cb.opaque);
    return p->paused || (p->started && (now < p->origin || now - p->origin < due(*p, p->current - 1)));
}
int ac1_bink_should_skip(ac1_bink_provider *b, uint32_t a) {
    Player *p = find(b, a); if (!p) return -1;
    uint64_t now = b->cb.monotonic_us(b->cb.opaque);
    return p->started && !p->paused && p->current < p->count && now >= p->origin &&
        now - p->origin >= due(*p, p->current);
}
int ac1_bink_pause(ac1_bink_provider *b, uint32_t a, int paused) {
    Player *p = find(b, a); if (!p) return -1;
    if (bool(paused) != p->paused) {
        uint64_t now = b->cb.monotonic_us(b->cb.opaque);
        if (paused) p->paused_at = now;
        else if (p->started) p->origin += now - p->paused_at;
        p->paused = bool(paused);
        control(b, *p, paused ? AC1_BINK_AUDIO_PAUSE : AC1_BINK_AUDIO_RESUME);
    }
    return write_record(b, *p);
}
int ac1_bink_set_volume(ac1_bink_provider *b, uint32_t a, uint32_t id, int volume) {
    Player *p = find(b, a); if (!p) return -1;
    if (volume < 0 || volume > 65536) return fail(b, "Bink volume outside 0..65536");
    for (auto &d : p->audio) if (d->id == id) { d->volume = volume; return 1; }
    // The DLL ignores IDs that are absent/not opened; AC1 resets IDs 0..3 on close.
    return 1;
}
int ac1_bink_copy_to_buffer(ac1_bink_provider *b, uint32_t a, void *dest, size_t size,
                           int32_t pitch, uint32_t height, uint32_t x, uint32_t y, uint32_t flags) {
    Player *p = find(b, a); if (!p) return -1;
    if (flags != 5 && flags != 0x80000005u) {
        std::snprintf(b->error, sizeof b->error, "BinkCopyToBuffer record=0x%08x unsupported flags=0x%08x (requires 0x80000005 or 5)", a, flags);
        return -1;
    }
    if (!p->have_frame) return fail(b, "BinkCopyToBuffer before DoFrame");
    uint64_t w = uint32_t(p->video.codec->width), h = uint32_t(p->video.codec->height);
    uint64_t row = w * 4, xb = uint64_t(x) * 4;
    if (!dest || pitch <= 0 || uint64_t(y) + h > height || xb + row > uint32_t(pitch))
        return fail(b, "BinkCopyToBuffer invalid pitch/rectangle");
    uint64_t start = uint64_t(y) * uint32_t(pitch) + xb;
    uint64_t end = start + (h - 1) * uint32_t(pitch) + row;
    if (end > size) return fail(b, "BinkCopyToBuffer destination span too small");
    auto *bytes = static_cast<uint8_t *>(dest);
    for (size_t i = 0; i < h; ++i) std::memcpy(bytes + start + i * uint32_t(pitch), p->bgra.data() + i * row, size_t(row));
    return 1;
}
int ac1_bink_get_frame_buffers_info(ac1_bink_provider *b, uint32_t a, uint32_t address) {
    Player *p = find(b, a); if (!p) return -1;
    if (!address || address > UINT32_MAX - AC1_BINK_FRAME_BUFFERS_BYTES) return fail(b, "invalid BINKFRAMEBUFFERS address");
    std::array<uint8_t, AC1_BINK_FRAME_BUFFERS_BYTES> info{};
    auto align8 = [](uint32_t x) { return (x + 7u) & ~7u; };
    uint32_t w = uint32_t(p->video.codec->width), h = uint32_t(p->video.codec->height);
    put32(info.data(), 2);
    put32(info.data() + 4, align8(w)); put32(info.data() + 8, align8(h));
    put32(info.data() + 0xc, align8((w + 1) / 2)); put32(info.data() + 0x10, align8((h + 1) / 2));
    // +0x14 is zero (DLL GetFrameBuffersInfo resets FrameNum). Both plane
    // descriptors contain {Allocate, Buffer32, BufferPitch}; alpha absent.
    for (size_t frame = 0; frame < 2; ++frame) for (size_t plane = 0; plane < 3; ++plane)
        put32(info.data() + 0x18 + frame * 0x30 + plane * 0xc, 1);
    return b->cb.write_guest(b->cb.opaque, address, info.data(), info.size()) ? 1
        : fail(b, "BINKFRAMEBUFFERS outside guest memory");
}
}
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define DIRECTSOUND_VERSION 0x0800
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
namespace {
struct BinkVoice {
    IDirectSoundBuffer *buffer = nullptr;
    DWORD capacity = 0, write = 0, play = 0, queued = 0, rate = 0, channels = 0;
    bool playing = false, paused = false;
    uint64_t serviced = 0;
    ~BinkVoice() { if (buffer) { buffer->Stop(); buffer->Release(); } }
    bool transfer(DWORD offset, const void *source, DWORD bytes) {
        if (!bytes) return true;
        void *a = nullptr, *b = nullptr; DWORD an = 0, bn = 0;
        HRESULT hr = buffer->Lock(offset, bytes, &a, &an, &b, &bn, 0);
        if (FAILED(hr)) return false;
        if (source) { std::memcpy(a, source, an); if (bn) std::memcpy(b, static_cast<const uint8_t *>(source)+an, bn); }
        else { std::memset(a, 0, an); if (bn) std::memset(b, 0, bn); }
        return SUCCEEDED(buffer->Unlock(a, an, b, bn));
    }
    bool service() {
        if (!playing || paused) return true;
        DWORD now = 0;
        if (FAILED(buffer->GetCurrentPosition(&now, nullptr))) return false;
        uint64_t ms = GetTickCount64();
        DWORD advanced = (now + capacity - play) % capacity;
        if (ms - serviced >= 8000) advanced = capacity;
        serviced = ms;
        if (advanced >= queued) {
            // Never replay stale PCM after starvation or a long guest stall.
            buffer->Stop(); playing = false; queued = write = play = 0;
            return SUCCEEDED(buffer->SetCurrentPosition(0)) && transfer(0, nullptr, capacity);
        }
        if (!transfer(play, nullptr, advanced)) return false;
        queued -= advanced; play = now; return true;
    }
};
struct BinkAudio {
    HMODULE module = nullptr;
    IDirectSound8 *device = nullptr;
    bool failed = false;
    std::map<std::pair<uint32_t,uint32_t>, std::unique_ptr<BinkVoice>> voices;
    ~BinkAudio() { voices.clear(); if (device) device->Release(); if (module) FreeLibrary(module); }
};
}
extern "C" void *ac1_bink_audio_create(char *error, size_t capacity) {
    auto audio = std::make_unique<BinkAudio>();
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    HRESULT hr = E_FAIL;
    if (n && n + 12 < MAX_PATH) {
        std::memcpy(path+n, L"\\dsound.dll", sizeof(L"\\dsound.dll"));
        audio->module = LoadLibraryW(path);
        using Create = HRESULT (WINAPI *)(LPCGUID, IDirectSound8 **, LPUNKNOWN);
        FARPROC raw = audio->module ? GetProcAddress(audio->module, "DirectSoundCreate8") : nullptr;
        Create fn = nullptr;
        static_assert(sizeof(fn) == sizeof(raw), "Windows function pointer size");
        std::memcpy(&fn, &raw, sizeof fn);
        if (fn) hr = fn(nullptr, &audio->device, nullptr);
        if (SUCCEEDED(hr)) hr = audio->device->SetCooperativeLevel(GetDesktopWindow(), DSSCL_NORMAL);
    }
    if (FAILED(hr)) {
        std::snprintf(error, capacity, "native DirectSound8 initialization HRESULT=0x%08x", unsigned(hr));
        return nullptr;
    }
    return audio.release();
}
extern "C" void ac1_bink_audio_destroy(void *opaque) { delete static_cast<BinkAudio *>(opaque); }
extern "C" int ac1_bink_audio_service(void *opaque) {
    if (!opaque) return 1;
    auto &audio = *static_cast<BinkAudio *>(opaque);
    for (auto &v : audio.voices) if (v.second && !v.second->service()) {
        std::fprintf(stderr, "AC1 native Bink audio cursor/clear failed: record=0x%08x track=%u\n", v.first.first, v.first.second);
        audio.failed = true;
    }
    return !audio.failed;
}
extern "C" int ac1_bink_audio_submit(void *opaque, uint32_t record, uint32_t track,
                                     const int16_t *pcm, size_t frames, uint32_t rate,
                                     uint32_t channels, uint64_t pts) {
    (void)pts;
    if (!opaque || channels < 1 || channels > 2 || rate < 100 || rate > 200000 || frames > UINT32_MAX/4) return 0;
    auto &audio = *static_cast<BinkAudio *>(opaque);
    auto &entry = audio.voices[{record, track}];
    if (!entry) {
        entry = std::make_unique<BinkVoice>();
        entry->capacity = rate * channels * 2 * 8; entry->rate = rate; entry->channels = channels;
        WAVEFORMATEX wave{};
        wave.wFormatTag = WAVE_FORMAT_PCM; wave.nChannels = WORD(channels); wave.nSamplesPerSec = rate;
        wave.wBitsPerSample = 16; wave.nBlockAlign = WORD(channels*2); wave.nAvgBytesPerSec = rate*wave.nBlockAlign;
        DSBUFFERDESC desc{}; desc.dwSize = sizeof desc; desc.dwBufferBytes = entry->capacity;
        desc.dwFlags = DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2; desc.lpwfxFormat = &wave;
        if (FAILED(audio.device->CreateSoundBuffer(&desc, &entry->buffer, nullptr)) ||
            !entry->transfer(0, nullptr, entry->capacity)) { entry.reset(); return 0; }
    }
    auto &v = *entry;
    DWORD bytes = DWORD(frames*channels*2);
    if (v.rate != rate || v.channels != channels || !v.service() || bytes > v.capacity-v.queued ||
        !v.transfer(v.write, pcm, bytes)) return 0;
    v.write = (v.write+bytes)%v.capacity; v.queued += bytes;
    if (!v.playing && !v.paused && v.queued) {
        v.serviced = GetTickCount64();
        if (FAILED(v.buffer->Play(0,0,DSBPLAY_LOOPING))) return 0;
        v.playing = true;
    }
    return 1;
}
extern "C" void ac1_bink_audio_control(void *opaque, uint32_t record, int event) {
    if (!opaque) return;
    auto &audio = *static_cast<BinkAudio *>(opaque);
    auto &voices = audio.voices;
    for (auto it = voices.begin(); it != voices.end();) {
        if (it->first.first != record) { ++it; continue; }
        if (event == AC1_BINK_AUDIO_STOP || !it->second) { it = voices.erase(it); continue; }
        auto &v = *it->second;
        if (event == AC1_BINK_AUDIO_PAUSE) {
            if (!v.service() || FAILED(v.buffer->Stop())) audio.failed = true;
            v.paused = true;
        }
        if (event == AC1_BINK_AUDIO_RESUME) {
            v.paused = false; v.serviced = GetTickCount64();
            if (v.playing && FAILED(v.buffer->Play(0,0,DSBPLAY_LOOPING))) audio.failed = true;
        }
        ++it;
    }
}
#endif

/*
 * DVBIP-1002 (Hi3798CV200 histb) software decoder backend.
 *
 * Compiled into decoder.cpp when HAVE_HISTB is defined.  Replaces the
 * classic DVB video/audio device implementation (which needs
 * /dev/dvb/adapterN/{video,audio}0, absent on the histb stack) with:
 *
 *   demux PES tap (DMX_OUT_TSDEMUX_TAP) -> user-space TS/PES demux
 *     video: avcodec (hivxe-ffmpeg): stateless v4l2request hwaccel
 *            (H.264, HEVC, MPEG-2, AVS/AVS+) -> AV_PIX_FMT_DRM_PRIME
 *            dmabufs -> histb-vdp DRM overlay plane; stateful V4L2 M2M
 *            decoders (mpeg4_v4l2m2m, vc1_v4l2m2m) and the software
 *            fallback present NV12 through the same plane (dumb copy)
 *     audio: avcodec (MP2/AAC/AC3/DTS/EAC3) -> swresample -> ALSA
 *
 * The hivxe-ffmpeg libraries are dlopen'ed at runtime: the binary keeps
 * working on systems where the "extra" add-on is not installed; decoder
 * start then fails with the same "no decoder" semantics as the missing
 * device nodes did.
 */
#ifndef __lib_dvb_decoder_histb_impl_h
#define __lib_dvb_decoder_histb_impl_h

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <system_error>
#include <chrono>
#include <atomic>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <linux/dvb/dmx.h>
#include <linux/dma-buf.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <lib/driver/uapi/drm/histb_drm.h>

#include <alsa/asoundlib.h>
#include <strings.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/hwcontext_v4l2request.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include <lib/base/eerror.h>
#include <lib/base/thread.h>
#include <lib/dvb/decoder.h>
#include <lib/driver/histb_drm.h>
#include <lib/dvb/histb_audio_control.h>
#include <lib/dvb/histb_video_entry.h>

/* ------------------------------------------------------------------ */
/* hivxe-ffmpeg loaded with dlopen so enigma2 stays startable without  */
/* the extra add-on.                                                   */
/* ------------------------------------------------------------------ */

#define HISTB_FF_LIBDIR "/usr/local/lib/hivxe-ffmpeg"

/*
 * dlsym 函数指针统一带 _ptr 后缀：ffmpeg 头文件（avcodec.h 等）已把这些
 * 符号声明为真实函数，同名指针变量会报 "redeclared as different kind of
 * entity"。声明用 NAME_ptr，随后用 #define NAME NAME_ptr 把下方调用点
 * 透明重定向；dlsym 的符号名字符串不受宏影响。
 */
#define FF_SYM(ret, name, ...) \
	static ret (*name##_ptr)(__VA_ARGS__) = nullptr

FF_SYM(unsigned, avcodec_version, void);
FF_SYM(void, av_log_set_level, int level);
FF_SYM(const AVCodec *, avcodec_find_decoder, enum AVCodecID id);
FF_SYM(const AVCodec *, avcodec_find_decoder_by_name, const char *name);
FF_SYM(AVCodecContext *, avcodec_alloc_context3, const AVCodec *codec);
FF_SYM(void, avcodec_free_context, AVCodecContext **avctx);
FF_SYM(int, avcodec_open2, AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options);
FF_SYM(int, avcodec_send_packet, AVCodecContext *avctx, const AVPacket *avpkt);
FF_SYM(int, avcodec_receive_frame, AVCodecContext *avctx, AVFrame *frame);
FF_SYM(void, avcodec_flush_buffers, AVCodecContext *avctx);
FF_SYM(AVCodecParserContext *, av_parser_init, int codec_id);
FF_SYM(int, av_parser_parse2, AVCodecParserContext *s, AVCodecContext *avctx, uint8_t **out, int *out_size, const uint8_t *data, int size, int64_t pts, int64_t dts, int64_t pos);
FF_SYM(void, av_parser_close, AVCodecParserContext *s);
FF_SYM(AVPacket *, av_packet_alloc, void);
FF_SYM(void, av_packet_free, AVPacket **pkt);
FF_SYM(void, av_packet_unref, AVPacket *pkt);
FF_SYM(int, av_packet_from_data, AVPacket *pkt, uint8_t *data, int size);
FF_SYM(AVFrame *, av_frame_alloc, void);
FF_SYM(int, av_frame_ref, AVFrame *dst, const AVFrame *src);
FF_SYM(void, av_frame_free, AVFrame **frame);
FF_SYM(void, av_frame_unref, AVFrame *frame);
FF_SYM(AVBufferRef *, av_buffer_ref, AVBufferRef *buf);
FF_SYM(void, av_buffer_unref, AVBufferRef **buf);
FF_SYM(int, av_hwdevice_ctx_create, AVBufferRef **device_ref, enum AVHWDeviceType type, const char *device, AVDictionary *opts, int flags);
FF_SYM(void *, av_malloc, size_t size);
FF_SYM(void, av_free, void *ptr);
FF_SYM(int, av_strerror, int errnum, char *errbuf, size_t errbuf_size);
FF_SYM(void, av_channel_layout_default, AVChannelLayout *ch_layout, int nb_channels);
FF_SYM(int, swr_alloc_set_opts2, struct SwrContext **ps, const AVChannelLayout *out_ch_layout, enum AVSampleFormat out_sample_fmt, int out_sample_rate, const AVChannelLayout *in_ch_layout, enum AVSampleFormat in_sample_fmt, int in_sample_rate, int log_offset, void *log_ctx);
FF_SYM(int, swr_init, struct SwrContext *s);
FF_SYM(void, swr_free, struct SwrContext **s);
FF_SYM(int, swr_convert, struct SwrContext *s, uint8_t **out, int out_count, const uint8_t **in, int in_count);
FF_SYM(struct SwsContext *, sws_getCachedContext, struct SwsContext *ctx, int srcW, int srcH, enum AVPixelFormat srcFormat, int dstW, int dstH, enum AVPixelFormat dstFormat, int flags, struct SwsFilter *srcFilter, struct SwsFilter *dstFilter, const double *param);
FF_SYM(int, sws_scale, struct SwsContext *c, const uint8_t *const srcSlice[], const int srcStride[], int srcSliceY, int srcSliceH, uint8_t *const dst[], const int dstStride[]);
FF_SYM(void, sws_freeContext, struct SwsContext *ctx);

/* FF_BIND(name) 中 dlsym 用 #name 取原始符号名；赋值与调用点经这些
 * 宏展开到 *_ptr 指针。必须在 FF_SYM 声明之后、首次调用之前。 */
#define avcodec_version avcodec_version_ptr
#define av_log_set_level av_log_set_level_ptr
#define avcodec_find_decoder avcodec_find_decoder_ptr
#define avcodec_find_decoder_by_name avcodec_find_decoder_by_name_ptr
#define avcodec_alloc_context3 avcodec_alloc_context3_ptr
#define avcodec_free_context avcodec_free_context_ptr
#define avcodec_open2 avcodec_open2_ptr
#define avcodec_send_packet avcodec_send_packet_ptr
#define avcodec_receive_frame avcodec_receive_frame_ptr
#define avcodec_flush_buffers avcodec_flush_buffers_ptr
#define av_parser_init av_parser_init_ptr
#define av_parser_parse2 av_parser_parse2_ptr
#define av_parser_close av_parser_close_ptr
#define av_packet_alloc av_packet_alloc_ptr
#define av_packet_free av_packet_free_ptr
#define av_packet_unref av_packet_unref_ptr
#define av_packet_from_data av_packet_from_data_ptr
#define av_frame_alloc av_frame_alloc_ptr
#define av_frame_ref av_frame_ref_ptr
#define av_frame_free av_frame_free_ptr
#define av_frame_unref av_frame_unref_ptr
#define av_buffer_ref av_buffer_ref_ptr
#define av_buffer_unref av_buffer_unref_ptr
#define av_hwdevice_ctx_create av_hwdevice_ctx_create_ptr
#define av_malloc av_malloc_ptr
#define av_free av_free_ptr
#define av_strerror av_strerror_ptr
#define av_channel_layout_default av_channel_layout_default_ptr
#define swr_alloc_set_opts2 swr_alloc_set_opts2_ptr
#define swr_init swr_init_ptr
#define swr_free swr_free_ptr
#define swr_convert swr_convert_ptr
#define sws_getCachedContext sws_getCachedContext_ptr
#define sws_scale sws_scale_ptr
#define sws_freeContext sws_freeContext_ptr

static const char *histb_av_err(int err)
{
	static __thread char buf[AV_ERROR_MAX_STRING_SIZE];
	av_strerror(err, buf, sizeof(buf));
	return buf;
}

static bool histb_ff_load()
{
	static bool loaded = false;
	static bool ok = false;
	if (loaded)
		return ok;
	loaded = true;

	void *h[4] = {nullptr, nullptr, nullptr, nullptr};
	const char *names[3] = {"libavutil.so.61", "libavcodec.so.63", "libswresample.so.7"};
	bool failed = false;
	for (int i = 0; i < 3; ++i)
	{
		h[i] = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
		if (!h[i])
		{
			char path[160];
			snprintf(path, sizeof(path), HISTB_FF_LIBDIR "/%s", names[i]);
			h[i] = dlopen(path, RTLD_NOW | RTLD_LOCAL);
		}
		if (!h[i])
		{
			eWarning("[histb-dec] dlopen %s failed: %s", names[i], dlerror());
			failed = true;
		}
	}
	/* swscale: soname differs between builds, try known majors then unversioned */
	static const char *sws_names[] = {
		"libswscale.so.11", "libswscale.so.10", "libswscale.so.9",
		"libswscale.so.8", "libswscale.so"};
	for (const char *n : sws_names)
	{
		h[3] = dlopen(n, RTLD_NOW | RTLD_LOCAL);
		if (!h[3])
		{
			char path[160];
			snprintf(path, sizeof(path), HISTB_FF_LIBDIR "/%s", n);
			h[3] = dlopen(path, RTLD_NOW | RTLD_LOCAL);
		}
		if (h[3])
		{
			eDebug("[histb-dec] swscale loaded as %s", n);
			break;
		}
	}
	if (!h[3])
	{
		eWarning("[histb-dec] dlopen libswscale failed: %s", dlerror());
		failed = true;
	}
	if (failed)
		return false;

#define FF_BIND(name) \
	do { \
		void *sym = nullptr; \
		for (int i = 0; i < 4 && !sym; ++i) \
			if (h[i]) sym = dlsym(h[i], #name); \
		if (!sym) { eWarning("[histb-dec] missing symbol %s", #name); return false; } \
		name = (decltype(name))sym; \
	} while (0)

	FF_BIND(avcodec_version);
	FF_BIND(av_log_set_level);
	FF_BIND(avcodec_find_decoder);
	FF_BIND(avcodec_find_decoder_by_name);
	FF_BIND(avcodec_alloc_context3);
	FF_BIND(avcodec_free_context);
	FF_BIND(avcodec_open2);
	FF_BIND(avcodec_send_packet);
	FF_BIND(avcodec_receive_frame);
	FF_BIND(avcodec_flush_buffers);
	FF_BIND(av_parser_init);
	FF_BIND(av_parser_parse2);
	FF_BIND(av_parser_close);
	FF_BIND(av_packet_alloc);
	FF_BIND(av_packet_free);
	FF_BIND(av_packet_unref);
	FF_BIND(av_packet_from_data);
	FF_BIND(av_frame_alloc);
	FF_BIND(av_frame_ref);
	FF_BIND(av_frame_free);
	FF_BIND(av_frame_unref);
	FF_BIND(av_buffer_ref);
	FF_BIND(av_buffer_unref);
	FF_BIND(av_hwdevice_ctx_create);
	FF_BIND(av_malloc);
	FF_BIND(av_free);
	FF_BIND(av_strerror);
	FF_BIND(av_channel_layout_default);
	FF_BIND(swr_alloc_set_opts2);
	FF_BIND(swr_init);
	FF_BIND(swr_free);
	FF_BIND(swr_convert);
	FF_BIND(sws_getCachedContext);
	FF_BIND(sws_scale);
	FF_BIND(sws_freeContext);
#undef FF_BIND

	/* Every request-backed native consumer below honors descriptor.wait. */
	setenv("HISTB_V4L2REQUEST_DEFER_CAPTURE_WAIT", "auto", 0);
	av_log_set_level(AV_LOG_FATAL);
	eDebug("[histb-dec] hivxe-ffmpeg loaded, avcodec %u", avcodec_version());
	ok = true;
	return ok;
}

static int64_t histb_monotonicUs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ------------------------------------------------------------------ */
/* TS tap -> PES demuxer (based on eDVBVideoSwDemux, GPL-2.0-only)     */
/* ------------------------------------------------------------------ */

class HistbPesDemux
{
public:
	/* called with the ES payload of one PES packet; pts is -1 when absent */
	using PesHandler = std::function<void(const uint8_t *es, size_t length, int64_t pts, bool random_access)>;
	void setHandler(PesHandler handler) { m_handler = std::move(handler); }

	int start(eDVBDemux *demux, int pid, int pes_type, int buffer_size = 2 * 1024 * 1024)
	{
		stop();
		if (!demux || pid <= 0 || pid >= 0x1FFF)
			return -1;
		/* openDemux()/adapter/demux are private; use the public getters
		 * and open the node directly, like eDVBTSRecorder::start does. */
		uint8_t ad = 0, dn = 0;
		demux->getCAAdapterID(ad);
		demux->getCADemuxID(dn);
		char devname[32];
		snprintf(devname, sizeof(devname), "/dev/dvb/adapter%d/demux%d", ad, dn);
		m_fd_demux = ::open(devname, O_RDONLY | O_CLOEXEC);
		if (m_fd_demux < 0)
		{
			eWarning("[histb-dec] unable to open %s", devname);
			return -1;
		}
		::fcntl(m_fd_demux, F_SETFL, O_NONBLOCK);
		if (::ioctl(m_fd_demux, DMX_SET_BUFFER_SIZE, buffer_size) < 0)
			eDebug("[histb-dec] DMX_SET_BUFFER_SIZE failed: %m");
		dmx_pes_filter_params flt = {};
		flt.pid = pid;
		flt.input = DMX_IN_FRONTEND;
		flt.output = DMX_OUT_TSDEMUX_TAP;
		flt.pes_type = (dmx_pes_type_t)pes_type;
		flt.flags = DMX_IMMEDIATE_START;
		if (::ioctl(m_fd_demux, DMX_SET_PES_FILTER, &flt) < 0)
		{
			eWarning("[histb-dec] DMX_SET_PES_FILTER pid %04x failed: %m", pid);
			::close(m_fd_demux);
			m_fd_demux = -1;
			return -1;
		}
		m_pid = pid;
		m_cc = -1;
		m_pes_broken = true;
		m_random_access = false;
		m_cc_breaks = 0;
		m_overflows = 0;
		m_partial.clear();
		m_pes.clear();
		m_delivery_head = m_delivery_count = m_delivery_bytes = 0;
		for (auto &packet : m_delivery) packet.bytes.clear();
		m_stop = false;
		try
		{
			m_delivery_thread = std::thread(&HistbPesDemux::deliverThread, this);
			m_thread = std::thread(&HistbPesDemux::thread, this);
		}
		catch (const std::system_error &error)
		{
			{ std::lock_guard<std::mutex> lock(m_delivery_mutex); m_stop = true; }
			m_delivery_cv.notify_all();
			if (m_delivery_thread.joinable()) m_delivery_thread.join();
			::ioctl(m_fd_demux, DMX_STOP);
			::close(m_fd_demux); m_fd_demux = -1; m_pid = -1;
			eWarning("[histb-dec] cannot start PES ingestion: %s", error.what());
			return -1;
		}
		return 0;
	}

	void stop()
	{
		{ std::lock_guard<std::mutex> lock(m_delivery_mutex); m_stop = true; }
		m_delivery_cv.notify_all();
		if (m_thread.joinable())
			m_thread.join();
		if (m_delivery_thread.joinable())
			m_delivery_thread.join();
		for (auto &packet : m_delivery) packet.bytes.clear();
		m_delivery_head = m_delivery_count = m_delivery_bytes = 0;
		if (m_fd_demux >= 0)
		{
			::ioctl(m_fd_demux, DMX_STOP);
			::close(m_fd_demux);
			m_fd_demux = -1;
		}
		m_pid = -1;
	}

	bool active() const { return m_fd_demux >= 0; }

private:
	enum { TSPacketSize = 188 };
	struct Delivery
	{
		std::vector<uint8_t> bytes;
		size_t offset = 0, length = 0;
		int64_t pts = -1;
		bool random_access = false;
	};
	static constexpr unsigned DeliverySlots = 16;
	static constexpr size_t DeliveryBytes = 8 * 1024 * 1024;

	void deliverThread()
	{
		std::unique_lock<std::mutex> lock(m_delivery_mutex);
		while (!m_stop)
		{
			m_delivery_cv.wait(lock, [&] { return m_stop || m_delivery_count; });
			if (m_stop) break;
			Delivery &packet = m_delivery[m_delivery_head];
			lock.unlock();
			if (m_handler)
				m_handler(packet.bytes.data() + packet.offset, packet.length,
					packet.pts, packet.random_access);
			lock.lock();
			m_delivery_bytes -= packet.bytes.size();
			packet.bytes.clear();
			m_delivery_head = (m_delivery_head + 1) % DeliverySlots;
			--m_delivery_count;
			m_delivery_cv.notify_all();
		}
	}

	void thread()
	{
		std::vector<uint8_t> buffer(TSPacketSize * 512);
		struct pollfd pfd = {m_fd_demux, POLLIN, 0};
		std::chrono::steady_clock::time_point last_input{};
		while (!m_stop)
		{
			if (::poll(&pfd, 1, 100) <= 0)
				continue;
			ssize_t length = ::read(m_fd_demux, buffer.data(), buffer.size());
			if (length < 0)
			{
				if (errno == EOVERFLOW)
				{
					/* demux ring overflow: data lost, decoder misses
					 * references until the next entry point */
					if (++m_overflows == 1 || m_overflows % 10 == 0)
						eWarning("[histb-dec] tap pid %04x demux overflows: %u",
							m_pid, m_overflows);
					m_partial.clear();
					m_pes.clear();
					continue;
				}
				if (errno != EAGAIN && errno != EINTR)
				{
					eWarning("[histb-dec] tap read error pid %04x: %m", m_pid);
					break;
				}
				continue;
			}
			if (length > 0)
			{
				/* input cadence diagnostics: large periodic gaps
				 * between reads mean the vendor demux delivers
				 * this tap in bursts, which the decoder then
				 * surfaces as periodic output gaps */
				auto now = std::chrono::steady_clock::now();
				if (last_input.time_since_epoch().count() &&
					now - last_input > std::chrono::milliseconds(200))
					eDebug("[histb-dec] tap pid %04x input gap %lld ms",
						m_pid,
						(long long)std::chrono::duration_cast<std::chrono::milliseconds>(
							now - last_input).count());
				last_input = now;
			}
			feed(buffer.data(), length);
		}
	}

	void feed(const uint8_t *data, size_t length)
	{
		m_partial.insert(m_partial.end(), data, data + length);
		size_t pos = 0;
		while (m_partial.size() - pos >= TSPacketSize && !m_stop)
		{
			const uint8_t *packet = &m_partial[pos];
			if (packet[0] != 0x47)
			{
				pos++;
				continue;
			}
			pos += TSPacketSize;
			if (packet[1] & 0x80) /* transport error */
				continue;
			int pid = ((packet[1] & 0x1f) << 8) | packet[2];
			int afc = (packet[3] >> 4) & 3;
			if (pid != m_pid)
				continue;
			int cc = packet[3] & 0x0f;
			if (!m_first_pid_packet)
			{
				m_first_pid_packet = true;
				eDebug("[histb-dec] tap pid %04x first matched TS packet: afc %d cc %d pusi %d",
					m_pid, afc, cc, !!(packet[1] & 0x40));
			}
			if (!(afc & 1))
				continue;
			bool discontinuity = (afc & 2) && packet[4] && (packet[5] & 0x80);
			if (m_cc >= 0 && !discontinuity)
			{
				if (cc == m_cc)
					continue;
				if (cc != ((m_cc + 1) & 0x0f))
				{
					/* continuity break: PES gap, the decoder stalls
					 * until the next entry point (visible stutter) */
					if (++m_cc_breaks == 1 || m_cc_breaks % 250 == 0)
						eDebug("[histb-dec] tap pid %04x CC breaks: %u",
							m_pid, m_cc_breaks);
					m_pes.clear();
					m_pes_broken = true;
				}
			}
			m_cc = cc;
			int offset = 4;
			if (afc & 2)
				offset += 1 + packet[4];
			if (offset >= TSPacketSize)
				continue;
			if (packet[1] & 0x40) /* payload unit start ends the previous PES */
			{
				if (!m_pes_broken)
					deliver();
				m_pes.clear();
				m_pes_broken = false;
				m_random_access = (afc & 2) && packet[4] && (packet[5] & 0x40);
			}
			else if (m_pes_broken)
				continue;
			m_pes.insert(m_pes.end(), packet + offset, packet + TSPacketSize);
		}
		m_partial.erase(m_partial.begin(), m_partial.begin() + pos);
	}

	void deliver()
	{
		if (m_pes.size() < 9 || m_pes[0] || m_pes[1] || m_pes[2] != 1)
			return;
		/* Nonzero PES_packet_length excludes TS stuffing and bounds audio
		 * payload. A zero length remains legal for video PES. */
		size_t end = m_pes.size();
		unsigned declared = (unsigned(m_pes[4]) << 8) | m_pes[5];
		if (declared)
		{
			end = 6 + declared;
			if (end > m_pes.size()) return; /* incomplete PES */
		}
		size_t header = 9 + m_pes[8];
		if (header >= end)
			return;
		int64_t pts = -1;
		if ((m_pes[7] & 0x80) && m_pes.size() >= 14)
			pts = ((int64_t)(m_pes[9] & 0x0e) << 29) | (m_pes[10] << 22) | ((m_pes[11] & 0xfe) << 14) | (m_pes[12] << 7) | (m_pes[13] >> 1);
		if (!m_first_delivered)
		{
			m_first_delivered = true;
			eDebug("[histb-dec] tap pid %04x first PES delivered: %zu bytes, stream_id %02x, pts %lld",
				m_pid, end - header, m_pes[3], (long long)pts);
		}
		/* Decode/ALSA/KMS may block; the TS reader must still drain its tap.
		 * Move PES storage into a bounded ring and recycle it without another
		 * payload copy. Count the consumer's in-flight packet in both bounds. */
		std::unique_lock<std::mutex> lock(m_delivery_mutex);
		m_delivery_cv.wait(lock, [&] {
			return m_stop || (m_delivery_count < DeliverySlots &&
				(!m_delivery_count || (m_delivery_bytes <= DeliveryBytes &&
				 m_pes.size() <= DeliveryBytes - m_delivery_bytes)));
		});
		if (m_stop) return;
		Delivery &packet = m_delivery[(m_delivery_head + m_delivery_count) % DeliverySlots];
		packet.bytes.swap(m_pes);
		packet.offset = header; packet.length = end - header;
		packet.pts = pts; packet.random_access = m_random_access;
		m_delivery_bytes += packet.bytes.size();
		++m_delivery_count;
		m_delivery_cv.notify_all();
	}

	int m_fd_demux = -1;
	Delivery m_delivery[DeliverySlots];
	size_t m_delivery_head = 0, m_delivery_count = 0, m_delivery_bytes = 0;
	std::mutex m_delivery_mutex;
	std::condition_variable m_delivery_cv;
	std::thread m_delivery_thread;
	int m_pid = -1;
	int m_cc = -1;
	bool m_pes_broken = true;
	bool m_first_pid_packet = false;
	bool m_first_delivered = false;
	bool m_random_access = false;
	unsigned m_cc_breaks = 0;
	unsigned m_overflows = 0;
	std::atomic<bool> m_stop{false};
	std::vector<uint8_t> m_partial;
	std::vector<uint8_t> m_pes;
	std::thread m_thread;
	PesHandler m_handler;
};

/* ------------------------------------------------------------------ */
/* DRM video output.  Native KMS histb-vdp (6.12.y upstream, 5 planes  */
/* g1/g3/v0/v1 + g0 primary): zero-copy PRIME import onto the "v0"     */
/* NV12 overlay (hardware ZME + VDM Y2R, composed below the G0 OSD     */
/* via zpos).  Fallbacks: dumb-buffer copy onto the same plane, then   */
/* legacy CRTC flip for the old simple-pipe driver (RGB-only primary,  */
/* no KMS planes).                                                     */
/* ------------------------------------------------------------------ */

class HistbDrmOutput
{
public:
	~HistbDrmOutput() { close(); }

	bool open()
	{
		if (m_fd >= 0)
			return true;
		auto &session = histbDrmSession();
		std::lock_guard<std::mutex> lock(session.mutex);
		int shared = histbDrmOpen();
		if (shared >= 0)
		{
			int fd = fcntl(shared, F_DUPFD_CLOEXEC, 0);
			if (fd < 0) return false;
			if (!probeCard(fd)) { ::close(fd); return false; }
			m_fd = fd;
			m_scanout_frame = av_frame_alloc();
			for (auto &slot : m_native_slots) slot.frame = av_frame_alloc();
			if (!m_scanout_frame || !m_native_slots[0].frame || !m_native_slots[1].frame)
			{
				av_frame_free(&m_scanout_frame);
				for (auto &slot : m_native_slots) av_frame_free(&slot.frame);
				::close(m_fd); m_fd = -1; return false;
			}
			return true;
		}
		eWarning("[histb-dec] no usable DRM card/crtc found");
		return false;
	}

	void close()
	{
		if (m_fd < 0)
			return;
		if (!hide()) return; /* A failed disable must not recycle scanout memory. */
		stopPresentThread();
		for (auto &b : m_v0_buf)
		{
			if (b.map)
			{
				munmap(b.map, b.size);
				b.map = nullptr;
			}
			if (b.fb)
			{
				drmModeRmFB(m_fd, b.fb);
				b.fb = 0;
			}
			if (b.handle)
			{
				destroyDumb(b.handle);
				b.handle = 0;
			}
		}
		m_v0_buf.clear();
		m_v0_free.clear();
		if (m_v0_sws)
		{
			sws_freeContext(m_v0_sws);
			m_v0_sws = nullptr;
		}
		m_v0_plane_id = 0;
		m_v0_inited = false;
		if (m_dumb_map)
		{
			munmap(m_dumb_map, m_dumb_size);
			m_dumb_map = nullptr;
		}
		if (m_dumb_fb)
		{
			drmModeRmFB(m_fd, m_dumb_fb);
			m_dumb_fb = 0;
		}
		if (m_dumb_handle)
		{
			destroyDumb(m_dumb_handle);
			m_dumb_handle = 0;
		}
		if (m_sws)
		{
			sws_freeContext(m_sws);
			m_sws = nullptr;
		}
		av_frame_free(&m_scanout_frame);
		for (auto &slot : m_native_slots) av_frame_free(&slot.frame);
		::close(m_fd);
		m_fd = -1;
	}

	static bool linearFrameLayout(const AVDRMFrameDescriptor *desc)
	{
		for (int l = 0; l < desc->nb_layers; ++l)
			for (int p = 0; p < desc->layers[l].nb_planes; ++p)
			{
				int object = desc->layers[l].planes[p].object_index;
				if (object < 0 || object >= desc->nb_objects ||
					desc->objects[object].format_modifier != DRM_FORMAT_MOD_LINEAR)
					return false;
			}
		return true;
	}

	static bool nativeTiledGeometry(const AVFrame *frame, uint32_t &width, uint32_t &height)
	{
		const auto *desc = reinterpret_cast<const AVDRMFrameDescriptor *>(frame->data[0]);
		if (!desc || desc->nb_layers != 1 || desc->nb_objects != 1 ||
			desc->layers[0].format != DRM_FORMAT_YUV420_10BIT ||
			desc->layers[0].nb_planes != 1 ||
			desc->layers[0].planes[0].object_index != 0 || desc->layers[0].planes[0].offset ||
			desc->objects[0].format_modifier != DRM_FORMAT_MOD_HISTB_VDH_10BIT || !frame->hw_frames_ctx)
			return false;
		const auto *frames = reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data);
		if (!frames->device_ctx || frames->device_ctx->type != AV_HWDEVICE_TYPE_V4L2REQUEST)
			return false;
		/* Coded (padded) capture geometry: the hw frames context is sized from
		 * the negotiated V4L2 G_FMT, i.e. the coded dimensions.  The frame
		 * descriptor itself only carries drm/wait/deinterlace. */
		width = frames->width; height = frames->height;
		if (!width || !height || width > 4096 || height > 2160 ||
			(width & 3) || (height & 1) || frame->width <= 0 || frame->height <= 0 ||
			uint32_t(frame->width) > width || uint32_t(frame->height) > height)
			return false;
		uint64_t stride = (width + 255) & ~255u, rows = (height + 63) & ~63u;
		return desc->layers[0].planes[0].pitch == int64_t(stride) &&
			desc->objects[0].size >= stride * rows * 15 / 8;
	}

	/* Preserve native format, pitches and offsets. P010/NV15 stays ten-bit. */
	bool showFrame(const AVFrame *frame, AVRational codecRate)
	{
		const auto *desc = reinterpret_cast<const AVDRMFrameDescriptor *>(frame->data[0]);
		if (m_fd < 0 || !desc || desc->nb_layers < 1 || desc->nb_objects < 1)
		{
			m_native_error = true; m_native_cv.notify_all();
			eWarning("[histb-dec] invalid native frame descriptor");
			return false;
		}
		uint32_t format = desc->layers[0].format;
		uint32_t codedWidth = 0, codedHeight = 0;
		bool tiled = format == DRM_FORMAT_YUV420_10BIT && nativeTiledGeometry(frame, codedWidth, codedHeight);
		if (!tiled && format != DRM_FORMAT_NV12 && format != DRM_FORMAT_NV21 &&
			format != DRM_FORMAT_P010 && format != DRM_FORMAT_NV15)
		{
			m_native_error = true; m_native_cv.notify_all();
			eWarning("[histb-dec] unsupported DRM frame format %08x", desc->layers[0].format);
			return false;
		}

		return enqueueNative(frame, codecRate);
	}

	/* Existing NV12 compatibility route, reached only after request completion.
	 * Main10 never enters a CPU pixel copy or reduced-depth fallback. */
	bool showFrameNv12Fallback(const AVDRMFrameDescriptor *desc, int width, int height, int64_t pts)
	{

		/* 2. dumb-buffer copy path on the same plane */
		if ((m_v0_plane_id || !m_v0_inited) && !m_sw)
		{
			if (showFrameV0(desc, width, height, pts))
				return true;
			if (!m_v0_plane_id)
			{
				eDebug("[histb-dec] no usable V0 plane, using legacy CRTC paths");
				m_v0_inited = true;
			}
		}

		/* 3. PRIME import onto the legacy CRTC; pointless when a real
		 * V0 plane exists (the NV12 fb would take over the primary
		 * and kick the OSD off the screen) */
		if (!m_sw && !m_v0_plane_id)
		{
			uint32_t handles[4] = {0, 0, 0, 0};
			int nh = mapHandles(desc, handles);
			if (nh < 0)
			{
				/* PRIME import rejected (e.g. community histb-vdp has no
				 * prime import): skip the direct path, present via
				 * mmap + sws below */
				eDebug("[histb-dec] PRIME import rejected (%s), switching to XRGB8888 sw path",
					strerror(errno));
				if (!initSwPath())
					return false;
				m_sw = true;
			}
			else
			{
				uint32_t pitches[4] = {0, 0, 0, 0}, offsets[4] = {0, 0, 0, 0};
				pitches[0] = desc->layers[0].planes[0].pitch;
				offsets[0] = desc->layers[0].planes[0].offset;
				pitches[1] = desc->layers[0].nb_planes > 1 ? desc->layers[0].planes[1].pitch : 0;
				offsets[1] = desc->layers[0].nb_planes > 1 ? desc->layers[0].planes[1].offset : 0;

				uint32_t fb = 0;
				int ret = drmModeAddFB2(m_fd, width, height, DRM_FORMAT_NV12,
					handles, pitches, offsets, &fb, 0);
				if (ret == 0 && fb)
				{
					if (!presentFb(fb))
					{
						drmModeRmFB(m_fd, fb);
						unmapHandles(handles);
						return false;
					}
					if (!m_nv12_presented)
					{
						m_nv12_presented = true;
						eDebug("[histb-dec] first NV12 frame presented %dx%d", width, height);
					}
					/* previous fb may still be scanned out until the flip
					 * takes effect at vblank; free it one frame later */
					if (m_prev_fb)
					{
						drmModeRmFB(m_fd, m_prev_fb);
						m_prev_fb = 0;
					}
					unmapHandles(m_prev_handles);
					m_prev_fb = m_fb;
					memcpy(m_prev_handles, m_gem_handles, sizeof(m_gem_handles));
					m_fb = fb;
					memcpy(m_gem_handles, handles, sizeof(handles));
					return true;
				}
				unmapHandles(handles);
				eDebug("[histb-dec] NV12 fb rejected (%s), switching to XRGB8888 sw path",
					strerror(errno));
				if (!initSwPath())
					return false;
				m_sw = true;
			}
		}

		/* software path */
		if (desc->layers[0].nb_planes < 2)
		{
			eDebug("[histb-dec] NV12 desc with %u planes", desc->layers[0].nb_planes);
			return false;
		}
		const uint8_t *src[2] = {nullptr, nullptr};
		int strides[2] = {0, 0};
		void *maps[2] = {nullptr, nullptr};
		size_t mapsz[2] = {0, 0};
		if (mapDecoderPlanes(desc, height, src, strides, maps, mapsz) < 0)
			return false;
		m_sws = sws_getCachedContext(m_sws, width, height, AV_PIX_FMT_NV12,
			m_width, m_height, AV_PIX_FMT_BGR0, SWS_BILINEAR,
			nullptr, nullptr, nullptr);
		if (!m_sws)
		{
			eWarning("[histb-dec] sws_getCachedContext failed");
			unmapDecoderPlanes(desc, maps, mapsz);
			return false;
		}
		uint8_t *dst[4] = {m_dumb_map, nullptr, nullptr, nullptr};
		int dststrides[4] = {(int)m_dumb_pitch, 0, 0, 0};
		int rc = sws_scale(m_sws, src, strides, 0, height, dst, dststrides);
		unmapDecoderPlanes(desc, maps, mapsz);
		if (rc < 0)
		{
			static int swarned = 0;
			if (!swarned++)
				eWarning("[histb-dec] sws_scale failed: %d", rc);
			return false;
		}
		if (!m_sw_presented)
		{
			m_sw_presented = true;
			eDebug("[histb-dec] first sw frame presented %dx%d -> %dx%d",
				width, height, m_width, m_height);
		}
		return presentFb(m_dumb_fb);
	}

	/* show one software NV12/YUV420P frame: stateful V4L2 M2M decoders
	 * output mmap'ed NV12, the v4l2request software fallback is
	 * YUV420P.  Copy into the NV12 dumb buffer on the V0 plane, or
	 * convert to XRGB8888 for the legacy CRTC when no overlay plane
	 * exists. */
	bool showFrameSw(const AVFrame *f, int64_t pts)
	{
		if (m_fd < 0 || !f || f->width <= 0 || f->height <= 0)
			return false;
		switch (f->format)
		{
		case AV_PIX_FMT_NV12:
		case AV_PIX_FMT_YUV420P:
		case AV_PIX_FMT_YUVJ420P:
			break;
		default:
			eDebug("[histb-dec] unsupported software frame format %d", f->format);
			return false;
		}
		if ((m_v0_plane_id || !m_v0_inited) && showFrameSwV0(f, pts))
			return true;
		if (!m_v0_plane_id)
			return showFrameSwXrgb(f);
		return false;
	}

	/* Cancel producers before joining the TS tap: a full bounded queue can
	 * otherwise keep m_pes.stop() waiting for a producer indefinitely. */
	void cancelNative()
	{
		std::lock_guard<std::mutex> lock(m_native_mutex);
		m_native_cancel = true;
		m_native_cv.notify_all();
		m_present_cv.notify_all();
	}

	bool resumeNative()
	{
		std::lock_guard<std::mutex> lock(m_native_mutex);
		if (m_fd < 0 || !m_scanout_frame || m_native_running || m_scanout_frame->buf[0]) return false;
		m_native_error = false;
		m_native_decode_failed = false;
		m_native_cancel = false;
		m_native_presented = 0;
		m_native_direct_capture_ready = false;
		m_native_completed = m_native_refresh_skipped = 0;
		m_native_output_period_us = m_native_output_source_period_us = m_native_output_phase_us = 0.0;
		m_frame_period_us = 40000.0;
		m_native_rate = AVRational{0, 1};
		m_native_pts_period_us = 0.0;
		m_native_pts_candidates = 0;
		startCaptureServer();
		return true;
	}

	bool hide()
	{
		if (m_fd < 0) return true;
		cancelNative();
		stopCaptureServer();
		if (m_native_thread.joinable()) m_native_thread.join();
		m_native_running = false;
		/* A queued request can still be executing when stop cancels pacing.
		 * Finish it before releasing its capture reference; hold no KMS/queue
		 * lock while the FFmpeg completion callback waits. */
		for (auto &slot : m_native_slots)
			if (slot.busy) waitNativeFrame(slot.frame);
		presentFlush();
		stopPresentThread();
		std::lock_guard<std::mutex> lock(histbDrmSession().mutex);
		if (m_v0_plane_id && drmModeSetPlane(m_fd, m_v0_plane_id, m_crtc_id, 0,
			0, 0, 0, 0, 0, 0, 0, 0, 0) < 0)
		{
			eWarning("[histb-dec] V0 disable failed; retaining scanout ownership: %m");
			return false;
		}
		retireFrame();
		{
			std::lock_guard<std::mutex> queueLock(m_native_mutex);
			for (auto &slot : m_native_slots)
			{
				av_frame_unref(slot.frame); slot.busy = false;
			}
			m_native_count = m_native_head = 0;
		}
		m_fb_bound = false;
		m_prime_failed = false;
		m_prime_presented = false;
		m_pacer_slot = std::chrono::steady_clock::time_point{};
		m_last_presented_pts = AV_NOPTS_VALUE;
		return true;
	}

	bool valid() const { return m_fd >= 0; }

	bool nativeError()
	{
		std::lock_guard<std::mutex> lock(m_native_mutex);
		return m_native_error;
	}

	bool nativeDecodeFailed() const { return m_native_decode_failed.load(); }

private:
#include <lib/dvb/histb_video_capture_impl.h>

	/* Exactly two capture references total across queued and worker-inflight
	 * states, plus one retained scanout. No AVFrame wrapper allocations on the
	 * per-frame path; full slots backpressure the producer without dropping. */
	bool enqueueNative(const AVFrame *frame, AVRational codecRate)
	{
		std::unique_lock<std::mutex> lock(m_native_mutex);
		m_native_cv.wait(lock, [&] {
			return m_native_cancel || m_native_error || !m_native_slots[0].busy || !m_native_slots[1].busy;
		});
		if (m_native_cancel || m_native_error) return false;
		int idx = m_native_slots[0].busy ? 1 : 0;
		if (av_frame_ref(m_native_slots[idx].frame, frame) < 0)
		{
			m_native_error = true; m_native_cv.notify_all();
			eWarning("[histb-dec] native frame reference failed");
			return false;
		}
		m_native_slots[idx].rate = codecRate;
		m_native_slots[idx].busy = true;
		m_native_queue[(m_native_head + m_native_count) % 2] = idx;
		++m_native_count;
		if (!m_native_running)
		{
			try
			{
				m_native_thread = std::thread(&HistbDrmOutput::nativeThread, this);
				m_native_running = true;
			}
			catch (const std::system_error &error)
			{
				/* Keep the pending request retained for hide() to finish;
				 * completion must not run while holding the queue mutex. */
				m_native_error = true;
				m_native_cv.notify_all();
				eWarning("[histb-dec] cannot start native presentation worker: %s", error.what());
				return false;
			}
			eDebug("[histb-dec] native presentation worker started: 2 capture slots + scanout");
		}
		m_native_cv.notify_all();
		return true;
	}

	static bool waitNativeFrame(const AVFrame *frame)
	{
		/* Other DRM_PRIME producers only promise AVDRMFrameDescriptor.
		 * Reading the request extension without this guard is out-of-bounds. */
		if (!frame->hw_frames_ctx) return true;
		const auto *frames = reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data);
		if (!frames->device_ctx || frames->device_ctx->type != AV_HWDEVICE_TYPE_V4L2REQUEST) return true;
		const auto *request = reinterpret_cast<const AVV4L2RequestFrameDescriptor *>(frame->data[0]);
		int ret = request->wait ? request->wait(request->wait_opaque) : 0;
		if (ret < 0) eWarning("[histb-dec] native request completion failed: %s", histb_av_err(ret));
		return ret >= 0;
	}

	bool paceNative(const AVFrame *frame, AVRational codecRate)
	{
		std::unique_lock<std::mutex> lock(m_native_mutex);
		if (m_native_cancel) return false;
		int64_t pts = frame->pts;
		AVRational rate = codecRate;
		const char *authority = "codec";
		if (frame->hw_frames_ctx)
		{
			const auto *frames = reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data);
			if (frames->device_ctx && frames->device_ctx->type == AV_HWDEVICE_TYPE_V4L2REQUEST && frames->hwctx)
			{
				const auto *request = static_cast<const AVV4L2RequestFramesContext *>(frames->hwctx);
				if (request->frame_rate.num > 0 && request->frame_rate.den > 0)
				{
					rate = request->frame_rate; authority = "request codec syntax";
				}
			}
		}
		if (rate.num > 0 && rate.den > 0)
		{
			if (m_native_rate.num != rate.num || m_native_rate.den != rate.den)
			{
				m_native_rate = rate;
				m_frame_period_us = 1000000.0 * rate.den / rate.num;
				eDebug("[histb-dec] native cadence %d/%d fps from %s (%.3f ms)",
					rate.num, rate.den, authority, m_frame_period_us / 1000.0);
			}
		}
		else if (!m_native_rate.num && m_native_pts_candidates < 3 && pts != AV_NOPTS_VALUE && m_last_presented_pts != AV_NOPTS_VALUE)
		{
			/* No codec authority: require repeated consistent short deltas.
			 * A missing-frame hole never increases an established cadence. */
			int64_t delta = pts - m_last_presented_pts;
			if (delta < 0) delta += (1LL << 33);
			double us = double(delta) * 1000000.0 / 90000.0;
			if (delta >= 900 && delta <= 9000 && (!m_native_pts_candidates || us <= m_frame_period_us * 1.25))
			{
				double difference = us - m_native_pts_period_us;
				if (difference < 0) difference = -difference;
				if (m_native_pts_period_us && difference <= m_native_pts_period_us * 0.1)
					++m_native_pts_candidates;
				else { m_native_pts_period_us = us; m_native_pts_candidates = 1; }
				if (m_native_pts_candidates >= 3) m_frame_period_us = m_native_pts_period_us;
			}
			else m_native_pts_candidates = 0;
		}
		m_last_presented_pts = pts;
		/* At a slower sink, selected flips already pace the stream. Waiting
		 * again at every source timestamp misses vblanks at noninteger ratios
		 * such as 50->30 and throttles the decoder despite presentation skips. */
		if (m_native_output_period_us > m_frame_period_us + 0.5)
		{
			m_pacer_slot = std::chrono::steady_clock::time_point{};
			return true;
		}
		auto now = std::chrono::steady_clock::now();
		if (m_pacer_slot.time_since_epoch().count() == 0) m_pacer_slot = now;
		if (now < m_pacer_slot)
			m_native_cv.wait_until(lock, m_pacer_slot, [&] { return m_native_cancel.load(); });
		if (m_native_cancel) return false;
		now = std::chrono::steady_clock::now();
		/* A slower HDMI flip can legitimately consume more than one source
		 * period. Keep those completed frames on the source clock; only a
		 * real stall resets it, otherwise every skipped refresh adds delay. */
		double late = std::max(m_frame_period_us * 2, m_native_output_period_us);
		if (now > m_pacer_slot + std::chrono::microseconds(int64_t(late))) m_pacer_slot = now;
		m_pacer_slot += std::chrono::microseconds(int64_t(m_frame_period_us));
		return true;
	}

	bool nativePresentationDue()
	{
		double output;
		{
			std::lock_guard<std::mutex> display(histbDrmSession().mutex);
			output = histbDrmSession().refreshPeriodUs;
		}
		if (output <= 0) output = histbDrmModePeriodUs(m_mode);
		if (output != m_native_output_period_us || m_frame_period_us != m_native_output_source_period_us)
		{
			m_native_output_period_us = output;
			m_native_output_source_period_us = m_frame_period_us;
			/* Present the first completed frame after either clock changes. */
			m_native_output_phase_us = output - m_frame_period_us;
			if (output > 0)
				eDebug("[histb-dec] native display cadence: source %.3f fps, HDMI %.3f Hz (presentation selection only)",
					1000000.0 / m_frame_period_us, 1000000.0 / output);
		}
		if ((!m_native_rate.num && m_native_pts_candidates < 3) || output <= m_frame_period_us + 0.5)
			return true;
		/* Complete every request and retain decoder DPB references. Only the
		 * presentation is resampled; the current scanout stays pinned. */
		m_native_output_phase_us += m_frame_period_us;
		if (m_native_output_phase_us + 0.001 < output) return false;
		m_native_output_phase_us -= output;
		return true;
	}

	void nativeThread()
	{
		std::unique_lock<std::mutex> lock(m_native_mutex);
		for (;;)
		{
			m_native_cv.wait(lock, [&] { return m_native_cancel || m_native_error || m_native_count; });
			if (m_native_cancel || m_native_error) break;
			int idx = m_native_queue[m_native_head];
			m_native_head = (m_native_head + 1) % 2; --m_native_count;
			AVFrame *frame = m_native_slots[idx].frame;
			lock.unlock();
			/* Complete before any PRIME import or CPU map. The decoder is free
			 * to submit the next request while this consumer waits/paces/flips. */
			bool ok = waitNativeFrame(frame);
			if (!ok) m_native_decode_failed = true;
			bool direct = false;
			bool refreshSkipped = false;
			if (ok)
			{
				const auto *desc = reinterpret_cast<const AVDRMFrameDescriptor *>(frame->data[0]);
				if (!m_prime_failed && paceNative(frame, m_native_slots[idx].rate))
				{
					refreshSkipped = !nativePresentationDue();
					if (!refreshSkipped) direct = tryDirectV0(frame);
				}
				ok = direct || refreshSkipped;
				bool cancelled;
				{ std::lock_guard<std::mutex> state(m_native_mutex); cancelled = m_native_cancel; }
				if (!ok && !cancelled && !m_native_error &&
					desc->layers[0].format == DRM_FORMAT_NV12 && linearFrameLayout(desc))
				{
					/* One-way compatibility transition; don't share pacing state
					 * concurrently between native and dumb presentation workers. */
					m_prime_failed = true;
					ok = showFrameNv12Fallback(desc, frame->width, frame->height, frame->pts);
				}
			}
			lock.lock();
			if (direct)
			{
				std::swap(m_scanout_frame, m_native_slots[idx].frame);
				m_native_direct_capture_ready = true;
			}
			else if (ok && !refreshSkipped) m_native_direct_capture_ready = false;
			/* A submitted flip without retirement proof still owns both the
			 * old scanout and this slot. Only successful hide may release it. */
			if (!m_native_inflight)
			{
				av_frame_unref(m_native_slots[idx].frame);
				m_native_slots[idx].busy = false;
			}
			if (!ok && !m_native_cancel)
			{
				m_native_error = true;
				eWarning("[histb-dec] native presentation failed; producer stopped (no frame/depth fallback)");
			}
			if (ok)
			{
				if (refreshSkipped) ++m_native_refresh_skipped;
				else ++m_native_presented;
				if (++m_native_completed % 500 == 0)
					eDebug("[histb-dec] native stats: %u completed, %u presented/queued, %u refresh-selected skips, 0 queue drops, 2 slots",
						m_native_completed, m_native_presented, m_native_refresh_skipped);
			}
			m_native_cv.notify_all();
		}
		m_native_cv.notify_all();
	}

	/* ---- V0 hardware video plane path ---- */

	/* Prefer CV200 V0 (NV12/NV21/NV15/P010), which has the ZME scaler.
	 * The two-format V1 plane is unity-only. */
	void findV0Plane()
	{
		drmModePlaneResPtr pr = drmModeGetPlaneResources(m_fd);
		if (!pr)
			return;
		int crtc_index = -1;
		drmModeResPtr res = drmModeGetResources(m_fd);
		if (res)
		{
			for (int k = 0; k < res->count_crtcs; ++k)
				if (res->crtcs[k] == m_crtc_id)
					crtc_index = k;
			drmModeFreeResources(res);
		}
		if (crtc_index < 0)
		{
			drmModeFreePlaneResources(pr);
			return;
		}
		for (int pass = 0; pass < 2 && !m_v0_plane_id; ++pass)
		{
			for (unsigned int i = 0; i < pr->count_planes && !m_v0_plane_id; ++i)
			{
				drmModePlanePtr p = drmModeGetPlane(m_fd, pr->planes[i]);
				if (!p)
					continue;
				if (p->possible_crtcs & (1u << crtc_index))
				{
					/* histb-vdp driver source (6.12.y):
					 * internal id3 "v0" advertises native ten-bit layouts and
					 * is the video plane with ZME scaling (up to 8x);
					 * id4 "v1" has 2 formats and enforces an
					 * exact 1:1 src/dst rect (DRM_PLANE_NO_
					 * SCALING -> -ERANGE when stretched).
					 * Prefer v0 so 720x576 can be scaled to
					 * the full display. */
					bool nativeDepth = false;
					for (unsigned int j = 0; j < p->count_formats; ++j)
						if (p->formats[j] == DRM_FORMAT_NV15 || p->formats[j] == DRM_FORMAT_YUV420_10BIT)
							nativeDepth = true;
					for (unsigned int j = 0; j < p->count_formats; ++j)
						if (p->formats[j] == DRM_FORMAT_NV12 &&
						    (pass == 1 || nativeDepth))
						{
							m_v0_plane_id = p->plane_id;
							break;
						}
				}
				drmModeFreePlane(p);
			}
		}
		drmModeFreePlaneResources(pr);
		if (m_v0_plane_id)
		{
			eDebug("[histb-dec] V0 overlay plane %u found", m_v0_plane_id);
			forceV0ZposBelowOsd();
			makeOsdTransparent();
		}
		else
			eDebug("[histb-dec] no NV12 overlay plane on crtc %u", m_crtc_id);
	}

	/* The video plane must sit below the OSD plane: gfx zpos = id+2
	 * (G0=2), video zpos = id-3 (V0=0).  A video plane left at a
	 * higher zpos (e.g. leftover kernel state from experiments)
	 * covers the whole OSD and menus become invisible.  Force V0's
	 * zpos property back to 0. */
	void forceV0ZposBelowOsd()
	{
		drmModeObjectPropertiesPtr props =
			drmModeObjectGetProperties(m_fd, m_v0_plane_id, DRM_MODE_OBJECT_PLANE);
		if (!props)
			return;
		for (unsigned int i = 0; i < props->count_props; ++i)
		{
			/* drmModeObjectProperties: props[] is a plain array of
			 * property IDs, the values live in the parallel
			 * prop_values[] array (no per-element struct) */
			drmModePropertyPtr p = drmModeGetProperty(m_fd, props->props[i]);
			if (!p)
				continue;
			bool is_zpos = strcmp(p->name, "zpos") == 0;
			uint64_t value = props->prop_values[i];
			drmModeFreeProperty(p);
			if (is_zpos)
			{
				if (value != 0 &&
				    drmModeObjectSetProperty(m_fd, m_v0_plane_id,
				    DRM_MODE_OBJECT_PLANE, props->props[i], 0) == 0)
					eDebug("[histb-dec] V0 plane zpos %llu -> 0",
						(unsigned long long)value);
				break;
			}
		}
		drmModeFreeObjectProperties(props);
	}

	/* The process-wide display owner supplies an ARGB primary with per-pixel
	 * alpha. Channel changes must not wipe UI memory or restore stale FBs. */
	bool makeOsdTransparent()
	{
		std::lock_guard<std::mutex> lock(histbDrmSession().mutex);
		return histbEnsureOsd();
	}



	/* Path 1: zero-copy PRIME import onto the V0 plane.  The native KMS
	 * histb-vdp imports the decoder dmabuf (contiguous buffers scan out
	 * directly; scattered ones are copied by the driver into a shadow),
	 * scales through ZME and converts through VDM Y2R in hardware, and
	 * composes the video below the G0 OSD plane. */
	bool tryDirectV0(const AVFrame *frame)
	{
		const auto *desc = reinterpret_cast<const AVDRMFrameDescriptor *>(frame->data[0]);
		int width = frame->width, height = frame->height;
		uint32_t fbWidth = width, fbHeight = height;
		bool tiled = desc->layers[0].format == DRM_FORMAT_YUV420_10BIT;
		if (tiled && !nativeTiledGeometry(frame, fbWidth, fbHeight)) return false;
		if (!m_v0_plane_id)
		{
			findV0Plane();
			if (!m_v0_plane_id)
			{
				m_v0_inited = true;
				return false;
			}
		}
		if (desc->layers[0].nb_planes < (tiled ? 1 : 2) || desc->layers[0].nb_planes > 4)
			return false;

		uint32_t handles[4] = {0, 0, 0, 0};
		int nh = mapHandles(desc, handles);
		if (nh < 0)
		{
			/* driver has no PRIME import: permanent, stop retrying */
			m_prime_failed = true;
			return false;
		}
		uint32_t pitches[4] = {}, offsets[4] = {};
		uint64_t modifiers[4] = {};
		bool explicitModifier = false;
		for (int p = 0; p < desc->layers[0].nb_planes; ++p)
		{
			const auto &plane = desc->layers[0].planes[p];
			if (plane.object_index < 0 || plane.object_index >= desc->nb_objects)
			{ unmapHandles(handles); return false; }
			pitches[p] = plane.pitch; offsets[p] = plane.offset;
			modifiers[p] = desc->objects[plane.object_index].format_modifier;
			explicitModifier |= modifiers[p] != DRM_FORMAT_MOD_LINEAR;
		}

		uint32_t fb = 0;
		int ret = explicitModifier ?
			drmModeAddFB2WithModifiers(m_fd, fbWidth, fbHeight, desc->layers[0].format,
				handles, pitches, offsets, modifiers, &fb, DRM_MODE_FB_MODIFIERS) :
			drmModeAddFB2(m_fd, fbWidth, fbHeight, desc->layers[0].format,
				handles, pitches, offsets, &fb, 0);
		if (ret < 0 || !fb)
		{
			static int fbwarned = 0;
			if (!fbwarned++)
				eDebug("[histb-dec] PRIME AddFB2 format %08x rejected (%s)",
					desc->layers[0].format, strerror(errno));
			unmapHandles(handles);
			m_prime_failed = true;
			return false;
		}
		/* V0 plane check requires even source geometry (2<<16 - 1
		 * alignment mask); round down odd stream sizes */
		uint32_t sw = (uint32_t)width & ~1u;
		uint32_t sh = (uint32_t)height & ~1u;
		/* Native completion/pacing happened before this display lock. Keep it
		 * through the flip event so AVControl cannot race pending scanout. */
		std::lock_guard<std::mutex> lock(histbDrmSession().mutex);
		syncOutputMode();
		bool committed = commitNativeV0(fb, sw, sh);
		unmapHandles(handles);
		if (!committed)
		{
			if (!m_native_inflight) drmModeRmFB(m_fd, fb);
			return false;
		}
		if (m_fb) drmModeRmFB(m_fd, m_fb);
		m_fb = fb;
		/* The matching event establishes replacement of the old scanout. */
		if (!m_prime_presented)
		{
			m_prime_presented = true;
			eDebug("[histb-dec] first PRIME V0 frame %dx%d format %08x modifier %016llx coded %ux%u -> %dx%d (native depth)",
				width, height, desc->layers[0].format, (unsigned long long)modifiers[0],
				fbWidth, fbHeight, m_width, m_height);
		}
		return true;
	}

	/* Event user_data is a process-unique integer, never a pointer to a
	 * stack object or freed decoder. A late event from a timed-out session
	 * cannot falsely complete a subsequent channel's flip. drmHandleEvent
	 * invokes callbacks synchronously on the current native worker. */
	static HistbDrmOutput *&nativeEventOwner()
	{
		static thread_local HistbDrmOutput *owner = nullptr;
		return owner;
	}

	static void nativeFlipHandler(int, unsigned, unsigned, unsigned, void *data)
	{
		auto *owner = nativeEventOwner();
		if (owner && reinterpret_cast<uintptr_t>(data) == owner->m_native_event_cookie)
			owner->m_native_flip_done = true;
	}

	/* Successful blocking disable completes earlier commits. Consume their
	 * queued events before a compatibility route uses a different callback. */
	void drainRetiredNativeEvents()
	{
		pollfd pfd = {m_fd, POLLIN, 0};
		while (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
		{
			drmEventContext context = {};
			context.version = 2; context.page_flip_handler = nativeFlipHandler;
			nativeEventOwner() = this;
			int handled = drmHandleEvent(m_fd, &context);
			nativeEventOwner() = nullptr;
			if (handled < 0) break;
		}
	}

	/* Called with HistbDrmSession::mutex held; one outstanding native commit.
	 * Cancellation stops future submissions, not retirement of an accepted
	 * one. A missing event is an error, never permission to recycle memory. */
	bool commitNativeV0(uint32_t fb, uint32_t width, uint32_t height)
	{
		if (!m_v0_props.tried) initV0Props();
		if (!m_v0_props.valid)
		{
			m_native_error = true; m_native_cv.notify_all();
			eWarning("[histb-dec] native atomic plane properties unavailable");
			return false;
		}
		drmModeAtomicReqPtr req = drmModeAtomicAlloc();
		if (!req)
		{
			m_native_error = true; m_native_cv.notify_all();
			eWarning("[histb-dec] native atomic request allocation failed");
			return false;
		}
		const auto &p = m_v0_props;
		bool valid = true;
		auto add = [&](uint32_t property, uint64_t value) {
			if (!property || drmModeAtomicAddProperty(req, m_v0_plane_id, property, value) < 0) valid = false;
		};
		add(p.fb_id, fb); add(p.crtc_id, m_crtc_id);
		add(p.src_x, 0); add(p.src_y, 0);
		add(p.src_w, uint64_t(width) << 16); add(p.src_h, uint64_t(height) << 16);
		add(p.crtc_x, 0); add(p.crtc_y, 0); add(p.crtc_w, m_width); add(p.crtc_h, m_height);
		static std::atomic<uintptr_t> nextCookie{1};
		m_native_event_cookie = nextCookie.fetch_add(1);
		m_native_flip_done = false;
		int ret = valid ? drmModeAtomicCommit(m_fd, req,
			DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
			reinterpret_cast<void *>(m_native_event_cookie)) : -1;
		int commitError = errno;
		drmModeAtomicFree(req);
		if (ret < 0)
		{
			m_native_error = true; m_native_cv.notify_all();
			eWarning("[histb-dec] native atomic submission failed: %s", strerror(commitError));
			return false;
		}
		m_native_inflight = true;
		m_native_pending_fb = fb;
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
		while (!m_native_flip_done)
		{
			auto now = std::chrono::steady_clock::now();
			if (now >= deadline) break;
			int remaining = std::max(1, int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()));
			pollfd pfd = {m_fd, POLLIN, 0};
			int ready = ::poll(&pfd, 1, remaining);
			if (ready < 0 && errno == EINTR) continue;
			if (ready <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
			if (pfd.revents & POLLIN)
			{
				drmEventContext context = {};
				context.version = 2; context.page_flip_handler = nativeFlipHandler;
				nativeEventOwner() = this;
				int handled = drmHandleEvent(m_fd, &context);
				nativeEventOwner() = nullptr;
				if (handled < 0) break;
			}
		}
		if (m_native_flip_done)
		{
			m_native_inflight = false; m_native_pending_fb = 0;
			if (!m_native_atomic_logged)
			{
				m_native_atomic_logged = true;
				eDebug("[histb-dec] native atomic flip completed by page-flip event");
			}
			return true;
		}
		m_native_error = true; m_native_cv.notify_all();
		eWarning("[histb-dec] native atomic flip completion missing; disabling plane before retirement");
		if (drmModeSetPlane(m_fd, m_v0_plane_id, m_crtc_id, 0,
			0, 0, 0, 0, 0, 0, 0, 0, 0) == 0)
		{
			drainRetiredNativeEvents();
			m_native_inflight = false; m_native_pending_fb = 0;
			eWarning("[histb-dec] native plane disabled after missing flip event; output remains failed");
		}
		else eWarning("[histb-dec] native plane disable failed: %m; retaining pending frame and scanout");
		return false;
	}

	/* Bracket fallback CPU reads with DMA-BUF cache synchronization.
	 * Map shared Y/UV objects once and respect the object byte size. */
	void unmapDecoderPlanes(const AVDRMFrameDescriptor *desc, void *maps[2], size_t sizes[2])
	{
		for (int p = 0; p < 2; ++p)
			if (maps[p])
			{
				int fd = desc->objects[desc->layers[0].planes[p].object_index].fd;
				dma_buf_sync sync = {}; sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
				if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
					eWarning("[histb-dec] DMA-BUF read end failed: %m");
				munmap(maps[p], sizes[p]); maps[p] = nullptr;
			}
	}

	int mapDecoderPlanes(const AVDRMFrameDescriptor *desc, int height,
		const uint8_t *src[2], int strides[2], void *maps[2], size_t sizes[2])
	{
		for (int p = 0; p < 2; ++p)
		{
			const auto &plane = desc->layers[0].planes[p];
			if (plane.object_index < 0 || plane.object_index >= desc->nb_objects) break;
			const auto &obj = desc->objects[plane.object_index];
			size_t rows = p ? (height + 1) / 2 : height;
			if (plane.pitch <= 0 || plane.offset < 0 || uint64_t(plane.offset) + uint64_t(plane.pitch) * rows > obj.size) break;
			if (p && plane.object_index == desc->layers[0].planes[0].object_index)
			{
				src[p] = static_cast<const uint8_t *>(maps[0]) + plane.offset;
				strides[p] = plane.pitch;
				return 0;
			}
			void *map = mmap(nullptr, obj.size, PROT_READ, MAP_SHARED, obj.fd, 0);
			if (map == MAP_FAILED) break;
			dma_buf_sync sync = {}; sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
			if (ioctl(obj.fd, DMA_BUF_IOCTL_SYNC, &sync) < 0) { munmap(map, obj.size); break; }
			maps[p] = map; sizes[p] = obj.size;
			src[p] = static_cast<const uint8_t *>(map) + plane.offset; strides[p] = plane.pitch;
			if (p == 1) return 0;
		}
		unmapDecoderPlanes(desc, maps, sizes);
		eWarning("[histb-dec] cannot map/synchronize decoder DMA-BUF: %m");
		return -1;
	}

	/* property ids of the V0 plane, needed for atomic commits */
	bool initV0Props()
	{
		m_v0_props.tried = true;
		drmModeObjectPropertiesPtr props =
			drmModeObjectGetProperties(m_fd, m_v0_plane_id, DRM_MODE_OBJECT_PLANE);
		if (!props)
		{
			eDebug("[histb-dec] V0 props: ObjectGetProperties failed: %s", strerror(errno));
			return false;
		}
		unsigned count = props->count_props;
		for (unsigned int i = 0; i < props->count_props; ++i)
		{
			drmModePropertyPtr p = drmModeGetProperty(m_fd, props->props[i]);
			if (!p)
				continue;
			const char *n = p->name;
			uint32_t id = p->prop_id;
			if (!strcmp(n, "FB_ID"))
				m_v0_props.fb_id = id;
			else if (!strcmp(n, "CRTC_ID"))
				m_v0_props.crtc_id = id;
			else if (!strcmp(n, "SRC_X"))
				m_v0_props.src_x = id;
			else if (!strcmp(n, "SRC_Y"))
				m_v0_props.src_y = id;
			else if (!strcmp(n, "SRC_W"))
				m_v0_props.src_w = id;
			else if (!strcmp(n, "SRC_H"))
				m_v0_props.src_h = id;
			else if (!strcmp(n, "CRTC_X"))
				m_v0_props.crtc_x = id;
			else if (!strcmp(n, "CRTC_Y"))
				m_v0_props.crtc_y = id;
			else if (!strcmp(n, "CRTC_W"))
				m_v0_props.crtc_w = id;
			else if (!strcmp(n, "CRTC_H"))
				m_v0_props.crtc_h = id;
			drmModeFreeProperty(p);
		}
		drmModeFreeObjectProperties(props);
		m_v0_props.valid = m_v0_props.fb_id && m_v0_props.crtc_id &&
			m_v0_props.src_w && m_v0_props.src_h &&
			m_v0_props.crtc_w && m_v0_props.crtc_h;
		eDebug("[histb-dec] V0 props: count=%u valid=%d", count, m_v0_props.valid ? 1 : 0);
		return m_v0_props.valid;
	}

	/* NV12 dumb buffers sized for `width x height` (one holds Y
	 * stride*height + UV stride*height/2: allocate bpp=8 with 1.5x
	 * height so the size matches exactly) */
	bool initV0Path(int width, int height)
	{
		if (!m_v0_buf.empty() && m_v0_buf[0].fb && m_v0_w == width && m_v0_h == height)
			return true;
		if (m_v0_inited && (m_v0_buf.empty() || !m_v0_buf[0].fb))
			return false;
		if (!m_v0_plane_id)
		{
			findV0Plane();
			if (!m_v0_plane_id)
				return false;
		}
		/* findV0Plane may already have run in the PRIME path, so the
		 * property enumeration must not be tied to the plane lookup */
		if (!m_v0_props.tried)
			initV0Props();
		/* re-create on video size change; the present thread must not
		 * touch the buffers being freed */
		if (!m_v0_buf.empty() && m_v0_buf[0].fb)
		{
			stopPresentThread();
			drmModeSetPlane(m_fd, m_v0_plane_id, m_crtc_id, 0,
				0, 0, 0, 0, 0, 0, 0, 0, 0);
			for (auto &b : m_v0_buf)
			{
				drmModeRmFB(m_fd, b.fb);
				b.fb = 0;
				destroyDumb(b.handle);
				b.handle = 0;
				munmap(b.map, b.size);
				b.map = nullptr;
			}
			m_v0_buf.clear();
			m_v0_free.clear();
		}
		/* pool size: 48 MB cap, at least 6 buffers to cover the
		 * scanout/paced/being-written pipeline, at most 32 (a ~1 s
		 * burst at 25 fps) */
		{
			size_t per = (size_t)((width + 63) / 64 * 64) *
				     (size_t)(((height + 1) / 2) * 3) + 4096;
			int nbuf = (int)(48ULL * 1024 * 1024 / per);
			nbuf = nbuf < 6 ? 6 : (nbuf > 32 ? 32 : nbuf);
			m_v0_buf.assign(nbuf, V0Buf{});
			m_v0_free.assign(nbuf, 1);
		}
		for (auto &b : m_v0_buf)
		{
			struct drm_mode_create_dumb creq = {};
			creq.width = width;
			creq.height = ((height + 1) / 2) * 3;
			creq.bpp = 8;
			if (::ioctl(m_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
			{
				eWarning("[histb-dec] create NV12 dumb buffer failed: %s", strerror(errno));
				return false;
			}
			b.yoff = creq.pitch * (uint32_t)height;
			uint32_t handles[4] = {creq.handle, creq.handle, 0, 0};
			uint32_t pitches[4] = {creq.pitch, creq.pitch, 0, 0};
			uint32_t offsets[4] = {0, b.yoff, 0, 0};
			if (drmModeAddFB2(m_fd, width, height, DRM_FORMAT_NV12,
				handles, pitches, offsets, &b.fb, 0) < 0 || !b.fb)
			{
				eWarning("[histb-dec] AddFB2 NV12 dumb failed: %s", strerror(errno));
				destroyDumb(creq.handle);
				return false;
			}
			struct drm_mode_map_dumb mreq = {};
			mreq.handle = creq.handle;
			if (::ioctl(m_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
			{
				eWarning("[histb-dec] map NV12 dumb failed: %s", strerror(errno));
				drmModeRmFB(m_fd, b.fb);
				b.fb = 0;
				destroyDumb(creq.handle);
				return false;
			}
			b.map = (uint8_t *)mmap(nullptr, creq.size,
				PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, mreq.offset);
			if (b.map == MAP_FAILED)
			{
				b.map = nullptr;
				drmModeRmFB(m_fd, b.fb);
				b.fb = 0;
				destroyDumb(creq.handle);
				eWarning("[histb-dec] mmap NV12 dumb failed: %s", strerror(errno));
				return false;
			}
			b.handle = creq.handle;
			b.pitch = creq.pitch;
			b.size = creq.size;
		}
		m_v0_w = width;
		m_v0_h = height;
		eDebug("[histb-dec] V0 path: NV12 dumb x%zu %dx%d (buf %dx%d) pitch %u on plane %u",
			m_v0_buf.size(), width, height, width, ((height + 1) / 2) * 3,
			m_v0_buf[0].pitch, m_v0_plane_id);
		return true;
	}

	/* NV12 -> NV12 row copy (handles differing pitch); far cheaper than
	 * routing through sws_scale for a like-for-like transfer */
	static void copyNV12(const uint8_t *src[2], const int strides[2],
			     uint8_t *dst, uint32_t dpitch, uint32_t yoff,
			     int width, int height)
	{
		uint32_t w = (uint32_t)width;
		uint32_t h = (uint32_t)height;
		for (uint32_t r = 0; r < h; ++r)
			memcpy(dst + r * dpitch, src[0] + r * strides[0], w);
		uint8_t *duv = dst + yoff;
		for (uint32_t r = 0; r < h / 2; ++r)
			memcpy(duv + r * dpitch, src[1] + r * strides[1], w);
	}

	/* ---- flip-synchronised presentation ----
	 *
	 * The decode thread only copies a frame into a free buffer and
	 * queues it; a dedicated present thread serialises the atomic
	 * commits and waits for each page-flip event before presenting
	 * the next frame.  This keeps the decode thread free of any
	 * DRM stalls and never drops frames for kernel backpressure:
	 * the previous nonblock-only design got -EBUSY whenever the
	 * previous commit's vblank event had not fired yet (measured
	 * 18-30% dropped frames = visible stutter). */

	static void flipHandler(int, unsigned int, unsigned int, unsigned int, void *data)
	{
		static_cast<std::atomic<bool> *>(data)->store(true, std::memory_order_release);
	}

	/* Native compatibility copies backpressure rather than discard a frame.
	 * Existing software callers retain their nonblocking acquisition policy. */
	int acquireV0Buf(bool block = false)
	{
		std::unique_lock<std::mutex> lock(m_present_mutex);
		if (block) m_present_cv.wait(lock, [&] {
			return m_native_cancel || m_native_error || m_present_stop ||
				std::find(m_v0_free.begin(), m_v0_free.end(), 1) != m_v0_free.end();
		});
		if (block && (m_native_cancel || m_native_error || m_present_stop)) return -1;
		for (size_t i = 0; i < m_v0_free.size(); ++i)
			if (m_v0_free[i])
			{
				m_v0_free[i] = 0;
				return (int)i;
			}
		return -1;
	}

	void releaseV0Buf(int idx)
	{
		std::lock_guard<std::mutex> l(m_present_mutex);
		m_v0_free[idx] = 1;
		m_present_cv.notify_all();
	}

	void enqueueV0(int idx)
	{
		std::lock_guard<std::mutex> l(m_present_mutex);
		m_present_queue.push_back(idx);
		m_present_cv.notify_one();
	}

	void startPresentThread()
	{
		if (m_present_running)
			return;
		m_present_stop = false;
		m_present_running = true;
		m_present_thread = std::thread(&HistbDrmOutput::presentThread, this);
	}

	/* stop the thread and return every buffer to the free pool;
	 * callers must not have in-flight commits afterwards */
	void stopPresentThread()
	{
		if (!m_present_running)
			return;
		m_present_stop = true;
		m_present_cv.notify_all();
		if (m_present_thread.joinable())
			m_present_thread.join();
		m_present_running = false;
		m_present_stop = false;
		{
			std::lock_guard<std::mutex> l(m_present_mutex);
			m_present_queue.clear();
			std::fill(m_v0_free.begin(), m_v0_free.end(), 1);
			m_v0_onscreen = -1;
		}
	}

	/* drop queued (never committed) frames; the on-screen buffer stays
	 * accounted and is released by the next successful flip */
	void presentFlush()
	{
		if (!m_present_running)
			return;
		{
			std::lock_guard<std::mutex> l(m_present_mutex);
			for (int idx : m_present_queue)
				m_v0_free[idx] = 1;
			m_present_queue.clear();
		}
		m_present_cv.notify_all();
		/* wait until a commit already taken by the thread is done
		 * touching the plane */
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
		while (m_present_busy && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}

	/* present-thread side: wait for the page-flip event of the commit
	 * just submitted (user_data == &m_flip_done) */
	void waitFlipEvent()
	{
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
		while (!m_flip_done.load(std::memory_order_acquire))
		{
			if (m_present_stop || std::chrono::steady_clock::now() >= deadline)
			{
				static int tw = 0;
				if (!tw++)
					eWarning("[histb-dec] V0 page-flip event wait timed out");
				break;
			}
			struct pollfd pfd = {m_fd, POLLIN, 0};
			if (::poll(&pfd, 1, 50) > 0)
			{
				drmEventContext ctx;
				memset(&ctx, 0, sizeof(ctx));
				ctx.version = 2;
				ctx.page_flip_handler = flipHandler;
				drmHandleEvent(m_fd, &ctx);
			}
		}
	}

	/* present-thread side: commit one queued V0 buffer and wait for
	 * the flip to take effect.  Returns true when the new frame is on
	 * screen (then the previously displayed buffer left scanout). */
	/* Follow AVControl's shared atomic output mode, including the direct path. */
	void syncOutputMode()
	{
		drmModeCrtcPtr crtc = drmModeGetCrtc(m_fd, m_crtc_id);
		if (!crtc || !crtc->mode_valid)
		{
			if (crtc)
				drmModeFreeCrtc(crtc);
			return;
		}
		uint32_t w = crtc->width, h = crtc->height;
		const auto &mode = crtc->mode;
		if (!histbDrmSession().refreshPeriodUs || mode.clock != m_mode.clock ||
			mode.htotal != m_mode.htotal || mode.vtotal != m_mode.vtotal ||
			mode.flags != m_mode.flags || mode.vscan != m_mode.vscan)
		{
			m_mode = mode;
			histbDrmSession().refreshPeriodUs = histbDrmModePeriodUs(mode);
		}
		drmModeFreeCrtc(crtc);
		if (w && h && (w != (uint32_t)m_width || h != (uint32_t)m_height))
		{
			eDebug("[histb-dec] output mode changed %dx%d -> %ux%u", m_width, m_height, w, h);
			m_width = (int)w;
			m_height = (int)h;
		}
	}

	bool commitV0(int idx)
	{
		std::lock_guard<std::mutex> displayLock(histbDrmSession().mutex);
		V0Buf &b = m_v0_buf[idx];
		bool ok = false;
		syncOutputMode();
		if (m_v0_props.valid && !m_v0_atomic_failed)
		{
			drmModeAtomicReqPtr req = drmModeAtomicAlloc();
			if (!req)
			{
				m_v0_atomic_failed = true;
			}
			else
			{
				uint32_t pid = m_v0_plane_id;
				drmModeAtomicAddProperty(req, pid, m_v0_props.fb_id, b.fb);
				drmModeAtomicAddProperty(req, pid, m_v0_props.crtc_id, m_crtc_id);
				drmModeAtomicAddProperty(req, pid, m_v0_props.src_x, 0);
				drmModeAtomicAddProperty(req, pid, m_v0_props.src_y, 0);
				drmModeAtomicAddProperty(req, pid, m_v0_props.src_w,
					(uint64_t)(uint32_t)b.w << 16);
				drmModeAtomicAddProperty(req, pid, m_v0_props.src_h,
					(uint64_t)(uint32_t)b.h << 16);
				drmModeAtomicAddProperty(req, pid, m_v0_props.crtc_x, 0);
				drmModeAtomicAddProperty(req, pid, m_v0_props.crtc_y, 0);
				drmModeAtomicAddProperty(req, pid, m_v0_props.crtc_w, m_width);
				drmModeAtomicAddProperty(req, pid, m_v0_props.crtc_h, m_height);
				uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
				m_flip_done = false;
				int ret = drmModeAtomicCommit(m_fd, req, flags, &m_flip_done);
				if (ret < 0 && errno == EBUSY)
				{
					/* previous flip still pending (start-up race or
					 * an OSD legacy commit): brief retry instead of
					 * dropping - the decode thread is never stalled */
					for (int r = 0; r < 40 && ret < 0 && !m_present_stop; ++r)
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(5));
						m_flip_done = false;
						ret = drmModeAtomicCommit(m_fd, req, flags, &m_flip_done);
					}
				}
				if (ret == 0)
				{
					waitFlipEvent();
					ok = m_flip_done.load();
				}
				else if (errno == EBUSY)
				{
					static unsigned busy = 0;
					if (++busy == 1 || busy % 100 == 0)
						eWarning("[histb-dec] V0 commit still busy after retries (%u)", busy);
				}
				else
				{
					static int af = 0;
					if (!af++)
						eDebug("[histb-dec] atomic V0 commit failed (%s), legacy SetPlane fallback",
							strerror(errno));
					m_v0_atomic_failed = true;
				}
				drmModeAtomicFree(req);
			}
		}
		if (!ok && m_v0_atomic_failed)
		{
			/* legacy path blocks up to one refresh period, but this
			 * runs on the present thread so decoding is unaffected */
			auto t0 = std::chrono::steady_clock::now();
			int ret = drmModeSetPlane(m_fd, m_v0_plane_id, m_crtc_id, b.fb,
				0, 0, 0, m_width, m_height,
				0, 0, (uint32_t)b.w << 16, (uint32_t)b.h << 16);
			auto us = std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - t0).count();
			unsigned prev = m_v0_setplane_max_us.load();
			while (us > prev && !m_v0_setplane_max_us.compare_exchange_weak(prev, (unsigned)us))
				;
			if (ret == 0)
				ok = true;
			else
			{
				static unsigned spfails = 0;
				if (++spfails == 1 || spfails % 30 == 0)
					eWarning("[histb-dec] drmModeSetPlane(V0) failed (%u total): %s",
						spfails, strerror(errno));
			}
		}
		return ok;
	}

	void presentThread()
	{
		std::unique_lock<std::mutex> lock(m_present_mutex);
		for (;;)
		{
			m_present_cv.wait(lock, [&] { return m_present_stop || !m_present_queue.empty(); });
			if (m_present_queue.empty())
			{
				if (m_present_stop)
					break;
				continue;
			}
			int idx = m_present_queue.front();
			m_present_queue.pop_front();
			m_present_busy = true; /* in flight: pacing counts too */
			/* frame-paced presentation: derive the frame period from
			 * PTS deltas (90 kHz), hold each frame until its scheduled
			 * slot and advance the slot by one period.  Decode bursts
			 * (e.g. after TS gaps) are smoothed out; the commit still
			 * lands on the next vblank, so the display cadence stays
			 * vblank-aligned (25 fps = every 2nd vblank at 50 Hz). */
			V0Buf &b = m_v0_buf[idx];
			if (b.pts != AV_NOPTS_VALUE && m_last_presented_pts != AV_NOPTS_VALUE)
			{
				int64_t d = b.pts - m_last_presented_pts;
				if (d < 0)
					d += (1LL << 33); /* 33-bit PTS wrap */
				if (d > 900 && d < 90000)
				{
					double us = (double)d * 1000000.0 / 90000.0;
					m_frame_period_us += 0.25 * (us - m_frame_period_us);
				}
			}
			m_last_presented_pts = b.pts;
			auto now = std::chrono::steady_clock::now();
			if (m_pacer_slot.time_since_epoch().count() == 0)
				m_pacer_slot = now; /* first frame after (re)start */
			if (now < m_pacer_slot)
			{
				m_present_cv.wait_until(lock, m_pacer_slot,
					[&] { return m_present_stop.load(); });
				now = std::chrono::steady_clock::now();
			}
			if (now > m_pacer_slot + std::chrono::microseconds((int64_t)m_frame_period_us / 2))
				m_pacer_slot = now; /* late (stall/gap): resync */
			m_pacer_slot += std::chrono::microseconds((int64_t)m_frame_period_us);
			lock.unlock();
			bool ok = commitV0(idx);
			m_present_busy = false;
			lock.lock();
			if (ok)
			{
				/* idx is on screen now; the previously displayed
				 * buffer left scanout at this flip and may be
				 * rewritten by the decoder */
				int prev = m_v0_onscreen;
				m_v0_onscreen = idx;
				if (prev >= 0)
					m_v0_free[prev] = 1;
				V0Buf &b = m_v0_buf[idx];
				if (!m_v0_presented)
				{
					m_v0_presented = true;
					eDebug("[histb-dec] first V0 frame presented %dx%d -> %dx%d",
						b.w, b.h, m_width, m_height);
				}
				unsigned frames = ++m_v0_frames;
				if (frames % 500 == 0)
					eDebug("[histb-dec] v0 stats: %u presented, %u dropped (flip-sync)",
						frames, m_v0_dropped.load());
			}
			else
			{
				m_v0_free[idx] = 1;
				m_native_error = true;
				m_native_cv.notify_all();
				eWarning("[histb-dec] compatibility plane commit failed; producer stopped");
			}
			m_present_cv.notify_all();
		}
	}

	bool showFrameV0(const AVDRMFrameDescriptor *desc, int width, int height, int64_t pts)
	{
		if (!initV0Path(width, height))
			return false;
		if (desc->layers[0].nb_planes < 2)
			return false;
		const uint8_t *src[2] = {nullptr, nullptr};
		int strides[2] = {0, 0};
		void *maps[2] = {nullptr, nullptr};
		size_t mapsz[2] = {0, 0};
		if (mapDecoderPlanes(desc, height, src, strides, maps, mapsz) < 0)
			return false;
		startPresentThread();
		int idx = acquireV0Buf(true);
		if (idx < 0)
		{
			unmapDecoderPlanes(desc, maps, mapsz);
			return false;
		}
		V0Buf &b = m_v0_buf[idx];
		b.w = width;
		b.h = height;
		b.pts = pts;
		copyNV12(src, strides, b.map, b.pitch, b.yoff, width, height);
		unmapDecoderPlanes(desc, maps, mapsz);
		enqueueV0(idx);
		return true;
	}

	/* software frame -> NV12 dumb buffer -> V0 plane (format conversion
	 * for YUV420P and friends via sws, double-buffered like the PRIME
	 * dumb path) */
	bool showFrameSwV0(const AVFrame *f, int64_t pts)
	{
		if (!initV0Path(f->width, f->height))
			return false;
		m_v0_sws = sws_getCachedContext(m_v0_sws, f->width, f->height,
			(enum AVPixelFormat)f->format, f->width, f->height,
			AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!m_v0_sws)
			return false;
		startPresentThread();
		int idx = acquireV0Buf();
		if (idx < 0)
		{
			unsigned d = ++m_v0_dropped;
			if (d == 1 || d % 500 == 0)
				eDebug("[histb-dec] V0 buffers exhausted, dropped %u frames", d);
			return true; /* previous frame stays on screen */
		}
		V0Buf &b = m_v0_buf[idx];
		b.w = f->width;
		b.h = f->height;
		b.pts = pts;
		uint8_t *dst[4] = {b.map, b.map + b.yoff, nullptr, nullptr};
		int dststrides[4] = {(int)b.pitch, (int)b.pitch, 0, 0};
		if (sws_scale(m_v0_sws, f->data, f->linesize, 0, f->height, dst, dststrides) < 0)
		{
			static int swwarned = 0;
			if (!swwarned++)
				eWarning("[histb-dec] V0 sw frame copy failed");
			releaseV0Buf(idx);
			return false;
		}
		if (!m_v0_presented)
		{
			/* log before enqueue so the first-frame line keeps its
			 * position relative to the dumb-path one */
			m_v0_presented = true;
			eDebug("[histb-dec] first sw NV12 frame on V0 %dx%d", f->width, f->height);
		}
		enqueueV0(idx);
		return true;
	}

	/* software frame -> XRGB8888 dumb buffer -> legacy CRTC */
	bool showFrameSwXrgb(const AVFrame *f)
	{
		if (!initSwPath())
			return false;
		m_sws = sws_getCachedContext(m_sws, f->width, f->height,
			(enum AVPixelFormat)f->format, m_width, m_height,
			AV_PIX_FMT_BGR0, SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!m_sws)
			return false;
		uint8_t *dst[4] = {m_dumb_map, nullptr, nullptr, nullptr};
		int dststrides[4] = {(int)m_dumb_pitch, 0, 0, 0};
		if (sws_scale(m_sws, f->data, f->linesize, 0, f->height, dst, dststrides) < 0)
			return false;
		if (!m_sw_presented)
		{
			m_sw_presented = true;
			eDebug("[histb-dec] first sw frame presented (XRGB) %dx%d -> %dx%d",
				f->width, f->height, m_width, m_height);
		}
		return presentFb(m_dumb_fb);
	}

	/* legacy flip: first frame binds crtc via modeset, then pageflip
	 * with modeset fallback */
	bool presentFb(uint32_t fb)
	{
		int ret;
		if (!m_fb_bound)
		{
			ret = drmModeSetCrtc(m_fd, m_crtc_id, fb, 0, 0, &m_conn_id, 1, &m_mode);
			if (ret < 0)
			{
				eWarning("[histb-dec] drmModeSetCrtc failed: %s", strerror(errno));
				return false;
			}
			m_fb_bound = true;
			return true;
		}
		ret = drmModePageFlip(m_fd, m_crtc_id, fb, 0, nullptr);
		if (ret < 0)
		{
			/* some vendor drivers only implement full modesets */
			ret = drmModeSetCrtc(m_fd, m_crtc_id, fb, 0, 0, &m_conn_id, 1, &m_mode);
		}
		if (ret < 0)
		{
			static int warned = 0;
			if (!warned++)
				eWarning("[histb-dec] drmModePageFlip/SetCrtc failed: %s", strerror(errno));
			return false;
		}
		return true;
	}

	/* XRGB8888 dumb buffer for the sw conversion path */
	bool initSwPath()
	{
		struct drm_mode_create_dumb creq = {};
		creq.width = m_width;
		creq.height = m_height;
		creq.bpp = 32;
		if (::ioctl(m_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
		{
			eWarning("[histb-dec] create dumb buffer failed: %s", strerror(errno));
			return false;
		}
		uint32_t handles[4] = {creq.handle, 0, 0, 0};
		uint32_t pitches[4] = {creq.pitch, 0, 0, 0};
		uint32_t offsets[4] = {0, 0, 0, 0};
		if (drmModeAddFB2(m_fd, m_width, m_height, DRM_FORMAT_XRGB8888,
			handles, pitches, offsets, &m_dumb_fb, 0) < 0 || !m_dumb_fb)
		{
			eWarning("[histb-dec] AddFB2 dumb buffer failed: %s", strerror(errno));
			destroyDumb(creq.handle);
			return false;
		}
		struct drm_mode_map_dumb mreq = {};
		mreq.handle = creq.handle;
		if (::ioctl(m_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
		{
			eWarning("[histb-dec] map dumb buffer failed: %s", strerror(errno));
			drmModeRmFB(m_fd, m_dumb_fb);
			m_dumb_fb = 0;
			destroyDumb(creq.handle);
			return false;
		}
		m_dumb_map = (uint8_t *)mmap(nullptr, creq.size,
			PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, mreq.offset);
		if (m_dumb_map == MAP_FAILED)
		{
			eWarning("[histb-dec] mmap dumb buffer failed: %s", strerror(errno));
			m_dumb_map = nullptr;
			drmModeRmFB(m_fd, m_dumb_fb);
			m_dumb_fb = 0;
			destroyDumb(creq.handle);
			return false;
		}
		m_dumb_handle = creq.handle;
		m_dumb_pitch = creq.pitch;
		m_dumb_size = creq.size;
		eDebug("[histb-dec] sw path: dumb %dx%d pitch %u",
			m_width, m_height, m_dumb_pitch);
		return true;
	}

	void destroyDumb(uint32_t handle)
	{
		struct drm_mode_destroy_dumb d = {};
		d.handle = handle;
		::ioctl(m_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
	}

	bool probeCard(int fd)
	{
		drmModeResPtr res = drmModeGetResources(fd);
		if (!res)
		{
			eDebug("[histb-dec] drmModeGetResources failed: %s", strerror(errno));
			return false;
		}
		int crtc_id = -1;
		int width = 0, height = 0;
		for (int i = 0; i < res->count_connectors && crtc_id < 0; ++i)
		{
			drmModeConnectorPtr conn = drmModeGetConnector(fd, res->connectors[i]);
			if (!conn)
				continue;
			eDebug("[histb-dec] connector %u connection %d encoder %u",
				conn->connector_id, conn->connection, conn->encoder_id);
			if (conn->connection == DRM_MODE_CONNECTED && conn->encoder_id)
			{
				drmModeEncoderPtr enc = drmModeGetEncoder(fd, conn->encoder_id);
				if (enc && enc->crtc_id)
				{
					crtc_id = enc->crtc_id;
					drmModeCrtcPtr crtc = drmModeGetCrtc(fd, crtc_id);
					if (crtc)
					{
					width = crtc->mode.hdisplay;
					height = crtc->mode.vdisplay;
					/* remember connector + currently active mode so
					 * the legacy CRTC paths can re-bind it */
					m_conn_id = conn->connector_id;
					m_mode = crtc->mode;
					drmModeFreeCrtc(crtc);
				}
			}
			else
				eDebug("[histb-dec] encoder %u has no crtc", conn->encoder_id);
			if (enc)
				drmModeFreeEncoder(enc);
		}
		drmModeFreeConnector(conn);
		}
		if (crtc_id < 0)
		{
			/* atomic drivers do not always report legacy encoder
			 * bindings; fall back to the first connected connector
			 * and the first CRTC (single-pipe hardware) */
			for (int i = 0; i < res->count_connectors && !m_conn_id; ++i)
			{
				drmModeConnectorPtr conn = drmModeGetConnector(fd, res->connectors[i]);
				if (!conn)
					continue;
				if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes)
				{
					m_conn_id = conn->connector_id;
					for (int m = 0; m < conn->count_modes && !m_mode.clock; ++m)
						if (conn->modes[m].type & DRM_MODE_TYPE_PREFERRED)
							m_mode = conn->modes[m];
					if (!m_mode.clock)
						m_mode = conn->modes[0];
				}
				drmModeFreeConnector(conn);
			}
			if (m_conn_id && m_mode.clock && res->count_crtcs)
			{
				crtc_id = res->crtcs[0];
				width = m_mode.hdisplay;
				height = m_mode.vdisplay;
				eDebug("[histb-dec] atomic fallback: crtc %u connector %u %dx%d",
					crtc_id, m_conn_id, width, height);
			}
		}
		if (crtc_id < 0 || !width || !height || !m_mode.clock)
		{
			eDebug("[histb-dec] no connected connector/active crtc crtc %d %dx%d",
				crtc_id, width, height);
			drmModeFreeResources(res);
			return false;
		}
		eDebug("[histb-dec] legacy KMS path: crtc %u connector %u %dx%d",
			crtc_id, m_conn_id, width, height);
		m_crtc_id = crtc_id;
		m_width = width;
		m_height = height;
		drmModeFreeResources(res);
		return true;
	}

	int mapHandles(const AVDRMFrameDescriptor *desc, uint32_t handles[4])
	{
		int nh = 0;
		static bool primelogged = false;
		for (int l = 0; l < desc->nb_layers && nh < 4; ++l)
			for (int p = 0; p < desc->layers[l].nb_planes && nh < 4; ++p)
			{
				int object = desc->layers[l].planes[p].object_index;
				int fd = desc->objects[object].fd;
				if (drmPrimeFDToHandle(m_fd, fd, &handles[nh]) < 0)
				{
					if (!primelogged)
					{
						primelogged = true;
						char lnk[64];
						snprintf(lnk, sizeof(lnk), "/proc/self/fd/%d", fd);
						char tgt[128] = "?";
						ssize_t n = readlink(lnk, tgt, sizeof(tgt) - 1);
						if (n >= 0)
							tgt[n] = 0;
						/* EINVAL from dma_buf_get means the fd is
						 * not a dma-buf at all; anything else
						 * points into the attach/map chain */
						eWarning("[histb-dec] drmPrimeFDToHandle failed: %m (fd %d -> %s, size %lld)",
							fd, tgt, (long long)desc->objects[object].size);
					}
					unmapHandles(handles);
					return -1;
				}
				nh++;
			}
		return nh;
	}

	void unmapHandles(uint32_t handles[4])
	{
		for (int i = 0; i < 4; ++i)
			if (handles[i])
			{
				/* Same DMA-BUF re-imports return the same GEM handle. */
				uint32_t handle = handles[i];
				for (int j = i; j < 4; ++j)
					if (handles[j] == handle) handles[j] = 0;
				struct drm_gem_close c = {handle, 0};
				::ioctl(m_fd, DRM_IOCTL_GEM_CLOSE, &c);
			}
	}

	void retireFrame()
	{
		/* Caller has proved successful plane disable before this cleanup. */
		if (m_native_pending_fb)
		{
			drainRetiredNativeEvents();
			drmModeRmFB(m_fd, m_native_pending_fb);
		}
		m_native_pending_fb = 0;
		m_native_inflight = false;
		if (m_prev_fb)
		{
			drmModeRmFB(m_fd, m_prev_fb);
			m_prev_fb = 0;
		}
		unmapHandles(m_prev_handles);
		if (m_fb)
		{
			drmModeRmFB(m_fd, m_fb);
			m_fb = 0;
		}
		unmapHandles(m_gem_handles);
		if (m_scanout_frame) av_frame_unref(m_scanout_frame);
		for (auto &slot : m_native_slots)
			if (slot.frame) av_frame_unref(slot.frame);
	}

	int m_fd = -1;
	AVFrame *m_scanout_frame = nullptr;
	bool m_native_direct_capture_ready = false;
	struct NativeSlot { AVFrame *frame = nullptr; AVRational rate = {0, 1}; bool busy = false; };
	NativeSlot m_native_slots[2];
	int m_native_queue[2] = {0, 0};
	unsigned m_native_head = 0, m_native_count = 0;
	std::thread m_native_thread;
	std::mutex m_native_mutex;
	std::condition_variable m_native_cv;
	std::atomic<bool> m_native_cancel{true}, m_native_error{false};
	std::atomic<bool> m_native_decode_failed{false};
	bool m_native_running = false;
	unsigned m_native_presented = 0;
	unsigned m_native_completed = 0, m_native_refresh_skipped = 0;
	double m_native_output_period_us = 0.0, m_native_output_source_period_us = 0.0;
	double m_native_output_phase_us = 0.0;
	AVRational m_native_rate = {0, 1};
	double m_native_pts_period_us = 0.0;
	unsigned m_native_pts_candidates = 0;
	uintptr_t m_native_event_cookie = 0;
	uint32_t m_native_pending_fb = 0;
	bool m_native_flip_done = false, m_native_inflight = false;
	bool m_native_atomic_logged = false;
	uint32_t m_crtc_id = 0;
	uint32_t m_conn_id = 0;
	drmModeModeInfo m_mode = {};
	bool m_fb_bound = false;
	bool m_nv12_presented = false;
	bool m_sw_presented = false;
	bool m_sw = false;
	int m_width = 0, m_height = 0;
	uint32_t m_fb = 0;
	uint32_t m_prev_fb = 0;
	uint32_t m_gem_handles[4] = {0, 0, 0, 0};
	uint32_t m_prev_handles[4] = {0, 0, 0, 0};
	uint32_t m_dumb_fb = 0, m_dumb_handle = 0, m_dumb_pitch = 0;
	size_t m_dumb_size = 0;
	uint8_t *m_dumb_map = nullptr;
	struct SwsContext *m_sws = nullptr;
	/* V0 hardware video plane state */
	uint32_t m_v0_plane_id = 0;
	uint32_t m_v0_w = 0, m_v0_h = 0;
	/* flip-synchronised NV12 dumb buffers: one on screen, several
	 * queued (paced), one being filled (a buffer is only rewritten
	 * after the flip that removed it from scanout completed).  The
	 * pool size adapts to the frame size under a 48 MB cap: headroom
	 * to absorb a ~1 s decode output burst (the vendor demux
	 * recorder-path tap delivers data in large periodic bursts). */
	struct V0Buf
	{
		uint32_t fb = 0, handle = 0, pitch = 0, yoff = 0;
		size_t size = 0;
		uint8_t *map = nullptr;
		int w = 0, h = 0;
		int64_t pts = AV_NOPTS_VALUE;
	};
	std::vector<V0Buf> m_v0_buf;
	std::vector<uint8_t> m_v0_free;
	/* presentation stats (logged every N frames) */
	std::atomic<unsigned> m_v0_frames{0};
	std::atomic<unsigned> m_v0_dropped{0};
	std::atomic<unsigned> m_v0_setplane_max_us{0};
	/* present thread state (see the flip-synchronised presentation
	 * comment above) */
	std::thread m_present_thread;
	std::mutex m_present_mutex;
	std::condition_variable m_present_cv;
	std::deque<int> m_present_queue;
	int m_v0_onscreen = -1;
	std::atomic<bool> m_flip_done{false};
	std::atomic<bool> m_present_busy{false};
	std::atomic<bool> m_present_stop{false};
	bool m_present_running = false;
	/* Single active presentation route owns pacing; transition is one-way. */
	std::chrono::steady_clock::time_point m_pacer_slot{};
	int64_t m_last_presented_pts = AV_NOPTS_VALUE;
	double m_frame_period_us = 40000.0;
	struct SwsContext *m_v0_sws = nullptr;
	bool m_v0_inited = false;
	bool m_v0_presented = false;
	/* atomic nonblock commit state for the V0 plane */
	struct V0Props
	{
		uint32_t fb_id = 0, crtc_id = 0;
		uint32_t src_x = 0, src_y = 0, src_w = 0, src_h = 0;
		uint32_t crtc_x = 0, crtc_y = 0, crtc_w = 0, crtc_h = 0;
		bool valid = false;
		bool tried = false;
	};
	V0Props m_v0_props;
	bool m_v0_atomic_failed = false;
	/* direct PRIME->V0 path state */
	bool m_prime_failed = false;
	bool m_prime_presented = false;
};

/* ------------------------------------------------------------------ */
/* audio: TS tap -> avcodec -> swresample -> ALSA                      */
/* ------------------------------------------------------------------ */

class HistbAudioPriv
{
public:
	HistbAudioPriv(eDVBDemux *demux, int dev)
		: m_demux(demux), m_dev(dev)
	{
		m_pes.setHandler([this](const uint8_t *es, size_t len, int64_t pts, bool ra) { feedPes(es, len, pts); });
	}
	~HistbAudioPriv() { stop(); }

	int startPid(int pid, int type)
	{
		stop();
		if (!histb_ff_load())
			return -1;

		static const struct { int e2; enum AVCodecID ff; const char *name; } map[] = {
			{eDVBAudio::aMPEG,  AV_CODEC_ID_MP2,       "mp2"},
			{eDVBAudio::aAC3,   AV_CODEC_ID_AC3,       "ac3"},
			{eDVBAudio::aDTS,   AV_CODEC_ID_DTS,       "dts"},
			{eDVBAudio::aAAC,   AV_CODEC_ID_AAC,       "aac"},
			{eDVBAudio::aAACHE, AV_CODEC_ID_AAC,       "aac"},
			{eDVBAudio::aLPCM,  AV_CODEC_ID_PCM_S16BE, "lpcm"},
			{eDVBAudio::aDTSHD, AV_CODEC_ID_DTS,       "dtshd"},
			{eDVBAudio::aDDP,   AV_CODEC_ID_EAC3,      "eac3"},
			/* hivxe-ffmpeg now registers DRA with AV_CODEC_ID_DRA; resolve by
			 * codec id, falling back to the decoder name "dra" if needed */
			{eDVBAudio::aDRA,   AV_CODEC_ID_DRA,       "dra"},
			{eDVBAudio::aAC4,   AV_CODEC_ID_AC4,       "ac4"},
		};
		const AVCodec *codec = nullptr;
		const char *codecName = nullptr;
		for (auto &m : map)
			if (m.e2 == type)
			{
				codec = avcodec_find_decoder(m.ff);
				codecName = m.name;
				break;
			}
		if (!codec && codecName)
			codec = avcodec_find_decoder_by_name(codecName);
		if (!codec)
		{
			if (codecName)
				eWarning("[histb-dec] audio codec %s missing in hivxe-ffmpeg", codecName);
			else
				eWarning("[histb-dec] no audio decoder for type %d", type);
			return -1;
		}

		m_avctx = avcodec_alloc_context3(codec);
		if (!m_avctx)
			return -1;
		m_avctx->pkt_timebase = AVRational{1, 90000};
		/* TS PES boundaries are not codec frame boundaries. The AC3 decoder
		 * also probes trailing bytes for a dependent frame, so a split next
		 * header must stay in the parser instead of reaching the decoder. */
		if (codec->id == AV_CODEC_ID_MP2 || codec->id == AV_CODEC_ID_AC3 ||
			codec->id == AV_CODEC_ID_EAC3)
		{
			m_parser = av_parser_init(codec->id);
			if (!m_parser)
			{
				eWarning("[histb-dec] audio parser unavailable for %s", codec->name);
				avcodec_free_context(&m_avctx);
				return -1;
			}
		}
		if (avcodec_open2(m_avctx, codec, nullptr) < 0)
		{
			eWarning("[histb-dec] avcodec_open2(%s) failed: %s", codec->name, histb_av_err(0));
			avcodec_free_context(&m_avctx);
			return -1;
		}
		m_pkt = av_packet_alloc();
		m_frame = av_frame_alloc();
		if (!m_pkt || !m_frame)
			return -1;

		if (openAlsa() < 0)
			return -1;

		m_frames = 0;
		m_noframe_count = 0;
		snprintf(m_codec_name, sizeof(m_codec_name), "%s", codec->name);
		if (m_pes.start(m_demux, pid, DMX_PES_AUDIO0 + m_dev) < 0)
			return -1;
		std::lock_guard<std::mutex> lock(m_audio_mutex);
		m_pid = pid;
		m_frozen = false;
		eDebug("[histb-dec] audio pid %04x type %d (%s) started", pid, type, codec->name);
		return 0;
	}

	void stop()
	{
		m_pes.stop();
		std::lock_guard<std::mutex> lock(m_audio_mutex);
		if (m_parser) { av_parser_close(m_parser); m_parser = nullptr; }
		m_write_pts = -1;
		m_next_pts = AV_NOPTS_VALUE;
		m_pts_remainder = 0;
		m_pid = -1;
		closeAlsa();
		if (m_avctx)
		{
			avcodec_free_context(&m_avctx);
			m_avctx = nullptr;
		}
		if (m_pkt)
		{
			av_packet_free(&m_pkt);
			m_pkt = nullptr;
		}
		if (m_frame)
		{
			av_frame_free(&m_frame);
			m_frame = nullptr;
		}
	}

	void flush()
	{
		std::lock_guard<std::mutex> lock(m_audio_mutex);
		if (m_parser)
		{
			av_parser_close(m_parser);
			m_parser = m_avctx ? av_parser_init(m_avctx->codec_id) : nullptr;
			if (m_avctx && !m_parser) eWarning("[histb-dec] audio parser reset failed");
		}
		if (m_avctx) avcodec_flush_buffers(m_avctx);
		if (m_swr) { swr_free(&m_swr); m_swr = nullptr; }
		m_write_pts = -1;
		m_next_pts = AV_NOPTS_VALUE;
		m_pts_remainder = 0;
		if (m_pcm)
		{
			snd_pcm_drop(m_pcm);
			int err = snd_pcm_prepare(m_pcm);
			if (err < 0) eWarning("[histb-dec] audio flush prepare failed: %s", snd_strerror(err));
		}
	}

	void freeze() { m_frozen = true; }
	void unfreeze() { m_frozen = false; }

	int getPTS(pts_t &now)
	{
		std::lock_guard<std::mutex> lock(m_audio_mutex);
		if (m_pid < 0 || m_write_pts < 0 || !m_pcm) return -1;
		snd_pcm_sframes_t delay = 0;
		if (snd_pcm_delay(m_pcm, &delay) < 0) return -1;
		/* Anchor is the end PTS of samples actually accepted by ALSA.
		 * Its live queue delay already includes wall-clock consumption. */
		int64_t pos = m_write_pts - (int64_t)std::max<snd_pcm_sframes_t>(delay, 0) * 90000 / m_sample_rate;
		now = (pos < 0 ? 0 : pos) & ((1LL << 33) - 1);
		return 0;
	}

private:
	static bool isHdmiPcm(const char *name)
	{
		if (name)
			for (; *name; ++name)
				if (strncasecmp(name, "hdmi", 4) == 0) return true;
		return false;
	}

	int openAlsa()
	{
		/* Card/PCM numbering is not an ABI: on DVBIP-1006 hw:0,0 is
		 * analog and hw:0,1 is HDMI. Discover a real playback endpoint. */
		char endpoint[64] = "default";
		bool found = false, discoveryError = false;
		int card = -1, err;
		snd_pcm_info_t *info;
		snd_pcm_info_alloca(&info);
		while (!found && (err = snd_card_next(&card)) >= 0 && card >= 0)
		{
			char control[32]; snprintf(control, sizeof(control), "hw:%d", card);
			snd_ctl_t *ctl = nullptr;
			err = snd_ctl_open(&ctl, control, 0);
			if (err < 0)
			{
				discoveryError = true;
				eWarning("[histb-dec] ALSA enumerate %s failed: %s", control, snd_strerror(err));
				continue;
			}
			int device = -1;
			for (;;)
			{
				err = snd_ctl_pcm_next_device(ctl, &device);
				if (err < 0) { discoveryError = true; break; }
				if (device < 0) break;
				snd_pcm_info_set_device(info, device);
				snd_pcm_info_set_subdevice(info, 0);
				snd_pcm_info_set_stream(info, SND_PCM_STREAM_PLAYBACK);
				err = snd_ctl_pcm_info(ctl, info);
				if (err == -ENOENT) continue; /* capture-only PCM */
				if (err < 0) { discoveryError = true; continue; }
				if (isHdmiPcm(snd_pcm_info_get_id(info)) || isHdmiPcm(snd_pcm_info_get_name(info)))
				{
					snprintf(endpoint, sizeof(endpoint), "hw:%d,%d", card, device);
					found = true;
					break;
				}
			}
			snd_ctl_close(ctl);
		}
		if (!found && (err < 0 || discoveryError))
		{
			eWarning("[histb-dec] ALSA HDMI discovery incomplete; refusing accidental analog fallback");
			return -1;
		}
		eDebug("[histb-dec] ALSA playback endpoint %s (%s)", endpoint,
			found ? "enumerated HDMI" : "no HDMI PCM; system default");
		err = snd_pcm_open(&m_pcm, endpoint, SND_PCM_STREAM_PLAYBACK, 0);
		if (err < 0)
		{
			eWarning("[histb-dec] snd_pcm_open %s failed: %s (no alternate endpoint attempted)", endpoint, snd_strerror(err));
			m_pcm = nullptr;
			return -1;
		}
		snd_pcm_hw_params_t *hw = nullptr;
		snd_pcm_hw_params_malloc(&hw);
		snd_pcm_hw_params_any(m_pcm, hw);
		snd_pcm_hw_params_set_access(m_pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
		snd_pcm_hw_params_set_format(m_pcm, hw, SND_PCM_FORMAT_S16_LE);
		snd_pcm_hw_params_set_channels(m_pcm, hw, 2);
		unsigned rate = 48000;
		snd_pcm_hw_params_set_rate_near(m_pcm, hw, &rate, nullptr);
		m_sample_rate = rate;
		snd_pcm_uframes_t bufsz = 8192;
		snd_pcm_hw_params_set_buffer_size_near(m_pcm, hw, &bufsz);
		err = snd_pcm_hw_params(m_pcm, hw);
		snd_pcm_hw_params_free(hw);
		if (err < 0)
		{
			eWarning("[histb-dec] snd_pcm_hw_params failed: %s", snd_strerror(err));
			closeAlsa();
			return -1;
		}
		snd_pcm_prepare(m_pcm);
		return 0;
	}

	void closeAlsa()
	{
		if (m_swr)
		{
			swr_free(&m_swr);
			m_swr = nullptr;
		}
		if (m_pcm)
		{
			snd_pcm_close(m_pcm);
			m_pcm = nullptr;
		}
	}

	void feedPes(const uint8_t *es, size_t len, int64_t pts)
	{
		std::lock_guard<std::mutex> lock(m_audio_mutex);
		if (!m_avctx || !m_pkt || m_pid < 0 || len == 0)
			return;
		if (!m_frames && ++m_noframe_count % 300 == 0)
			eWarning("[histb-dec] no audio frames after %u pes packets (codec %s)",
				m_noframe_count, m_codec_name);
		uint8_t *data = (uint8_t *)av_malloc(len + AV_INPUT_BUFFER_PADDING_SIZE);
		if (!data)
			return;
		memcpy(data, es, len);
		memset(data + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
		if (m_avctx->codec_id == AV_CODEC_ID_MP2 || m_avctx->codec_id == AV_CODEC_ID_AC3 ||
			m_avctx->codec_id == AV_CODEC_ID_EAC3)
		{
			if (!m_parser)
			{
				eWarning("[histb-dec] audio parser missing after reset (%s)", m_codec_name);
				av_free(data); return;
			}
			const uint8_t *input = data;
			int remaining = int(len);
			int64_t timestamp = pts >= 0 ? pts : AV_NOPTS_VALUE;
			while (remaining > 0)
			{
				uint8_t *packet = nullptr; int packetSize = 0;
				int consumed = av_parser_parse2(m_parser, m_avctx, &packet, &packetSize,
					input, remaining, timestamp, timestamp, -1);
				if (consumed < 0)
				{
					eWarning("[histb-dec] audio parse failed (%s): %s", m_codec_name, histb_av_err(consumed));
					break;
				}
				input += consumed; remaining -= consumed;
				if (consumed) timestamp = AV_NOPTS_VALUE;
				if (packetSize > 0)
				{
					/* Parser data remains valid until the next parse call;
					 * send_packet copies unreferenced input before returning. */
					av_packet_unref(m_pkt);
					m_pkt->data = packet; m_pkt->size = packetSize;
					m_pkt->pts = m_parser->pts; m_pkt->dts = m_parser->dts;
					decodeAudioPacket();
				}
				if (!consumed && !packetSize)
				{
					eWarning("[histb-dec] audio parser made no progress (%s)", m_codec_name);
					break;
				}
			}
			av_free(data);
			return;
		}
		av_packet_unref(m_pkt);
		if (av_packet_from_data(m_pkt, data, (int)len) < 0)
		{
			av_free(data);
			return;
		}
		m_pkt->pts = (pts >= 0) ? pts : AV_NOPTS_VALUE;
		m_pkt->dts = m_pkt->pts;

		decodeAudioPacket();
	}

	void decodeAudioPacket()
	{
		int ret;
		do
		{
			ret = avcodec_send_packet(m_avctx, m_pkt);
			if (ret < 0 && ret != AVERROR(EAGAIN))
			{
				eWarning("[histb-dec] audio packet decode failed: %s", histb_av_err(ret));
				break;
			}
			while (avcodec_receive_frame(m_avctx, m_frame) == 0)
			{
				outputFrame();
				av_frame_unref(m_frame);
			}
		} while (ret == AVERROR(EAGAIN));
		av_packet_unref(m_pkt);
	}

	void outputFrame()
	{
		AVFrame *f = m_frame;
		++m_frames;
		int64_t startPts = f->pts != AV_NOPTS_VALUE ? f->pts : m_next_pts;
		if (!m_pcm || m_frozen)
			return;

		/* convert everything to s16le stereo @ alsa rate */
		if (!m_swr)
		{
			AVChannelLayout out_ch, in_ch;
			av_channel_layout_default(&out_ch, 2);
			AVChannelLayout *in_layout = &f->ch_layout;
			if (in_layout->nb_channels <= 0)
			{
				av_channel_layout_default(&in_ch, 2);
				in_layout = &in_ch;
			}
			if (swr_alloc_set_opts2(&m_swr, &out_ch, AV_SAMPLE_FMT_S16, m_sample_rate,
					in_layout, (enum AVSampleFormat)f->format, f->sample_rate, 0, nullptr) < 0 || swr_init(m_swr) < 0)
			{
				eWarning("[histb-dec] swr setup failed");
				swr_free(&m_swr);
				m_swr = nullptr;
				return;
			}
		}

		int out_frames = f->nb_samples + 256;
		uint8_t *out_data[1] = {(uint8_t *)av_malloc(out_frames * 4 /* stereo s16 */)};
		if (!out_data[0])
			return;
		const uint8_t *in_data[8] = {0};
		for (int p = 0; p < 8; ++p)
			in_data[p] = f->data[p];
		int got = swr_convert(m_swr, out_data, out_frames, (const uint8_t **)in_data, f->nb_samples);
		if (got > 0)
		{
			histbApplyPcmVolume(reinterpret_cast<int16_t *>(out_data[0]), got);
			int wrote = 0;
			while (wrote < got)
			{
				int err = snd_pcm_writei(m_pcm, out_data[0] + wrote * 4, got - wrote);
				if (err > 0)
					wrote += err;
				else if (err == -EPIPE)
					snd_pcm_prepare(m_pcm);
				else if (err == -EAGAIN)
					usleep(2000);
				else
				{
					eWarning("[histb-dec] snd_pcm_writei failed: %s", snd_strerror(err));
					break;
				}
			}
			if (startPts != AV_NOPTS_VALUE)
			{
				/* Retain fractional sample ticks instead of truncating each
				 * 44.1 kHz frame independently. A new explicit PTS reanchors. */
				if (f->pts != AV_NOPTS_VALUE) m_pts_remainder = 0;
				int64_t accepted = int64_t(wrote) * 90000 + m_pts_remainder;
				if (wrote > 0) m_write_pts = startPts + accepted / m_sample_rate;
				int64_t complete = int64_t(got) * 90000 + m_pts_remainder;
				m_next_pts = startPts + complete / m_sample_rate;
				m_pts_remainder = complete % m_sample_rate;
			}
		}
		av_free(out_data[0]);
	}

	ePtr<eDVBDemux> m_demux;
	int m_dev;
	HistbPesDemux m_pes;
	AVCodecContext *m_avctx = nullptr;
	AVCodecParserContext *m_parser = nullptr;
	std::mutex m_audio_mutex;
	AVPacket *m_pkt = nullptr;
	AVFrame *m_frame = nullptr;
	snd_pcm_t *m_pcm = nullptr;
	SwrContext *m_swr = nullptr;
	int64_t m_pts_remainder = 0;
	int m_sample_rate = 48000;
	int m_pid = -1;
	unsigned m_frames = 0;
	unsigned m_noframe_count = 0;
	char m_codec_name[16] = {0};
	std::atomic<bool> m_frozen{false};
	int64_t m_write_pts = -1;
	int64_t m_next_pts = AV_NOPTS_VALUE;
};

/* ------------------------------------------------------------------ */
/* video: TS tap -> avcodec (v4l2request hwaccel) -> DRM plane         */
/* ------------------------------------------------------------------ */

class HistbVideoPriv
{
public:
	HistbVideoPriv(eDVBDemux *demux, int dev)
		: m_demux(demux), m_dev(dev)
	{
		/* cross-thread event marshaling: the decode thread queues events
		 * and pokes the pipe; eDVBVideo (main thread) drains via its
		 * eSocketNotifier and emits from main-loop context.  Emitting the
		 * sigc chain (which reaches Python) directly from the decode
		 * thread segfaults in PyTuple_New. */
		if (pipe2(m_evpipe, O_NONBLOCK | O_CLOEXEC) < 0)
		{
			m_evpipe[0] = m_evpipe[1] = -1;
		}
		m_pes.setHandler([this](const uint8_t *es, size_t len, int64_t pts, bool ra) { feedPes(es, len, pts, ra); });
	}
	~HistbVideoPriv()
	{
		stop();
		for (int i = 0; i < 2; i++)
		{
			if (m_evpipe[i] >= 0)
				::close(m_evpipe[i]);
			m_evpipe[i] = -1;
		}
	}

	int eventFd() const { return m_evpipe[0]; }

	void emitEvent(const struct iTSMPEGDecoder::videoEvent &ev)
	{
		{
			std::lock_guard<std::mutex> l(m_evmutex);
			if (m_evq.size() >= 32)
				m_evq.pop_front();
			m_evq.push_back(ev);
		}
		if (m_evpipe[1] >= 0)
		{
			char c = 1;
			ssize_t r = ::write(m_evpipe[1], &c, 1);
			(void)r;
		}
	}

	/* main thread only */
	void drainEvents()
	{
		char buf[64];
		while (::read(m_evpipe[0], buf, sizeof(buf)) > 0)
			;
		std::deque<struct iTSMPEGDecoder::videoEvent> q;
		{
			std::lock_guard<std::mutex> l(m_evmutex);
			q.swap(m_evq);
		}
		for (const auto &ev : q)
		{
			if (m_event_sink)
				m_event_sink(ev);
		}
	}

	int startPid(int pid, int type)
	{
		stop();
		if (!histb_ff_load())
			return -1;

		/* Stateless v4l2request hwaccel codecs: the software decoder is
		 * selected by id and the hwaccel attaches via hw_device_ctx.
		 * Stateful M2M codecs (MPEG-4 Part 2, VC-1) have no v4l2request
		 * hwaccel; the generic find_decoder would return the software
		 * decoder, so select the histb M2M decoder by name and fall
		 * back to software if it cannot open. */
		const AVCodec *codec = nullptr;
		bool use_hwaccel = false;
		switch (type)
		{
		case eDVBVideo::MPEG2:      codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO); use_hwaccel = true; break;
		case eDVBVideo::MPEG1:      codec = avcodec_find_decoder(AV_CODEC_ID_MPEG1VIDEO); use_hwaccel = true; break;
		case eDVBVideo::MPEG4_H264: codec = avcodec_find_decoder(AV_CODEC_ID_H264);       use_hwaccel = true; break;
		case eDVBVideo::H265_HEVC:  codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);       use_hwaccel = true; break;
		case eDVBVideo::AVS:        codec = avcodec_find_decoder(AV_CODEC_ID_CAVS);       use_hwaccel = true; break;
		case eDVBVideo::MPEG4_Part2:
			codec = avcodec_find_decoder_by_name("mpeg4_v4l2m2m");
			if (!codec)
				codec = avcodec_find_decoder(AV_CODEC_ID_MPEG4);
			break;
		case eDVBVideo::VC1:
			codec = avcodec_find_decoder_by_name("vc1_v4l2m2m");
			if (!codec)
				codec = avcodec_find_decoder(AV_CODEC_ID_VC1);
			break;
		case eDVBVideo::VC1_SM:
			codec = avcodec_find_decoder_by_name("wmv3_v4l2m2m");
			if (!codec)
				codec = avcodec_find_decoder(AV_CODEC_ID_VC1);
			break;
		case eDVBVideo::AVS2:
			/* CV200 VDH has no AVS2 engine (MV310 only upstream) */
			eWarning("[histb-dec] AVS2 hardware decoding unavailable on CV200 (type %d)", type);
			return -1;
		default:
			eWarning("[histb-dec] no video decoder for type %d", type);
			return -1;
		}
		if (!codec)
		{
			eWarning("[histb-dec] video codec for type %d missing in hivxe-ffmpeg", type);
			return -1;
		}

		m_avctx = avcodec_alloc_context3(codec);
		if (!m_avctx)
			return -1;

		if (use_hwaccel)
		{
			/* hardware decode through the v4l2request hwaccel */
			/* One VDH engine; automatic frame threads exhaust UHD capture CMA. */
			m_avctx->thread_count = 1;
			AVBufferRef *hwdev = nullptr;
			if (av_hwdevice_ctx_create(&hwdev, AV_HWDEVICE_TYPE_V4L2REQUEST, nullptr, nullptr, 0) < 0)
			{
				eWarning("[histb-dec] v4l2request hwdevice create failed");
				avcodec_free_context(&m_avctx);
				return -1;
			}
			m_avctx->hw_device_ctx = av_buffer_ref(hwdev);
			m_avctx->get_format = [](AVCodecContext *, const AVPixelFormat *formats) -> AVPixelFormat {
				for (; *formats != AV_PIX_FMT_NONE; ++formats)
					if (*formats == AV_PIX_FMT_DRM_PRIME) return *formats;
				eWarning("[histb-dec] hardware DRM_PRIME unavailable; refusing CPU fallback");
				return AV_PIX_FMT_NONE;
			};
			av_buffer_unref(&hwdev);
		}

		if (avcodec_open2(m_avctx, codec, nullptr) < 0)
		{
			eWarning("[histb-dec] avcodec_open2(%s) failed", codec->name);
			avcodec_free_context(&m_avctx);
			m_avctx = nullptr;
			if (!use_hwaccel && strstr(codec->name, "_v4l2m2m"))
			{
				/* M2M node unavailable: retry with the software decoder
				 * and present through the sw frame path */
				codec = avcodec_find_decoder(codec->id);
				if (!codec)
					return -1;
				m_avctx = avcodec_alloc_context3(codec);
				if (!m_avctx)
					return -1;
				if (avcodec_open2(m_avctx, codec, nullptr) < 0)
				{
					eWarning("[histb-dec] avcodec_open2(%s) failed", codec->name);
					avcodec_free_context(&m_avctx);
					m_avctx = nullptr;
					return -1;
				}
				eDebug("[histb-dec] opened software decoder %s", codec->name);
			}
			else
				return -1;
		}
		m_pkt = av_packet_alloc();
		m_frame = av_frame_alloc();
		if (!m_pkt || !m_frame)
			return -1;
		m_codec_type = codec->id;

		if (!m_drm.open())
		{
			avcodec_free_context(&m_avctx);
			m_avctx = nullptr;
			return -1;
		}
		if (!m_drm.resumeNative()) return -1;

		m_synced = false;
		m_unsynced_count = 0;
		m_first_frame = false;
		m_output_error = false;
		m_output_error_since = 0;
		m_last_recover_try = 0;
		/* DMX_PES_OTHER: the vendor demux delivers TSDEMUX_TAP data on
		 * OTHER (recorder path, verified working on 8001 streaming) and
		 * AUDIO0 taps, but not on VIDEO0 - video tap stayed silent. */
		if (m_pes.start(m_demux, pid, DMX_PES_OTHER) < 0)
			return -1;
		m_pid = pid;
		m_frozen = false;
		eDebug("[histb-dec] video pid %04x type %d (%s) started", pid, type, codec->name);
		return 0;
	}

	void stop()
	{
		m_drm.cancelNative();
		m_pes.stop();
		m_drm.hide();
		m_pid = -1;
		if (m_avctx)
		{
			avcodec_free_context(&m_avctx);
			m_avctx = nullptr;
		}
		if (m_pkt)
		{
			av_packet_free(&m_pkt);
			m_pkt = nullptr;
		}
		if (m_frame)
		{
			av_frame_free(&m_frame);
			m_frame = nullptr;
		}
	}

	void flush()
	{
		m_drm.cancelNative();
		std::lock_guard<std::mutex> lock(m_decode_mutex);
		if (!m_drm.hide()) return;
		if (m_avctx) avcodec_flush_buffers(m_avctx);
		m_output_error = !m_drm.resumeNative();
		m_output_error_since = 0;
		m_last_recover_try = 0;
	}

	void freeze() { m_frozen = true; }
	void unfreeze() { m_frozen = false; }
	int setSlowMotion(int) { return 0; }
	int setFastForward(int) { return 0; }

	int getPTS(pts_t &now)
	{
		if (m_pid < 0 || m_last_pts < 0)
			return -1;
		now = m_last_pts;
		return 0;
	}

	int getWidth() const { return m_width; }
	int getHeight() const { return m_height; }
	int getFrameRate() const { return m_framerate; }
	int getProgressive() const { return m_progressive; }
	int getAspect() const { return m_aspect; }

	/* the owner (eDVBVideo) forwards into its sigc signal; eConnection
	 * needs an iObject owner which the priv class is not */
	void setEventSink(std::function<void(const struct iTSMPEGDecoder::videoEvent &)> sink)
	{
		m_event_sink = std::move(sink);
	}

private:
	void feedPes(const uint8_t *es, size_t len, int64_t pts, bool random_access)
	{
		std::lock_guard<std::mutex> lock(m_decode_mutex);
		if (!m_avctx || !m_pkt || m_pid < 0 || len == 0)
			return;
		if (m_output_error)
		{
			/* A lost/invalid reference must not freeze a live service forever.
			 *
			 * Two distinct failure classes are fused through m_output_error:
			 *  - nativeDecodeFailed(): the VDEC pipeline itself died (e.g. an
			 *    AVS completion error during a TS desync storm) and left an
			 *    invalid DPB; avcodec_flush_buffers() discards it, so we may
			 *    rejoin at a bounded timeout instead of waiting forever for an
			 *    IDR.
			 *  - video output failure (showFrame failed with nativeError()):
			 *    the decode pipeline is still healthy and only the KMS/DRM
			 *    presentation path choked. Recover at the next random access
			 *    point when one is due, but guarantee the live picture can
			 *    never freeze longer than a bounded timeout (~2 s) regardless
			 *    of GOP structure. This is the fix for the "play a while then
			 *    the picture freezes forever" symptom. */
			bool decode_died = m_drm.nativeDecodeFailed();
			bool out_err = m_drm.nativeError();
			if (!decode_died && !out_err)
				return;   /* stop/flush cancellation, not an error: stay fused */
			{
				/* Both failure classes recover through the same gate so a live
				 * picture can never freeze longer than a bounded timeout:
				 *  - decode death (nativeDecodeFailed): the VDEC/native worker
				 *    died (AVS completion error or a lost native frame during a
				 *    TS desync storm). The original code waited forever for a
				 *    clean IDR here, which is exactly the "play a while then
				 *    freeze forever" symptom. avcodec_flush_buffers() clears
				 *    the invalid DPB, so rejoining without an IDR is safe.
				 *  - output failure (nativeError): only the KMS/DRM path choked
				 *    while the decode pipeline is healthy.
				 * Rejoin at the next broadcast random access when one is due,
				 * otherwise force recovery after ~2 s, throttled to one
				 * attempt / 500 ms so a wedged display is not hammered. */
				int64_t now = histb_monotonicUs();
				if (m_output_error_since == 0) m_output_error_since = now;
				if (random_access)
				{
					/* clean rejoin at broadcast random access: no throttle */
				}
				else if (now - m_output_error_since >= 2000000)
				{
					if (now - m_last_recover_try < 500000) return;
					m_last_recover_try = now;
				}
				else
				{
					return;   /* wait for a broadcast IDR or the bounded timeout */
				}
			}
			if (!m_drm.hide()) return;
			av_frame_unref(m_frame);
			avcodec_flush_buffers(m_avctx);
			if (!m_drm.resumeNative()) return;
			m_output_error = false;
			m_output_error_since = 0;
			m_last_recover_try = 0;
			m_synced = false;
			eWarning("[histb-dec] %s; resuming decode%s",
				decode_died ? "discarded failed decode references at broadcast random access"
				            : "recovered from transient video output failure",
				random_access ? " (random access)" : "");
		}
		if (!m_synced)
		{
			if (random_access || histbVideoEntryPoint(m_codec_type, es, len))
			{
				m_synced = true;
				eDebug("[histb-dec] video entry point found after %u pes packets%s",
					m_unsynced_count, random_access ? " (random access flag)" : "");
			}
			else
			{
				if (m_unsynced_count == 0)
					eDebug("[histb-dec] first pes before sync: %zu bytes, head %02x %02x %02x %02x %02x %02x %02x %02x",
						len, es[0], es[1], es[2], es[3],
						len > 4 ? es[4] : 0, len > 5 ? es[5] : 0,
						len > 6 ? es[6] : 0, len > 7 ? es[7] : 0);
				if (++m_unsynced_count % 200 == 0)
					eDebug("[histb-dec] no entry point yet after %u pes packets", m_unsynced_count);
				return;
			}
		}

		uint8_t *data = (uint8_t *)av_malloc(len + AV_INPUT_BUFFER_PADDING_SIZE);
		if (!data)
			return;
		memcpy(data, es, len);
		memset(data + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
		av_packet_unref(m_pkt);
		if (av_packet_from_data(m_pkt, data, (int)len) < 0)
		{
			av_free(data);
			return;
		}
		m_pkt->pts = (pts >= 0) ? pts : AV_NOPTS_VALUE;
		m_pkt->dts = m_pkt->pts;

		int ret;
		do
		{
			ret = avcodec_send_packet(m_avctx, m_pkt);
			if (ret < 0 && ret != AVERROR(EAGAIN)) break;
			while (avcodec_receive_frame(m_avctx, m_frame) == 0)
			{
				outputFrame();
				av_frame_unref(m_frame);
				if (m_output_error) break;
			}
		} while (ret == AVERROR(EAGAIN) && !m_output_error);
		av_packet_unref(m_pkt);
	}

	void outputFrame()
	{
		AVFrame *f = m_frame;
		/* decode output gap detection: a wall-clock gap between
		 * consecutive decoded frames points at TS loss recovery or
		 * decoder stalls - the visible stutter source */
		int64_t now_us = histb_monotonicUs();
		if (m_last_output_us && now_us - m_last_output_us > 150000)
			eDebug("[histb-dec] decode output gap %lld ms (pts %lld)",
				(long long)((now_us - m_last_output_us) / 1000),
				(long long)(f->pts == AV_NOPTS_VALUE ? -1LL : f->pts));
		m_last_output_us = now_us;
		if (!m_first_frame)
		{
			m_first_frame = true;
			eDebug("[histb-dec] first decoded video frame: %dx%d format %d pts %lld",
				f->width, f->height, f->format,
				f->pts == AV_NOPTS_VALUE ? -1LL : (long long)f->pts);
		}
		if (f->pts != AV_NOPTS_VALUE)
			m_last_pts = f->pts;

		/* E2 uses ETSI aspect codes and milli-fps, not sample aspect
		 * alone or the 90 kHz PTS clock. Recheck them on sequence changes
		 * even when coded dimensions remain unchanged. */
		int progressive = (f->flags & AV_FRAME_FLAG_INTERLACED) ? 0 : 1;
		AVRational sar = f->sample_aspect_ratio;
		if (sar.num <= 0 || sar.den <= 0) sar = m_avctx->sample_aspect_ratio;
		if (sar.num <= 0 || sar.den <= 0) sar = AVRational{1, 1};
		int aspect = int64_t(f->width) * sar.num * 3 > int64_t(f->height) * sar.den * 4 ? 3 : 2;
		AVRational rate = m_avctx->framerate;
		if (f->hw_frames_ctx)
		{
			const auto *frames = reinterpret_cast<const AVHWFramesContext *>(f->hw_frames_ctx->data);
			if (frames->device_ctx && frames->device_ctx->type == AV_HWDEVICE_TYPE_V4L2REQUEST && frames->hwctx)
			{
				const auto *request = static_cast<const AVV4L2RequestFramesContext *>(frames->hwctx);
				if (request->frame_rate.num > 0 && request->frame_rate.den > 0) rate = request->frame_rate;
			}
		}
		int framerate = rate.num > 0 && rate.den > 0 ? int(1000LL * rate.num / rate.den) : 0;
		bool size_changed = f->width != m_width || f->height != m_height || aspect != m_aspect;
		bool prog_changed = progressive != m_progressive;
		bool rate_changed = framerate != m_framerate;
		if (size_changed || prog_changed || rate_changed)
		{
			m_width = f->width; m_height = f->height; m_aspect = aspect;
			m_progressive = progressive; m_framerate = framerate;
			struct iTSMPEGDecoder::videoEvent ev = {};
			ev.width = m_width; ev.height = m_height; ev.aspect = m_aspect;
			ev.progressive = m_progressive; ev.framerate = m_framerate;
			if (size_changed) { ev.type = iTSMPEGDecoder::videoEvent::eventSizeChanged; emitEvent(ev); }
			if (prog_changed) { ev.type = iTSMPEGDecoder::videoEvent::eventProgressiveChanged; emitEvent(ev); }
			if (rate_changed) { ev.type = iTSMPEGDecoder::videoEvent::eventFrameRateChanged; emitEvent(ev); }
		}

		if (m_frozen)
			return;
		if (f->format != AV_PIX_FMT_DRM_PRIME)
		{
			/* V4L2 M2M decoders and the v4l2request software fallback
			 * produce NV12/YUV420P frames: present them on the video
			 * plane instead of dropping them */
			if (!m_drm.showFrameSw(f, f->pts))
			{
				static int warned = 0;
				if (!warned++)
					eDebug("[histb-dec] software frame (format %d) not presentable", f->format);
			}
			return;
		}
		if (!m_drm.showFrame(f, m_avctx->framerate))
		{
			m_output_error = true;
			if (m_drm.nativeError())
				eWarning("[histb-dec] video output failed; refusing further decode until restart/flush");
			else eDebug("[histb-dec] native producer cancelled for stop/flush");
		}
	}

	ePtr<eDVBDemux> m_demux;
	int m_dev;
	HistbPesDemux m_pes;
	std::mutex m_decode_mutex;
	AVCodecContext *m_avctx = nullptr;
	AVPacket *m_pkt = nullptr;
	AVFrame *m_frame = nullptr;
	HistbDrmOutput m_drm;
	int64_t m_last_output_us = 0;
	enum AVCodecID m_codec_type = AV_CODEC_ID_NONE;
	int m_pid = -1;
	bool m_synced = false;
	unsigned m_unsynced_count = 0;
	bool m_first_frame = false;
	bool m_output_error = false;
	int64_t m_output_error_since = 0;   /* wall time the output fuse first tripped */
	int64_t m_last_recover_try = 0;     /* last forced-recovery attempt timestamp */
	std::atomic<bool> m_frozen{false};
	int64_t m_last_pts = -1;
	int m_width = 0, m_height = 0, m_framerate = 0, m_aspect = 0, m_progressive = 0;
	std::function<void(const struct iTSMPEGDecoder::videoEvent &)> m_event_sink;
	int m_evpipe[2] = {-1, -1};
	std::mutex m_evmutex;
	std::deque<struct iTSMPEGDecoder::videoEvent> m_evq;
};

/* ------------------------------------------------------------------ */
/* eDVBAudio                                                           */
/* ------------------------------------------------------------------ */

int eDVBAudio::m_debug = -1;

eDVBAudio::eDVBAudio(eDVBDemux *demux, int dev)
	: m_demux(demux), m_fd(-1), m_fd_demux(-1), m_dev(dev), m_is_freezed(0), m_bypass(-1)
{
	m_histb = new HistbAudioPriv(demux, dev);
}

void eDVBAudio::setChannel(int channel)
{
	/* mono downmix not wired yet; ALSA plays stereo */
}

void eDVBAudio::stop()
{
	if (m_histb)
		((HistbAudioPriv *)m_histb)->stop();
}

int eDVBAudio::startPid(int pid, int type)
{
	if (!m_histb)
		return -1;
	return ((HistbAudioPriv *)m_histb)->startPid(pid, type);
}

void eDVBAudio::flush()
{
	if (m_histb)
		((HistbAudioPriv *)m_histb)->flush();
}

void eDVBAudio::freeze()
{
	m_is_freezed = 1;
	if (m_histb)
		((HistbAudioPriv *)m_histb)->freeze();
}

void eDVBAudio::unfreeze()
{
	m_is_freezed = 0;
	if (m_histb)
		((HistbAudioPriv *)m_histb)->unfreeze();
}

int eDVBAudio::getPTS(pts_t &now)
{
	if (!m_histb)
		return -1;
	return ((HistbAudioPriv *)m_histb)->getPTS(now);
}

eDVBAudio::~eDVBAudio()
{
	if (m_histb)
	{
		delete (HistbAudioPriv *)m_histb;
		m_histb = nullptr;
	}
}

/* ------------------------------------------------------------------ */
/* eDVBVideo                                                           */
/* ------------------------------------------------------------------ */

int eDVBVideo::m_debug = -1;
int eDVBVideo::m_close_invalidates_attributes = 0;

eDVBVideo::eDVBVideo(eDVBDemux *demux, int dev, bool fcc_enable)
	: m_demux(demux), m_fd(-1), m_fd_demux(-1), m_dev(dev), m_fcc_enable(fcc_enable),
	m_is_slow_motion(0), m_is_fast_forward(0), m_is_freezed(0),
	m_width(0), m_height(0), m_framerate(0), m_aspect(0), m_progressive(0), m_gamma(0), m_streamtype(-1)
{
	m_hdr_detector = nullptr;
	m_hdr_gamma = 0;
	m_driver_gamma = 0;
	m_hdr_gamma_authoritative = false;
	m_gamma_from_driver_event = false;
	HistbVideoPriv *v = new HistbVideoPriv(demux, dev);
	m_histb = v;
	/* deliver decode-thread events from main-loop context (see
	 * HistbVideoPriv::emitEvent).  The 4th argument of create() is
	 * "startnow" - passing false leaves the notifier inactive and
	 * nobody would ever drain the event pipe, so events (and with
	 * them evVideoSizeChanged / the video resolution in the UI)
	 * would never reach anything. */
	int evfd = v->eventFd();
	if (evfd >= 0)
	{
		m_sn = eSocketNotifier::create(eApp, evfd, eSocketNotifier::Read, true);
		CONNECT(m_sn->activated, eDVBVideo::video_event);
	}
}

/* main thread: drain queued events from the decode thread and emit */
void eDVBVideo::video_event(int what)
{
	(void)what;
	if (m_histb)
		((HistbVideoPriv *)m_histb)->drainEvents();
}

void eDVBVideo::stop()
{
	if (m_histb)
	{
		HistbVideoPriv *v = (HistbVideoPriv *)m_histb;
		v->stop();
		m_width = v->getWidth();
		m_height = v->getHeight();
		m_framerate = v->getFrameRate();
		m_aspect = v->getAspect();
		m_progressive = v->getProgressive();
	}
}

int eDVBVideo::startPid(int pid, int type)
{
	if (!m_histb)
		return -1;
	m_streamtype = type;
	return ((HistbVideoPriv *)m_histb)->startPid(pid, type);
}

void eDVBVideo::flush()
{
	if (m_histb)
		((HistbVideoPriv *)m_histb)->flush();
}

void eDVBVideo::freeze()
{
	m_is_freezed = 1;
	if (m_histb)
		((HistbVideoPriv *)m_histb)->freeze();
}

int eDVBVideo::setSlowMotion(int repeat)
{
	if (m_histb)
		return ((HistbVideoPriv *)m_histb)->setSlowMotion(repeat);
	return 0;
}

int eDVBVideo::setFastForward(int skip)
{
	if (m_histb)
		return ((HistbVideoPriv *)m_histb)->setFastForward(skip);
	return 0;
}

void eDVBVideo::unfreeze()
{
	m_is_freezed = 0;
	if (m_histb)
		((HistbVideoPriv *)m_histb)->unfreeze();
}

int eDVBVideo::getPTS(pts_t &now)
{
	if (!m_histb)
		return -1;
	return ((HistbVideoPriv *)m_histb)->getPTS(now);
}

eDVBVideo::~eDVBVideo()
{
	if (m_histb)
	{
		delete (HistbVideoPriv *)m_histb;
		m_histb = nullptr;
	}
}

RESULT eDVBVideo::connectEvent(const sigc::slot<void(struct iTSMPEGDecoder::videoEvent)> &event, ePtr<eConnection> &conn)
{
	if (!m_histb)
		return -1;
	((HistbVideoPriv *)m_histb)->setEventSink([this](const struct iTSMPEGDecoder::videoEvent &ev) {
		/* cache the decoded attributes: iServiceInformation
		 * (sVideoWidth/sVideoHeight/...) reads the getters below,
		 * which otherwise stay 0 until stop() and the UI cannot
		 * show the video's original resolution */
		if (ev.type == iTSMPEGDecoder::videoEvent::eventSizeChanged ||
			ev.type == iTSMPEGDecoder::videoEvent::eventProgressiveChanged ||
			ev.type == iTSMPEGDecoder::videoEvent::eventFrameRateChanged)
		{
			m_width = ev.width;
			m_height = ev.height;
			m_aspect = ev.aspect;
			m_progressive = ev.progressive;
			m_framerate = ev.framerate;
		}
		m_event(ev);
	});
	conn = new eConnection(this, m_event.connect(event));
	return 0;
}

int eDVBVideo::getWidth() { return m_width; }
int eDVBVideo::getHeight() { return m_height; }
int eDVBVideo::getProgressive() { return m_progressive; }
int eDVBVideo::getFrameRate() { return m_framerate; }
int eDVBVideo::getAspect() { return m_aspect; }
int eDVBVideo::getGamma() { return m_gamma; }

/* ------------------------------------------------------------------ */
/* eDVBPCR / eDVBTText: no kernel PCR/teletext decoder on histb        */
/* ------------------------------------------------------------------ */

int eDVBPCR::m_debug = -1;

eDVBPCR::eDVBPCR(eDVBDemux *demux, int dev)
	: m_demux(demux), m_fd_demux(-1), m_dev(dev)
{
}

int eDVBPCR::startPid(int pid)
{
	/* software clock: the audio decoder PTS is the master clock */
	eDebug("[histb-dec] PCR pid %04x ignored (software sync)", pid);
	return 0;
}

void eDVBPCR::stop()
{
}

eDVBPCR::~eDVBPCR()
{
}

int eDVBTText::m_debug = -1;

eDVBTText::eDVBTText(eDVBDemux *demux, int dev)
	: m_demux(demux), m_fd_demux(-1), m_dev(dev)
{
}

int eDVBTText::startPid(int pid)
{
	/* teletext is read by eDVBTeletextParser with its own PES reader */
	return 0;
}

void eDVBTText::stop()
{
}

eDVBTText::~eDVBTText()
{
}

/*
 * The classic DEFINE_REF lines (AddRef/Release) live in the decoder.cpp
 * span excluded by the HAVE_HISTB #ifdef; emit them here so refcounting
 * and the vtables resolve for the histb backend classes.
 */
DEFINE_REF(eDVBAudio);
DEFINE_REF(eDVBVideo);
DEFINE_REF(eDVBPCR);
DEFINE_REF(eDVBTText);

#endif /* __lib_dvb_decoder_histb_impl_h */

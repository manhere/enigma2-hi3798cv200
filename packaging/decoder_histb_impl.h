/*
 * DVBIP-1002 (Hi3798CV200 histb) software decoder backend.
 *
 * Compiled into decoder.cpp when HAVE_HISTB is defined.  Replaces the
 * classic DVB video/audio device implementation (which needs
 * /dev/dvb/adapterN/{video,audio}0, absent on the histb stack) with:
 *
 *   demux PES tap (DMX_OUT_TSDEMUX_TAP) -> user-space TS/PES demux
 *     video: avcodec (hivxe-ffmpeg) with the v4l2request hwaccel
 *            (AVS+/CAVS, H.264, HEVC, MPEG2, ...) -> AV_PIX_FMT_DRM_PRIME
 *            dmabufs -> histb-vdp DRM overlay plane
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
#include <functional>
#include <thread>
#include <sys/ioctl.h>

#include <linux/dvb/dmx.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <alsa/asoundlib.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libswresample/swresample.h>

#include <lib/base/eerror.h>
#include <lib/base/thread.h>
#include <lib/dvb/decoder.h>

/* ------------------------------------------------------------------ */
/* hivxe-ffmpeg loaded with dlopen so enigma2 stays startable without  */
/* the extra add-on.                                                   */
/* ------------------------------------------------------------------ */

#define HISTB_FF_LIBDIR "/usr/local/lib/hivxe-ffmpeg"

#define FF_SYM(ret, name, ...) \
	static ret (*name)(__VA_ARGS__) = nullptr

FF_SYM(unsigned, avcodec_version, void);
FF_SYM(void, av_log_set_level, int level);
FF_SYM(const AVCodec *, avcodec_find_decoder, enum AVCodecID id);
FF_SYM(AVCodecContext *, avcodec_alloc_context3, const AVCodec *codec);
FF_SYM(void, avcodec_free_context, AVCodecContext **avctx);
FF_SYM(int, avcodec_open2, AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options);
FF_SYM(int, avcodec_send_packet, AVCodecContext *avctx, const AVPacket *avpkt);
FF_SYM(int, avcodec_receive_frame, AVCodecContext *avctx, AVFrame *frame);
FF_SYM(void, avcodec_flush_buffers, AVCodecContext *avctx);
FF_SYM(AVPacket *, av_packet_alloc, void);
FF_SYM(void, av_packet_free, AVPacket **pkt);
FF_SYM(void, av_packet_unref, AVPacket *pkt);
FF_SYM(int, av_packet_from_data, AVPacket *pkt, uint8_t *data, int size);
FF_SYM(AVFrame *, av_frame_alloc, void);
FF_SYM(void, av_frame_free, AVFrame **frame);
FF_SYM(void, av_frame_unref, AVFrame *frame);
FF_SYM(AVBufferRef *, av_buffer_ref, AVBufferRef *buf);
FF_SYM(void, av_buffer_unref, AVBufferRef **buf);
FF_SYM(int, av_hwdevice_ctx_create, AVBufferRef **device_ref, enum AVHWDeviceType type, const char *device, AVDictionary *opts, int flags);
FF_SYM(void *, av_malloc, size_t size);
FF_SYM(void, av_free, void *ptr);
FF_SYM(int, av_strerror, int errnum, char *errbuf, size_t errbuf_size);
FF_SYM(void, av_channel_layout_default, AVChannelLayout *ch_layout, int nb_channels);
FF_SYM(struct SwrContext *, swr_alloc_set_opts2, struct SwrContext **ps, const AVChannelLayout *out_ch_layout, enum AVSampleFormat out_sample_fmt, int out_sample_rate, const AVChannelLayout *in_ch_layout, enum AVSampleFormat in_sample_fmt, int in_sample_rate, int log_offset, void *log_ctx);
FF_SYM(int, swr_init, struct SwrContext *s);
FF_SYM(void, swr_free, struct SwrContext **s);
FF_SYM(int, swr_convert, struct SwrContext *s, uint8_t **out, int out_count, const uint8_t **in, int in_count);

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

	void *h[3] = {nullptr, nullptr, nullptr};
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
	if (failed)
		return false;

#define FF_BIND(name) \
	do { \
		void *sym = nullptr; \
		for (int i = 0; i < 3 && !sym; ++i) \
			if (h[i]) sym = dlsym(h[i], #name); \
		if (!sym) { eWarning("[histb-dec] missing symbol %s", #name); return false; } \
		name = (decltype(name))sym; \
	} while (0)

	FF_BIND(avcodec_version);
	FF_BIND(av_log_set_level);
	FF_BIND(avcodec_find_decoder);
	FF_BIND(avcodec_alloc_context3);
	FF_BIND(avcodec_free_context);
	FF_BIND(avcodec_open2);
	FF_BIND(avcodec_send_packet);
	FF_BIND(avcodec_receive_frame);
	FF_BIND(avcodec_flush_buffers);
	FF_BIND(av_packet_alloc);
	FF_BIND(av_packet_free);
	FF_BIND(av_packet_unref);
	FF_BIND(av_packet_from_data);
	FF_BIND(av_frame_alloc);
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
#undef FF_BIND

	av_log_set_level(AV_LOG_ERROR);
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
		m_fd_demux = demux->openDemux();
		if (m_fd_demux < 0)
		{
			eWarning("[histb-dec] unable to open demux");
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
		m_partial.clear();
		m_pes.clear();
		m_stop = false;
		m_thread = std::thread(&HistbPesDemux::thread, this);
		return 0;
	}

	void stop()
	{
		m_stop = true;
		if (m_thread.joinable())
			m_thread.join();
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

	void thread()
	{
		std::vector<uint8_t> buffer(TSPacketSize * 512);
		struct pollfd pfd = {m_fd_demux, POLLIN, 0};
		while (!m_stop)
		{
			if (::poll(&pfd, 1, 100) <= 0)
				continue;
			ssize_t length = ::read(m_fd_demux, buffer.data(), buffer.size());
			if (length < 0)
			{
				if (errno == EOVERFLOW)
				{
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
			if (!(afc & 1))
				continue;
			int cc = packet[3] & 0x0f;
			bool discontinuity = (afc & 2) && packet[4] && (packet[5] & 0x80);
			if (m_cc >= 0 && !discontinuity)
			{
				if (cc == m_cc)
					continue;
				if (cc != ((m_cc + 1) & 0x0f))
				{
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
		size_t header = 9 + m_pes[8];
		if (header >= m_pes.size())
			return;
		int64_t pts = -1;
		if ((m_pes[7] & 0x80) && m_pes.size() >= 14)
			pts = ((int64_t)(m_pes[9] & 0x0e) << 29) | (m_pes[10] << 22) | ((m_pes[11] & 0xfe) << 14) | (m_pes[12] << 7) | (m_pes[13] >> 1);
		if (m_handler)
			m_handler(&m_pes[header], m_pes.size() - header, pts, m_random_access);
	}

	int m_fd_demux = -1;
	int m_pid = -1;
	int m_cc = -1;
	bool m_pes_broken = true;
	bool m_random_access = false;
	std::atomic<bool> m_stop{false};
	std::vector<uint8_t> m_partial;
	std::vector<uint8_t> m_pes;
	std::thread m_thread;
	PesHandler m_handler;
};

/* ------------------------------------------------------------------ */
/* DRM overlay plane output (histb-vdp)                                */
/* ------------------------------------------------------------------ */

class HistbDrmOutput
{
public:
	~HistbDrmOutput() { close(); }

	bool open()
	{
		if (m_fd >= 0)
			return true;
		for (int card = 0; card < 4; ++card)
		{
			char path[64];
			snprintf(path, sizeof(path), "/dev/dri/card%d", card);
			int fd = ::open(path, O_RDWR | O_CLOEXEC);
			if (fd < 0)
				continue;
			if (!probeCard(fd))
			{
				::close(fd);
				continue;
			}
			m_fd = fd;
			/* make sure we can drive the plane even when fbcon holds KMS */
			if (drmSetMaster(m_fd) == 0)
				m_master = true;
			eDebug("[histb-dec] DRM output on card%d plane %u crtc %u %dx%d",
				card, m_plane_id, m_crtc_id, m_width, m_height);
			return true;
		}
		eWarning("[histb-dec] no usable DRM card/overlay plane found");
		return false;
	}

	void close()
	{
		if (m_fd < 0)
			return;
		hide();
		if (m_master)
			drmDropMaster(m_fd);
		::close(m_fd);
		m_fd = -1;
		m_master = false;
	}

	/* show one NV12 dmabuf described by an AVDRMFrameDescriptor */
	bool showFrame(const AVDRMFrameDescriptor *desc, int width, int height)
	{
		if (m_fd < 0 || !desc || desc->nb_layers < 1 || desc->nb_objects < 1)
			return false;
		if (desc->layers[0].format != DRM_FORMAT_NV12)
		{
			eDebug("[histb-dec] unsupported DRM frame format %08x", desc->layers[0].format);
			return false;
		}

		uint32_t handles[4] = {0, 0, 0, 0};
		if (mapHandles(desc, handles) < 0)
			return false;

		uint32_t pitches[2], offsets[2];
		pitches[0] = desc->layers[0].planes[0].pitch;
		offsets[0] = desc->layers[0].planes[0].offset;
		pitches[1] = desc->layers[0].nb_planes > 1 ? desc->layers[0].planes[1].pitch : 0;
		offsets[1] = desc->layers[0].nb_planes > 1 ? desc->layers[0].planes[1].offset : 0;

		uint32_t fb = 0;
		int ret = drmModeAddFB2(m_fd, width, height, DRM_FORMAT_NV12,
			handles, pitches, offsets, &fb, 0);
		if (ret < 0 || !fb)
		{
			eWarning("[histb-dec] drmModeAddFB2 failed: %s", strerror(errno));
			unmapHandles(handles);
			return false;
		}

		ret = drmModeSetPlane(m_fd, m_plane_id, m_crtc_id, fb, 0,
			0, 0, m_width << 16, m_height << 16,
			0, 0, width << 16, height << 16);
		if (ret < 0)
		{
			static int warned = 0;
			if (!warned++)
				eWarning("[histb-dec] drmModeSetPlane failed: %s", strerror(errno));
			drmModeRmFB(m_fd, fb);
			unmapHandles(handles);
			return false;
		}

		retireFrame();
		m_fb = fb;
		memcpy(m_gem_handles, handles, sizeof(handles));
		return true;
	}

	void hide()
	{
		if (m_fd >= 0 && m_plane_id && m_crtc_id)
			drmModeSetPlane(m_fd, m_plane_id, m_crtc_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
		retireFrame();
	}

	bool valid() const { return m_fd >= 0; }

private:
	bool probeCard(int fd)
	{
		drmModeResPtr res = drmModeGetResources(fd);
		if (!res)
			return false;
		int crtc_id = -1;
		int width = 0, height = 0;
		for (int i = 0; i < res->count_connectors && crtc_id < 0; ++i)
		{
			drmModeConnectorPtr conn = drmModeGetConnector(fd, res->connectors[i]);
			if (!conn)
				continue;
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
						drmModeFreeCrtc(crtc);
					}
				}
				if (enc)
					drmModeFreeEncoder(enc);
			}
			drmModeFreeConnector(conn);
		}
		if (crtc_id < 0 || !width || !height)
		{
			drmModeFreeResources(res);
			return false;
		}

		drmModePlaneResPtr planes = drmModeGetPlaneResources(fd);
		if (!planes)
		{
			drmModeFreeResources(res);
			return false;
		}
		uint32_t plane_id = 0;
		for (uint32_t i = 0; i < planes->count_planes && !plane_id; ++i)
		{
			drmModePlanePtr plane = drmModeGetPlane(fd, planes->planes[i]);
			if (!plane)
				continue;
			drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
			if (props)
			{
				for (uint32_t p = 0; p < props->count_props; ++p)
				{
					drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[p]);
					if (prop && !strcmp(prop->name, "type") && props->prop_values[p] == DRM_PLANE_TYPE_OVERLAY)
						plane_id = plane->plane_id;
					if (prop)
						drmModeFreeProperty(prop);
					if (plane_id)
						break;
				}
				drmModeFreeObjectProperties(props);
			}
			drmModeFreePlane(plane);
		}
		drmModeFreePlaneResources(planes);
		drmModeFreeResources(res);
		if (!plane_id)
			return false;
		m_crtc_id = crtc_id;
		m_plane_id = plane_id;
		m_width = width;
		m_height = height;
		return true;
	}

	int mapHandles(const AVDRMFrameDescriptor *desc, uint32_t handles[4])
	{
		int nh = 0;
		for (int l = 0; l < desc->nb_layers && nh < 4; ++l)
			for (int p = 0; p < desc->layers[l].nb_planes && nh < 4; ++p)
			{
				int object = desc->layers[l].planes[p].object_index;
				int fd = desc->objects[object].fd;
				if (drmPrimeFDToHandle(m_fd, fd, &handles[nh]) < 0)
				{
					eWarning("[histb-dec] drmPrimeFDToHandle failed: %m");
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
				struct drm_gem_close c = {handles[i], 0};
				::ioctl(m_fd, DRM_IOCTL_GEM_CLOSE, &c);
				handles[i] = 0;
			}
	}

	void retireFrame()
	{
		if (m_fb)
		{
			drmModeRmFB(m_fd, m_fb);
			m_fb = 0;
		}
		unmapHandles(m_gem_handles);
	}

	int m_fd = -1;
	bool m_master = false;
	uint32_t m_crtc_id = 0;
	uint32_t m_plane_id = 0;
	int m_width = 0, m_height = 0;
	uint32_t m_fb = 0;
	uint32_t m_gem_handles[4] = {0, 0, 0, 0};
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
		};
		const AVCodec *codec = nullptr;
		for (auto &m : map)
			if (m.e2 == type)
			{
				codec = avcodec_find_decoder(m.ff);
				if (!codec)
					eWarning("[histb-dec] audio codec %s missing in hivxe-ffmpeg", m.name);
				break;
			}
		/* aDRA / aAC4 unsupported by hivxe-ffmpeg for now */
		if (!codec)
		{
			eWarning("[histb-dec] no audio decoder for type %d (DRA/AC4 pending)", type);
			return -1;
		}

		m_avctx = avcodec_alloc_context3(codec);
		if (!m_avctx)
			return -1;
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

		if (m_pes.start(m_demux, pid, DMX_PES_AUDIO0 + m_dev) < 0)
			return -1;
		m_pid = pid;
		m_frozen = false;
		eDebug("[histb-dec] audio pid %04x type %d (%s) started", pid, type, codec->name);
		return 0;
	}

	void stop()
	{
		m_pes.stop();
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
		if (m_avctx)
			avcodec_flush_buffers(m_avctx);
		if (m_pcm)
			snd_pcm_drop(m_pcm);
	}

	void freeze() { m_frozen = true; }
	void unfreeze() { m_frozen = false; }

	int getPTS(pts_t &now)
	{
		if (m_pid < 0 || m_write_pts < 0)
			return -1;
		/* playback position: PTS at last write minus time since minus delay */
		int64_t elapsed = (histb_monotonicUs() - m_write_wall) * 9 / 100; /* us -> 90 kHz */
		snd_pcm_sframes_t delay = 0;
		if (m_pcm && snd_pcm_delay(m_pcm, &delay) == 0 && delay > 0)
			elapsed += (int64_t)delay * 90000 / m_sample_rate;
		int64_t pos = m_write_pts - elapsed;
		if (pos < 0)
			pos = 0;
		now = pos;
		return 0;
	}

private:
	int openAlsa()
	{
		int err = snd_pcm_open(&m_pcm, "hw:0,0", SND_PCM_STREAM_PLAYBACK, 0);
		if (err < 0)
		{
			eDebug("[histb-dec] snd_pcm_open hw:0,0 failed (%s), trying default", snd_strerror(err));
			err = snd_pcm_open(&m_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
		}
		if (err < 0)
		{
			eWarning("[histb-dec] snd_pcm_open failed: %s", snd_strerror(err));
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
		if (!m_avctx || !m_pkt || m_pid < 0 || len == 0)
			return;
		uint8_t *data = (uint8_t *)av_malloc(len);
		if (!data)
			return;
		memcpy(data, es, len);
		av_packet_unref(m_pkt);
		if (av_packet_from_data(m_pkt, data, (int)len) < 0)
		{
			av_free(data);
			return;
		}
		m_pkt->pts = (pts >= 0) ? pts : AV_NOPTS_VALUE;
		m_pkt->dts = m_pkt->pts;

		int ret = avcodec_send_packet(m_avctx, m_pkt);
		av_packet_unref(m_pkt);
		if (ret < 0 && ret != AVERROR(EAGAIN))
			return;
		while (avcodec_receive_frame(m_avctx, m_frame) == 0)
		{
			outputFrame();
			av_frame_unref(m_frame);
		}
	}

	void outputFrame()
	{
		AVFrame *f = m_frame;
		if (f->pts != AV_NOPTS_VALUE)
		{
			m_write_pts = f->pts;
			m_write_wall = histb_monotonicUs();
		}
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
					in_layout, f->format, f->sample_rate, 0, nullptr) < 0 || swr_init(m_swr) < 0)
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
		}
		av_free(out_data[0]);
	}

	ePtr<eDVBDemux> m_demux;
	int m_dev;
	HistbPesDemux m_pes;
	AVCodecContext *m_avctx = nullptr;
	AVPacket *m_pkt = nullptr;
	AVFrame *m_frame = nullptr;
	snd_pcm_t *m_pcm = nullptr;
	SwrContext *m_swr = nullptr;
	int m_sample_rate = 48000;
	int m_pid = -1;
	std::atomic<bool> m_frozen{false};
	int64_t m_write_pts = -1;
	int64_t m_write_wall = 0;
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
		m_pes.setHandler([this](const uint8_t *es, size_t len, int64_t pts, bool ra) { feedPes(es, len, pts, ra); });
	}
	~HistbVideoPriv() { stop(); }

	int startPid(int pid, int type)
	{
		stop();
		if (!histb_ff_load())
			return -1;

		const AVCodec *codec = nullptr;
		switch (type)
		{
		case eDVBVideo::MPEG2:      codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO); break;
		case eDVBVideo::MPEG1:      codec = avcodec_find_decoder(AV_CODEC_ID_MPEG1VIDEO); break;
		case eDVBVideo::MPEG4_H264: codec = avcodec_find_decoder(AV_CODEC_ID_H264);       break;
		case eDVBVideo::MPEG4_Part2: codec = avcodec_find_decoder(AV_CODEC_ID_MPEG4);     break;
		case eDVBVideo::H265_HEVC:  codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);       break;
		case eDVBVideo::AVS:        codec = avcodec_find_decoder(AV_CODEC_ID_CAVS);       break;
		default:
			eWarning("[histb-dec] no video decoder for type %d (VC1/AVS2 pending)", type);
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

		/* hardware decode through the v4l2request hwaccel */
		AVBufferRef *hwdev = nullptr;
		if (av_hwdevice_ctx_create(&hwdev, AV_HWDEVICE_TYPE_V4L2REQUEST, nullptr, nullptr, 0) < 0)
		{
			eWarning("[histb-dec] v4l2request hwdevice create failed");
			avcodec_free_context(&m_avctx);
			return -1;
		}
		m_avctx->hw_device_ctx = av_buffer_ref(hwdev);
		av_buffer_unref(&hwdev);

		if (avcodec_open2(m_avctx, codec, nullptr) < 0)
		{
			eWarning("[histb-dec] avcodec_open2(%s) failed", codec->name);
			avcodec_free_context(&m_avctx);
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

		m_synced = false;
		if (m_pes.start(m_demux, pid, DMX_PES_VIDEO0 + m_dev) < 0)
			return -1;
		m_pid = pid;
		m_frozen = false;
		eDebug("[histb-dec] video pid %04x type %d (%s) started", pid, type, codec->name);
		return 0;
	}

	void stop()
	{
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
		if (m_avctx)
			avcodec_flush_buffers(m_avctx);
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
	/* parameter sets or a picture to start the decoding with */
	bool entryPoint(const uint8_t *es, size_t length) const
	{
		for (size_t i = 0; i + 3 < length; ++i)
		{
			if (es[i] || es[i + 1] || es[i + 2] != 1)
				continue;
			uint8_t code = es[i + 3];
			switch (m_codec_type)
			{
			case AV_CODEC_ID_MPEG2VIDEO:
			case AV_CODEC_ID_MPEG1VIDEO:
				return code == 0xb3; /* sequence header */
			case AV_CODEC_ID_H264:
				return (code & 0x1f) == 7 || (code & 0x1f) == 5; /* SPS or IDR */
			case AV_CODEC_ID_HEVC:
			{
				int t = (code >> 1) & 0x3f;
				return (t >= 16 && t <= 21) || t == 32 || t == 33; /* IRAP/VPS/SPS */
			}
			case AV_CODEC_ID_CAVS:
				return code == 0xb3; /* sequence header */
			default:
				return true;
			}
		}
		return false;
	}

	void feedPes(const uint8_t *es, size_t len, int64_t pts, bool random_access)
	{
		if (!m_avctx || !m_pkt || m_pid < 0 || len == 0)
			return;
		if (!m_synced)
		{
			if (random_access || entryPoint(es, len))
				m_synced = true;
			else
				return;
			eDebug("[histb-dec] video entry point found");
		}

		uint8_t *data = (uint8_t *)av_malloc(len);
		if (!data)
			return;
		memcpy(data, es, len);
		av_packet_unref(m_pkt);
		if (av_packet_from_data(m_pkt, data, (int)len) < 0)
		{
			av_free(data);
			return;
		}
		m_pkt->pts = (pts >= 0) ? pts : AV_NOPTS_VALUE;
		m_pkt->dts = m_pkt->pts;

		int ret = avcodec_send_packet(m_avctx, m_pkt);
		av_packet_unref(m_pkt);
		if (ret < 0 && ret != AVERROR(EAGAIN))
			return;
		while (avcodec_receive_frame(m_avctx, m_frame) == 0)
		{
			outputFrame();
			av_frame_unref(m_frame);
		}
	}

	void outputFrame()
	{
		AVFrame *f = m_frame;
		if (f->pts != AV_NOPTS_VALUE)
			m_last_pts = f->pts;

		bool size_changed = f->width != m_width || f->height != m_height;
		if (size_changed)
		{
			m_width = f->width;
			m_height = f->height;
			if (f->sample_aspect_ratio.num && f->sample_aspect_ratio.den)
				m_aspect = f->sample_aspect_ratio.num > f->sample_aspect_ratio.den ? 1 : 0; /* 1 = 16:9 */
			m_framerate = m_avctx->framerate.den ? (int)(90000LL * m_avctx->framerate.num / m_avctx->framerate.den) : 0;
			m_progressive = 1;
			struct iTSMPEGDecoder::videoEvent ev;
			memset(&ev, 0, sizeof(ev));
			ev.type = iTSMPEGDecoder::videoEvent::eventSizeChanged;
			ev.width = m_width;
			ev.height = m_height;
			ev.aspect = m_aspect;
			ev.progressive = m_progressive;
			ev.framerate = m_framerate;
			if (m_event_sink)
				m_event_sink(ev);
		}

		if (m_frozen)
			return;
		if (f->format != AV_PIX_FMT_DRM_PRIME)
		{
			static int warned = 0;
			if (!warned++)
				eDebug("[histb-dec] software video frame (format %d), plane output skipped", f->format);
			return;
		}
		AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)f->data[0];
		m_drm.showFrame(desc, f->width, f->height);
	}

	ePtr<eDVBDemux> m_demux;
	int m_dev;
	HistbPesDemux m_pes;
	AVCodecContext *m_avctx = nullptr;
	AVPacket *m_pkt = nullptr;
	AVFrame *m_frame = nullptr;
	HistbDrmOutput m_drm;
	enum AVCodecID m_codec_type = AV_CODEC_ID_NONE;
	int m_pid = -1;
	bool m_synced = false;
	std::atomic<bool> m_frozen{false};
	int64_t m_last_pts = -1;
	int m_width = 0, m_height = 0, m_framerate = 0, m_aspect = 0, m_progressive = 0;
	std::function<void(const struct iTSMPEGDecoder::videoEvent &)> m_event_sink;
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
	m_histb = new HistbVideoPriv(demux, dev);
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

#endif /* __lib_dvb_decoder_histb_impl_h */

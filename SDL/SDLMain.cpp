
// Must come before any system header: fopencookie() is a GNU extension, and newlib's features.h
// derives __GNU_VISIBLE from _GNU_SOURCE (it overwrites a direct __GNU_VISIBLE define). Only this
// translation unit needs it, for the timestamped log stream.
#define _GNU_SOURCE 1

#include <cstdlib>
#include <unistd.h>
#include <pwd.h>

#include "ppsspp_config.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_mouse.h>
#include "SDL/SDLJoystick.h"
SDLJoystick *joystick = NULL;

#if PPSSPP_PLATFORM(RPI)
#include <bcm_host.h>
#endif

#include <atomic>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <thread>
#include <locale>
#if PPSSPP_PLATFORM(SWITCH)
#include <cstdio>
#endif

#if !PPSSPP_PLATFORM(SWITCH)
#include "ext/portable-file-dialogs/portable-file-dialogs.h"
#endif

#include "ext/imgui/imgui.h"
#include "ext/imgui/imgui_impl_platform.h"
#include "Common/System/Display.h"
#include "Common/System/System.h"
#include "Common/System/Request.h"
#include "Common/System/NativeApp.h"
#include "Common/GPU/thin3d.h"
#include "Common/Audio/AudioBackend.h"
#include "Core/CmdLine.h"
#include "Core/EmuThread.h"
#include "ext/glslang/glslang/Public/ShaderLang.h"
#include "Common/Data/Format/PNGLoad.h"
#include "Common/Net/Resolve.h"
#include "Common/File/FileUtil.h"
#include "Common/File/DirListing.h"
#include "Common/System/OSD.h"
#include "SDL/SDLMain.h"
#include "Common/SwitchAppRoot.h"
#include "NKCodeFromSDL.h"
#include "Common/Math/math_util.h"
#include "Common/GPU/OpenGL/GLRenderManager.h"
#include "Common/Profiler/Profiler.h"
#include "Common/Log/LogManager.h"

#if defined(VK_USE_PLATFORM_XLIB_KHR)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#elif defined(VK_USE_PLATFORM_XCB_KHR)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xlib-xcb.h>
#endif

#include "Common/GPU/GraphicsContext.h"
#include "Common/GPU/Vulkan/VulkanLoader.h"
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/GPU/Vulkan/VulkanGraphicsContext.h"
#include "Common/TimeUtil.h"
#include "Common/Input/InputState.h"
#include "Common/Input/KeyCodes.h"
#include "Common/Data/Collections/ConstMap.h"
#include "Common/Data/Encoding/Utf8.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/StringUtils.h"
#include "Core/HW/Camera.h"
#include "Core/System.h"
#include "Core/Core.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/HLE/sceUsbMic.h"
#include "SDLGLGraphicsContext.h"
#include "SDLUtil.h"

#include <SDL3/SDL_vulkan.h>

#if PPSSPP_PLATFORM(MAC) || PPSSPP_PLATFORM(IOS)
#include "Core/Util/DarwinFileSystemServices.h"
#include "SDL/SDLCocoaMetalLayer.h"
#include "Common/GPU/Metal/MetalGraphicsContext.h"
#include <SDL3/SDL_metal.h>
#endif

#if PPSSPP_PLATFORM(MAC)
#include "CocoaBarItems.h"
#endif

#if PPSSPP_PLATFORM(SWITCH)
#include <switch.h>
#include <switch/services/apm.h>
#include <switch/services/time.h>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <mutex>
#include <switch/services/pcv.h>
#include "Common/GPU/Vulkan/SwitchLSFG.h"
#include "UI/SwitchLibrary.h"
#include "UI/SwitchUpdate.h"

#define LIBNX_SWKBD_LIMIT 500 // enforced by HOS
extern "C" {
u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 0;
}

static ViDisplay g_switchDisplay;
static ViLayer g_switchLayer;
static NWindow g_switchWindow;
static AppletOperationMode g_switchOperationMode;

static void SwitchOutputSize(AppletOperationMode operationMode, u32 *width, u32 *height) {
	if (operationMode == AppletOperationMode_Handheld) {
		*width = 1280;
		*height = 720;
	} else {
		*width = 1920;
		*height = 1080;
	}
}

extern "C" NWindow *nwindowGetDefault() {
	return &g_switchWindow;
}

extern "C" void __nx_win_init() {
	Result rc = viInitialize(ViServiceType_Default);
	if (R_SUCCEEDED(rc)) {
		rc = viOpenDefaultDisplay(&g_switchDisplay);
		if (R_SUCCEEDED(rc)) {
			rc = viCreateLayer(&g_switchDisplay, &g_switchLayer);
			if (R_SUCCEEDED(rc)) {
				rc = viSetLayerScalingMode(&g_switchLayer, ViScalingMode_FitToLayer);
				if (R_SUCCEEDED(rc)) {
					s32 maxZ = 0;
					if (R_SUCCEEDED(viGetZOrderCountMax(&g_switchDisplay, &maxZ)) && maxZ > 0) {
						viSetLayerZ(&g_switchLayer, maxZ - 1);
					}
					rc = nwindowCreateFromLayer(&g_switchWindow, &g_switchLayer);
					if (R_SUCCEEDED(rc)) {
						u32 width;
						u32 height;
						g_switchOperationMode = appletGetOperationMode();
						SwitchOutputSize(g_switchOperationMode, &width, &height);
						rc = nwindowSetDimensions(&g_switchWindow, width, height);
					}
				}
				if (R_FAILED(rc)) {
					viCloseLayer(&g_switchLayer);
				}
			}
			if (R_FAILED(rc)) {
				viCloseDisplay(&g_switchDisplay);
			}
		}
		if (R_FAILED(rc)) {
			viExit();
		}
	}
	if (R_FAILED(rc)) {
		diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadGfxInit));
	}
}

#if PPSSPP_PLATFORM(SWITCH)
// The port never asked for a performance configuration, so the console stayed on whatever the system
// picked and the CPU sat around 1.7GHz while the emulator wanted more.
//
// The values below are not invented: libnx's apm.h documents 0x92220009 (docked) and 0x9222000A
// (handheld) for ApmCpuBoostMode_FastLoad - "boost the CPU, throttle the GPU to minimum" - and
// 0x00010000 is the baseline configuration stock titles report. We try them in that order and keep
// the first the kernel accepts, logging every rc. If the console refuses all of them we stay on the
// system default, which is exactly the situation before this change, so nothing can get worse.
// Values outside the official table (what sys-clk is for) are deliberately not attempted: the
// kernel rejects them.
static ApmPerformanceMode g_apmMode = ApmPerformanceMode_Invalid;
static u32 g_apmConfig = 0;

static void SwitchRequestMaxPerformance() {
	const Result initRc = apmInitialize();  // no-op when applet already initialised it
	if (R_FAILED(initRc)) {
		printf("[perf] apmInitialize rc=0x%x (continuing)\n", initRc);
	}

	// 0x92220009/0x0A = FastLoad boost (CPU 1785, GPU min) - some consoles/CFW reject them.
	// 0x00020000 = CPU 1785MHz / GPU 768MHz / MEM 1600MHz (official table).
	// 0x00010000 = CPU 1224MHz / GPU 384MHz / MEM 1600MHz (safe fallback).
	static const u32 candidates[] = { 0x92220009, 0x9222000A, 0x00020000, 0x00010000 };
	Result lastRc = initRc;
	for (size_t i = 0; i < ARRAY_SIZE(candidates) && !g_apmConfig; i++) {
		lastRc = apmSetPerformanceConfiguration(ApmPerformanceMode_Boost, candidates[i]);
		printf("[perf] apm boost config=0x%08x rc=0x%x\n", candidates[i], lastRc);
		if (R_SUCCEEDED(lastRc)) {
			g_apmMode = ApmPerformanceMode_Boost;
			g_apmConfig = candidates[i];
		}
	}
	if (!g_apmConfig) {
		// Boost is refused in some contexts (e.g. applet mode); the same table entries are accepted
		// in Normal mode there, so retry once before giving up.
		for (size_t i = 0; i < ARRAY_SIZE(candidates) && !g_apmConfig; i++) {
			lastRc = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, candidates[i]);
			printf("[perf] apm normal config=0x%08x rc=0x%x\n", candidates[i], lastRc);
			if (R_SUCCEEDED(lastRc)) {
				g_apmMode = ApmPerformanceMode_Normal;
				g_apmConfig = candidates[i];
			}
		}
	}

	// Read back what the console actually took, so one log line answers "did it stick".
	u32 appliedConfig = 0;
	if (g_apmMode != ApmPerformanceMode_Invalid) {
		apmGetPerformanceConfiguration(g_apmMode, &appliedConfig);
	}
	u32 cpuHz = 0;
	if (R_SUCCEEDED(pcvInitialize())) {
		pcvGetClockRate(PcvModule_CpuBus, &cpuHz);
		pcvExit();
	}
	printf("[perf] apm set rc=0x%x config=0x%08x mode=%d cpu=%u kHz\n",
		lastRc, appliedConfig, (int)g_apmMode, cpuHz / 1000);
}

static void SwitchReleasePerformance() {
	if (g_apmMode == ApmPerformanceMode_Invalid) {
		return;
	}
	const Result rc = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, 0);
	printf("[perf] apm restore rc=0x%x\n", rc);
	g_apmMode = ApmPerformanceMode_Invalid;
	g_apmConfig = 0;
	// Deliberately no apmExit(): libnx's appletExit() already closes apm when the applet is of type
	// Application, and closing a service twice during teardown is exactly the class of bug this port
	// has been fighting at exit. Leaving it open until process exit is harmless.
}
#endif  // PPSSPP_PLATFORM(SWITCH)

extern "C" void __nx_win_exit() {
	// libnx calls this from __appExit(), i.e. from exit() once main() has returned and the
	// C++ static destructors have run - it is the very last thing this process does with the
	// display. Every step is logged (with the Result of the calls that return one) because the
	// console's "the software was closed because an error occurred" box appears *after* this
	// window is gone: the log then tells us whether we got here at all, and which call failed.
	//
	// Double-close of the NWindow/layer is fatal, so guard it even though libnx (and nothing
	// else) is the only caller right now - a second teardown path would be easy to add by
	// mistake.
	static bool nxWinExitDone = false;
	if (nxWinExitDone) {
		printf("[exit] __nx_win_exit: already done, skipping\n");
		return;
	}
	nxWinExitDone = true;

	printf("[exit] __nx_win_exit: enter\n");
	SwitchReleasePerformance();
	nwindowClose(&g_switchWindow);
	printf("[exit] __nx_win_exit: nwindowClose done\n");
	const Result closeLayerResult = viCloseLayer(&g_switchLayer);
	printf("[exit] __nx_win_exit: viCloseLayer -> 0x%08x\n", closeLayerResult);
	const Result closeDisplayResult = viCloseDisplay(&g_switchDisplay);
	printf("[exit] __nx_win_exit: viCloseDisplay -> 0x%08x\n", closeDisplayResult);
	viExit();
	printf("[exit] __nx_win_exit: viExit done, handing over to libnx\n");
}

// Shutdown tracing. The "the software was closed because an error occurred" box shows up
// after our window is gone, which means the fault is in teardown that starts once main()
// returns and is otherwise invisible. Each stage below prints one line to
// sdmc:/ppsspp_log.txt (stdout is redirected and unbuffered there, see main()), so the last
// line in that log names the stage that hung or crashed.
static void SwitchExitStage(const char *stage) {
	printf("[exit] %s\n", stage);
}

// libnx's exception entry (libnx/source/runtime/arm/exception.c, object file exception.o) calls
// this weak hook with the register dump of the faulting thread. When the hook is not defined at
// all, libnx just aborts - and an aborted process is exactly what the console reports with
// "the software was closed because an error occurred". Defining it therefore costs nothing and
// turns that mystery into a log entry: we dump the registers, then return, and libnx aborts the
// process exactly like before (see __libnx_exception_returnentry, which svcBreak()s as soon as
// this returns).
//
// Written with snprintf + write() instead of fprintf: the faulting thread may have died while
// holding a stdio lock, and the exception runs on its own stack, so we avoid the locking stdio
// path entirely.
extern "C" void __libnx_exception_handler(ThreadExceptionDump *ctx) {
	char buf[512];
	int len;
	if (!ctx) {
		len = snprintf(buf, sizeof(buf), "[crash] unhandled exception without a register dump\n");
	} else {
		len = snprintf(buf, sizeof(buf),
			"[crash] unhandled exception: desc=0x%x esr=0x%x far=0x%llx\n"
			"[crash] pc=0x%llx lr=0x%llx sp=0x%llx fp=0x%llx pstate=0x%x\n"
			"[crash] x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx\n"
			"[crash] x4=0x%llx x5=0x%llx x6=0x%llx x7=0x%llx\n"
			"[crash] returning to libnx, which will abort the process\n",
			ctx->error_desc, ctx->esr, (unsigned long long)ctx->far.x,
			(unsigned long long)ctx->pc.x, (unsigned long long)ctx->lr.x,
			(unsigned long long)ctx->sp.x, (unsigned long long)ctx->fp.x, ctx->pstate,
			(unsigned long long)ctx->cpu_gprs[0].x, (unsigned long long)ctx->cpu_gprs[1].x,
			(unsigned long long)ctx->cpu_gprs[2].x, (unsigned long long)ctx->cpu_gprs[3].x,
			(unsigned long long)ctx->cpu_gprs[4].x, (unsigned long long)ctx->cpu_gprs[5].x,
			(unsigned long long)ctx->cpu_gprs[6].x, (unsigned long long)ctx->cpu_gprs[7].x);
	}
	if (len > 0) {
		const size_t toWrite = (size_t)len < sizeof(buf) ? (size_t)len : sizeof(buf) - 1;
		// Both stdout and stderr point at sdmc:/ppsspp_log.txt; fd 2 is used directly so no
		// FILE state is involved.
		const ssize_t written = write(STDERR_FILENO, buf, toWrite);
		(void)written;
	}
}

#define EXIT_TRACE(stage) SwitchExitStage(stage)
#else
#define EXIT_TRACE(stage) ((void)0)
#endif

GlobalUIState lastUIState = UISTATE_MENU;
GlobalUIState GetUIState();

// How long the cursor stays visible after the mouse stops moving, when auto-hiding it.
static constexpr double CURSOR_HIDE_DELAY = 0.5;
static double g_lastCursorMoveTime = 0.0;

static bool g_QuitRequested = false;
static bool g_RestartRequested = false;
#if PPSSPP_PLATFORM(SWITCH)
static std::string g_restartParams;
static std::string g_switchExecutablePath;
// Restart handoff lives in the data root (next to the NRO), not a fixed directory: the file has to
// survive a process restart, and the root is the one path we know stays ours.
static std::string SwitchRestartHandoffPath() {
	return SwitchAppRoot() + ".restart";
}

static bool SwitchIsAppletMode() {
	uint64_t isApplication = 1;
	if (R_SUCCEEDED(svcGetInfo(&isApplication, InfoType_IsApplication, CUR_PROCESS_HANDLE, 0))) {
		return isApplication == 0;
	}
	AppletType type = appletGetAppletType();
	return type != AppletType_Application && type != AppletType_SystemApplication;
}

static bool SwitchWriteRestartHandoff(std::string_view params) {
	FILE *file = fopen(SwitchRestartHandoffPath().c_str(), "wb");
	if (!file) {
		return false;
	}
	bool success = fwrite(params.data(), 1, params.size(), file) == params.size();
	success = fclose(file) == 0 && success;
	if (!success) {
		remove(SwitchRestartHandoffPath().c_str());
	}
	return success;
}

static std::string SwitchTakeRestartHandoff() {
	FILE *file = fopen(SwitchRestartHandoffPath().c_str(), "rb");
	if (!file) {
		return {};
	}
	char buffer[512]{};
	size_t size = fread(buffer, 1, sizeof(buffer) - 1, file);
	fclose(file);
	remove(SwitchRestartHandoffPath().c_str());
	return std::string(buffer, size);
}

static std::string SwitchNormalizeNroPath(std::string path) {
	if (path.rfind("sdmc:/", 0) == 0) {
		return path;
	}
	if (!path.empty() && path[0] == '/') {
		return "sdmc:" + path;
	}
	if (path.rfind("switch/", 0) == 0) {
		return "sdmc:/" + path;
	}
	return {};
}

// Reads one boolean out of ppsspp.ini without loading the whole config: this runs in main()
// before g_Config exists, and only needs to know whether file logging was switched on last time.
// Anything we cannot parse counts as "off" - the log file is opt-in, so failing closed is right.
static bool SwitchReadBoolSettingFromIni(const std::string &iniPath, const char *key) {
	FILE *file = fopen(iniPath.c_str(), "rb");
	if (!file) {
		return false;
	}
	const size_t keyLen = strlen(key);
	bool enabled = false;
	char line[512];
	while (fgets(line, sizeof(line), file)) {
		const char *p = line;
		while (*p == ' ' || *p == '\t') {
			++p;
		}
		// Case-insensitive prefix match on the key, then require "=".
		size_t i = 0;
		while (i < keyLen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)key[i])) {
			++i;
		}
		if (i != keyLen) {
			continue;
		}
		p += keyLen;
		while (*p == ' ' || *p == '\t') {
			++p;
		}
		if (*p != '=') {
			continue;
		}
		++p;
		while (*p == ' ' || *p == '\t') {
			++p;
		}
		enabled = !strncasecmp(p, "true", 4) || *p == '1' || !strncasecmp(p, "yes", 3);
		break;
	}
	fclose(file);
	return enabled;
}

static void CheckSwitchOperationMode() {
	AppletOperationMode operationMode = appletGetOperationMode();
	if (operationMode != g_switchOperationMode) {
		g_switchOperationMode = operationMode;
		g_restartParams.clear();
		g_RestartRequested = true;
	}
}
#endif

static int g_DesktopWidth = 0;
static int g_DesktopHeight = 0;
static float g_DesktopDPI = 1.0f;
static float g_ForcedDPI = 0.0f; // if this is 0.0f, use g_DesktopDPI
static float g_RefreshRate = 60.f;
#if PPSSPP_PLATFORM(SWITCH)
static int g_sampleRate = 48000;
static int g_nxlinkSocket = -1;
static bool g_socketInitialized = false;
#else
static int g_sampleRate = 44100;
#endif

static SDL_AudioSpec g_retFmt;
static int g_audioFramesPerBuffer = 0;

static bool g_textFocusChanged;
static bool g_textFocus;
// Set when SDL_StartTextInput fails, so we generate KeyInputFlags::CHAR from key events ourselves.
// SDL 3.4.0 to 3.4.4 refuse text input on Wayland compositors without zwp_text_input_v3 (like WSLg's Weston).
static bool g_textInputFallback;
static bool g_dropBootedFile = false;
double g_audioStartTime = 0.0;

// Window state to be transferred to the main SDL thread.
static std::mutex g_mutexWindow;
struct WindowState {
	std::string title;
	bool applyFullScreenNextFrame;
	bool clipboardDataAvailable;
	std::string clipboardString;
	bool update;
};
static WindowState g_windowState;

#if !PPSSPP_PLATFORM(MAC)
// -1 until polled, or without a battery.
static int g_batteryPercent = -1;
#endif

#if PPSSPP_PLATFORM(MAC)

// These are from MacCameraHelper.mm.
std::vector<std::string> __mac_getDeviceList();
int __mac_startCapture(int width, int height);
int __mac_stopCapture();

#endif

#if PPSSPP_PLATFORM(LINUX) && !PPSSPP_PLATFORM(ANDROID)

#include "Core/HLE/sceUsbCam.h"

#include "ext/jpge/jpgd.h"
#include "ext/jpge/jpge.h"

extern "C" {
#ifdef USE_FFMPEG
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
#endif //USE_FFMPEG
}

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "Common/Thread/ThreadUtil.h"

typedef struct {
	void         *start;
	int           length;
} v4l_buf_t;

static int        v4l_fd = -1;
static uint32_t   v4l_format;
static int        v4l_hw_width;
static int        v4l_hw_height;
static int        v4l_height_fixed_aspect;
static int        v4l_ideal_width;
static int        v4l_ideal_height;

static pthread_t  v4l_thread;
static int        v4l_buffer_count;
static v4l_buf_t *v4l_buffers;

std::vector<std::string> __v4l_getDeviceList();
int __v4l_startCapture(int width, int height);
int __v4l_stopCapture();


#ifdef USE_FFMPEG
void convert_frame(int inw, int inh, unsigned char *inData, AVPixelFormat inFormat,
					int outw, int outh, unsigned char **outData, int *outLen) {
	struct SwsContext *sws_context = sws_getContext(
				inw, inh, inFormat,
				outw, outh, AV_PIX_FMT_RGB24,
				SWS_BICUBIC, NULL, NULL, NULL);

	// resize
	uint8_t *src[4] = {0};
	uint8_t *dst[4] = {0};
	int srcStride[4], dstStride[4];

	unsigned char *rgbData = (unsigned char*)malloc(outw * outh * 4);

	av_image_fill_linesizes(srcStride, inFormat,         inw);
	av_image_fill_linesizes(dstStride, AV_PIX_FMT_RGB24, outw);

	av_image_fill_pointers(src, inFormat,         inh,  inData,  srcStride);
	av_image_fill_pointers(dst, AV_PIX_FMT_RGB24, outh, rgbData, dstStride);

	sws_scale(sws_context,
		src, srcStride, 0, inh,
		dst, dstStride);

	// compress jpeg
	*outLen = outw * outh * 2;
	*outData = (unsigned char*)malloc(*outLen);

	jpge::params params;
	params.m_quality = 60;
	params.m_subsampling = jpge::H2V2;
	params.m_two_pass_flag = false;
	jpge::compress_image_to_jpeg_file_in_memory(
		*outData, *outLen, outw, outh, 3, rgbData, params);
	free(rgbData);
}
#endif //USE_FFMPEG



#endif

#if PPSSPP_PLATFORM(LINUX) && !PPSSPP_PLATFORM(ANDROID)

std::vector<std::string> __v4l_getDeviceList() {
	std::vector<std::string> deviceList;
#ifdef USE_FFMPEG
	for (int i = 0; i < 64; i++) {
		char path[256];
		snprintf(path, sizeof(path), "/dev/video%d", i);
		if (access(path, F_OK) < 0) {
			break;
		}
		int fd = -1;
		if((fd = open(path, O_RDONLY)) < 0) {
			ERROR_LOG(Log::HLE, "Cannot open '%s'; errno=%d(%s)", path, errno, strerror(errno));
			continue;
		}
		struct v4l2_capability video_cap;
		if(ioctl(fd, VIDIOC_QUERYCAP, &video_cap) < 0) {
			ERROR_LOG(Log::HLE, "VIDIOC_QUERYCAP");
			goto cont;
		} else {
			char device[256];
			snprintf(device, sizeof(device), "%d:%s", i, video_cap.card);
			deviceList.push_back(device);
		}
cont:
		close(fd);
		fd = -1;
	}
#endif //USE_FFMPEG
	return deviceList;
}

void *v4l_loop(void *data) {
#ifdef USE_FFMPEG
	SetCurrentThreadName("v4l_loop");
	while (v4l_fd >= 0) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;

		if (ioctl(v4l_fd, VIDIOC_DQBUF, &buf) == -1) {
			ERROR_LOG(Log::HLE, "VIDIOC_DQBUF; errno=%d(%s)", errno, strerror(errno));
			switch (errno) {
			case EAGAIN:
				continue;
			default:
				return nullptr;
			}
		}

		unsigned char *jpegData = nullptr;
		int jpegLen = 0;

		if (v4l_format == V4L2_PIX_FMT_YUYV) {
			convert_frame(v4l_hw_width, v4l_hw_height, (unsigned char*)v4l_buffers[buf.index].start, AV_PIX_FMT_YUYV422,
				v4l_ideal_width, v4l_ideal_height, &jpegData, &jpegLen);
		} else if (v4l_format == V4L2_PIX_FMT_JPEG
				|| v4l_format == V4L2_PIX_FMT_MJPEG) {
			// decompress jpeg
			int width, height, req_comps;
			unsigned char *rgbData = jpgd::decompress_jpeg_image_from_memory(
				(unsigned char*)v4l_buffers[buf.index].start, buf.bytesused, &width, &height, &req_comps, 3);

			convert_frame(v4l_hw_width, v4l_hw_height, (unsigned char*)rgbData, AV_PIX_FMT_RGB24,
				v4l_ideal_width, v4l_ideal_height, &jpegData, &jpegLen);
			free(rgbData);
		}

		if (jpegData) {
			Camera::pushCameraImage(jpegLen, jpegData);
			free(jpegData);
			jpegData = nullptr;
		}

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		if (ioctl(v4l_fd, VIDIOC_QBUF, &buf) == -1) {
			ERROR_LOG(Log::HLE, "VIDIOC_QBUF");
			return nullptr;
		}
	}
#endif //USE_FFMPEG
	return nullptr;
}

int __v4l_startCapture(int ideal_width, int ideal_height) {
#ifdef USE_FFMPEG
	if (v4l_fd >= 0) {
		__v4l_stopCapture();
	}
	v4l_ideal_width  = ideal_width;
	v4l_ideal_height = ideal_height;

	int dev_index = 0;
	char dev_name[64];
	sscanf(g_Config.sCameraDevice.c_str(), "%d:", &dev_index);
	snprintf(dev_name, sizeof(dev_name), "/dev/video%d", dev_index);

	if ((v4l_fd = open(dev_name, O_RDWR)) == -1) {
		ERROR_LOG(Log::HLE, "Cannot open '%s'; errno=%d(%s)", dev_name, errno, strerror(errno));
		return -1;
	}

	struct v4l2_capability cap;
	memset(&cap, 0, sizeof(cap));
	if (ioctl(v4l_fd, VIDIOC_QUERYCAP, &cap) == -1) {
		ERROR_LOG(Log::HLE, "VIDIOC_QUERYCAP");
		return -1;
	}
	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
		ERROR_LOG(Log::HLE, "V4L2_CAP_VIDEO_CAPTURE");
		return -1;
	}
	if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
		ERROR_LOG(Log::HLE, "V4L2_CAP_STREAMING");
		return -1;
	}

	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof(fmt));
	fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.pixelformat = 0;

	// select a pixel format
	struct v4l2_fmtdesc desc;
	memset(&desc, 0, sizeof(desc));
	desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	while (ioctl(v4l_fd, VIDIOC_ENUM_FMT, &desc) == 0) {
		desc.index++;
		INFO_LOG(Log::HLE, "V4L2: pixel format supported: %s", desc.description);
		if (fmt.fmt.pix.pixelformat != 0) {
			continue;
		} else if (desc.pixelformat == V4L2_PIX_FMT_YUYV
				|| desc.pixelformat == V4L2_PIX_FMT_JPEG
				|| desc.pixelformat == V4L2_PIX_FMT_MJPEG) {
			INFO_LOG(Log::HLE, "V4L2: %s selected", desc.description);
			fmt.fmt.pix.pixelformat = desc.pixelformat;
			v4l_format              = desc.pixelformat;
		}
	}
	if (fmt.fmt.pix.pixelformat == 0) {
		ERROR_LOG(Log::HLE, "V4L2: No supported format found");
		return -1;
	}

	// select a frame size
	fmt.fmt.pix.width  = 0;
	fmt.fmt.pix.height = 0;
	struct v4l2_frmsizeenum frmsize;
	memset(&frmsize, 0, sizeof(frmsize));
	frmsize.pixel_format = fmt.fmt.pix.pixelformat;
	while (ioctl(v4l_fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) {
		frmsize.index++;
		if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
			INFO_LOG(Log::HLE, "V4L2: frame size supported: %dx%d", frmsize.discrete.width, frmsize.discrete.height);
			bool matchesIdeal = frmsize.discrete.width >= ideal_width && frmsize.discrete.height >= ideal_height;
			bool zeroPix = fmt.fmt.pix.width == 0 && fmt.fmt.pix.height == 0;
			bool pixLarger = frmsize.discrete.width < fmt.fmt.pix.width && frmsize.discrete.height < fmt.fmt.pix.height;
			if (matchesIdeal && (zeroPix || pixLarger)) {
				fmt.fmt.pix.width  = frmsize.discrete.width;
				fmt.fmt.pix.height = frmsize.discrete.height;
			}
		}
	}

	if (fmt.fmt.pix.width == 0 && fmt.fmt.pix.height == 0) {
		fmt.fmt.pix.width  = ideal_width;
		fmt.fmt.pix.height = ideal_height;
	}
	INFO_LOG(Log::HLE, "V4L2: asking for   %dx%d", fmt.fmt.pix.width, fmt.fmt.pix.height);
	if (ioctl(v4l_fd, VIDIOC_S_FMT, &fmt) == -1) {
		ERROR_LOG(Log::HLE, "VIDIOC_S_FMT");
		return -1;
	}
	v4l_hw_width  = fmt.fmt.pix.width;
	v4l_hw_height = fmt.fmt.pix.height;
	INFO_LOG(Log::HLE, "V4L2: will receive %dx%d", v4l_hw_width, v4l_hw_height);
	v4l_height_fixed_aspect = v4l_hw_width * ideal_height / ideal_width;
	INFO_LOG(Log::HLE, "V4L2: will use     %dx%d", v4l_hw_width, v4l_height_fixed_aspect);

	struct v4l2_requestbuffers req;
	memset(&req, 0, sizeof(req));
	req.count  = 1;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl(v4l_fd, VIDIOC_REQBUFS, &req) == -1) {
		ERROR_LOG(Log::HLE, "VIDIOC_REQBUFS");
		return -1;
	}
	v4l_buffer_count = req.count;
	INFO_LOG(Log::HLE, "V4L2: buffer count: %d", v4l_buffer_count);
	v4l_buffers = (v4l_buf_t*) calloc(v4l_buffer_count, sizeof(v4l_buf_t));

	for (int buf_id = 0; buf_id < v4l_buffer_count; buf_id++) {
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = buf_id;
		if (ioctl(v4l_fd, VIDIOC_QUERYBUF, &buf) == -1) {
			ERROR_LOG(Log::HLE, "VIDIOC_QUERYBUF");
			return -1;
		}

		v4l_buffers[buf_id].length = buf.length;
		v4l_buffers[buf_id].start = mmap(NULL,
				buf.length,
				PROT_READ | PROT_WRITE,
				MAP_SHARED,
				v4l_fd, buf.m.offset);
		if (v4l_buffers[buf_id].start == MAP_FAILED) {
			ERROR_LOG(Log::HLE, "MAP_FAILED");
			return -1;
		}

		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = buf_id;
		if (ioctl(v4l_fd, VIDIOC_QBUF, &buf) == -1) {
			ERROR_LOG(Log::HLE, "VIDIOC_QBUF");
			return -1;
		}
	}

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(v4l_fd, VIDIOC_STREAMON, &type) == -1) {
		ERROR_LOG(Log::HLE, "VIDIOC_STREAMON");
		return -1;
	}

	pthread_create(&v4l_thread, NULL, v4l_loop, NULL);
#endif //USE_FFMPEG
	return 0;
}

int __v4l_stopCapture() {
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (v4l_fd < 0) {
		goto exit;
	}

	if (ioctl(v4l_fd, VIDIOC_STREAMOFF, &type) == -1) {
		ERROR_LOG(Log::HLE, "VIDIOC_STREAMOFF");
		goto exit;
	}

	for (int buf_id = 0; buf_id < v4l_buffer_count; buf_id++) {
		if (munmap(v4l_buffers[buf_id].start, v4l_buffers[buf_id].length) == -1) {
			ERROR_LOG(Log::HLE, "munmap");
			goto exit;
		}
	}

	if (close(v4l_fd) == -1) {
		ERROR_LOG(Log::HLE, "close");
		goto exit;
	}

	v4l_fd = -1;
	//pthread_join(v4l_thread, NULL);

exit:
	v4l_fd = -1;
	return 0;
}

#endif // PPSSPP_PLATFORM(LINUX) && !PPSSPP_PLATFORM(ANDROID)

static int getDisplayNumber(void) {
	int displayNumber = 0;
	char * displayNumberStr;

	//get environment
	displayNumberStr=getenv("SDL_VIDEO_FULLSCREEN_HEAD");

	if (displayNumberStr) {
		displayNumber = atoi(displayNumberStr);
	}

	return displayNumber;
}

static void sdl_mixaudio_callback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
	thread_local bool affinitySet = false;
	if (!affinitySet) {
		SetCurrentThreadAffinity(ThreadAffinityRole::AUDIO);
		affinitySet = true;
	}
	(void)total_amount;
	if (additional_amount <= 0) {
		return;
	}

	const int frames = additional_amount / (int)(sizeof(int16_t) * 2);
	if (frames <= 0) {
		return;
	}

	thread_local std::vector<int16_t> mixBuf;
	mixBuf.resize(frames * 2);
	NativeMix(mixBuf.data(), frames, g_sampleRate, userdata);
	SDL_PutAudioStreamData(stream, mixBuf.data(), (int)(mixBuf.size() * sizeof(int16_t)));
}

static SDL_AudioDeviceID audioDev = 0;
static SDL_AudioStream *audioStream = nullptr;

static std::mutex micMutex;
static SDL_AudioStream *micStream = nullptr;
static SDL_AudioSpec micFormat{};
static std::atomic<bool> micDeviceChanged{false};
static std::atomic<SDL_AudioDeviceID> micDevice{0};
static std::atomic<SDL_AudioDeviceID> micPhysicalDevice{0};

static void sdl_mic_callback(void *userdata, SDL_AudioStream *stream, int, int) {
	const SDL_AudioSpec *format = static_cast<const SDL_AudioSpec *>(userdata);
	// Bound capture buffering while emulation is paused.
	if (SDL_GetAudioStreamAvailable(stream) > format->freq * format->channels * (int)sizeof(int16_t)) {
		SDL_ClearAudioStream(stream);
	}
}

// micMutex must be held. The callback only touches its SDL stream.
static void StopSDLMicrophone() {
	micDevice = 0;
	micPhysicalDevice = 0;
	if (micStream) {
		SDL_DestroyAudioStream(micStream);
		micStream = nullptr;
	}
}

static void StartSDLMicrophone(int sampleRate) {
	StopSDLMicrophone();
	Microphone::flushAudioData();
	micFormat.freq = sampleRate;
	micFormat.format = SDL_AUDIO_S16;
	micFormat.channels = 1;
	micDeviceChanged = false;

	int count = 0;
	SDL_AudioDeviceID *devices = SDL_GetAudioRecordingDevices(&count);
	SDL_AudioDeviceID selected = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
	for (int i = 0; i < count; i++) {
		const char *name = SDL_GetAudioDeviceName(devices[i]);
		if (name && g_Config.sMicDevice == name) {
			selected = devices[i];
			break;
		}
	}
	SDL_free(devices);

	micStream = SDL_OpenAudioDeviceStream(selected, &micFormat, sdl_mic_callback, &micFormat);
	if (!micStream && selected != SDL_AUDIO_DEVICE_DEFAULT_RECORDING) {
		WARN_LOG(Log::Audio, "Could not open microphone '%s': %s. Trying default.", g_Config.sMicDevice.c_str(), SDL_GetError());
		micStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &micFormat, sdl_mic_callback, &micFormat);
		selected = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
	}
	if (!micStream || !SDL_ResumeAudioStreamDevice(micStream)) {
		ERROR_LOG(Log::Audio, "Could not start microphone: %s", SDL_GetError());
		StopSDLMicrophone();
		return;
	}
	micDevice = SDL_GetAudioStreamDevice(micStream);
	micPhysicalDevice = selected;
	INFO_LOG(Log::Audio, "Microphone started at %d Hz", sampleRate);
}

static void PollSDLMicrophone() {
	if (micDeviceChanged.exchange(false)) {
		StartSDLMicrophone(micFormat.freq);
	}
	if (!micStream) {
		return;
	}

	// Called on the CPU thread; the SDL capture callback never accesses PSP memory.
	u8 samples[4096];
	int bytes;
	while ((bytes = SDL_GetAudioStreamData(micStream, samples, sizeof(samples))) > 0) {
		Microphone::addAudioData(samples, bytes);
	}
	if (bytes < 0) {
		ERROR_LOG(Log::Audio, "Could not read microphone: %s", SDL_GetError());
		StopSDLMicrophone();
	}
}

// Must be called after NativeInit().
static void InitSDLAudioDevice(const std::string &name = "") {
	SDL_AudioSpec fmt{};
	fmt.freq = g_sampleRate;
	fmt.format = SDL_AUDIO_S16;
	fmt.channels = 2;
#if PPSSPP_PLATFORM(SWITCH)
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "2048");
#endif
	g_audioFramesPerBuffer = std::max(g_Config.iSDLAudioBufferSize, 128);

	std::string startDevice = name;
	if (startDevice.empty()) {
		startDevice = g_Config.sAudioDevice;
	}

	int deviceCount = 0;
	SDL_AudioDeviceID *devices = SDL_GetAudioPlaybackDevices(&deviceCount);
	SDL_AudioDeviceID chosenDevice = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;

	// List available audio devices before trying to open, for debugging purposes.
	if (deviceCount > 0 && devices) {
		INFO_LOG(Log::Audio, "Available audio devices:");
		for (int i = 0; i < deviceCount; i++) {
			const char *deviceName = SDL_GetAudioDeviceName(devices[i]);
			if (!deviceName) {
				deviceName = "(unknown)";
			}
			INFO_LOG(Log::Audio, " * '%s'", deviceName);
			if (!startDevice.empty() && startDevice == deviceName) {
				chosenDevice = devices[i];
			}
		}
	} else {
		INFO_LOG(Log::Audio, "Failed to list audio devices: retval=%d", deviceCount);
	}

	if (!startDevice.empty() && chosenDevice == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) {
		WARN_LOG(Log::Audio, "Audio device '%s' not found, using default", startDevice.c_str());
	}

	if (audioStream) {
		SDL_DestroyAudioStream(audioStream);
		audioStream = nullptr;
	}

	audioDev = 0;
	if (chosenDevice == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) {
		INFO_LOG(Log::Audio, "Opening default audio device");
	} else {
		INFO_LOG(Log::Audio, "Opening audio device: '%s'", startDevice.c_str());
	}

	audioStream = SDL_OpenAudioDeviceStream(chosenDevice, &fmt, sdl_mixaudio_callback, nullptr);
	if (!audioStream && chosenDevice != SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) {
		WARN_LOG(Log::Audio, "SDL: Error opening '%s': '%s'. Trying default.", startDevice.c_str(), SDL_GetError());
		audioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &fmt, sdl_mixaudio_callback, nullptr);
	}

	if (!audioStream) {
		ERROR_LOG(Log::Audio, "Failed to open audio device '%s', second try. Giving up.", SDL_GetError());
	} else {
		audioDev = SDL_GetAudioStreamDevice(audioStream);
		if (!SDL_GetAudioDeviceFormat(audioDev, &g_retFmt, &g_audioFramesPerBuffer)) {
			WARN_LOG(Log::Audio, "Could not query active audio format: %s", SDL_GetError());
			g_retFmt = fmt;
			g_audioFramesPerBuffer = std::max(g_Config.iSDLAudioBufferSize, 128);
		}

		if (g_retFmt.freq != fmt.freq || g_retFmt.format != fmt.format || g_retFmt.channels != fmt.channels) {
			WARN_LOG(Log::Audio, "Audio output format differs from requested (freq=%d/%d format=%u/%u ch=%d/%d)", g_retFmt.freq, fmt.freq, (unsigned)g_retFmt.format, (unsigned)fmt.format, g_retFmt.channels, fmt.channels);
		}

		if (!SDL_ResumeAudioStreamDevice(audioStream)) {
			ERROR_LOG(Log::Audio, "Failed to start audio stream: %s", SDL_GetError());
			SDL_DestroyAudioStream(audioStream);
			audioStream = nullptr;
			audioDev = 0;
		}
	}

	if (devices) {
		SDL_free(devices);
	}
}

static void StopSDLAudioDevice() {
	if (audioStream) {
		SDL_DestroyAudioStream(audioStream);
		audioStream = nullptr;
	}
	audioDev = 0;
}

static void UpdateScreenDPI(SDL_Window *window) {
	// SDL3's window display scale already accounts for the display's content
	// scale and the window's pixel density, so we don't need to (incorrectly)
	// derive it ourselves from the ratio of pixel size to window size.
	float scale = SDL_GetWindowDisplayScale(window);
	if (scale <= 0.0f) {
		WARN_LOG(Log::System, "SDL_GetWindowDisplayScale failed: %s", SDL_GetError());
		scale = 1.0f;
	}
	g_DesktopDPI = scale;
}

// Simple implementations of System functions

void System_Toast(std::string_view text) {
#ifdef _WIN32
	std::wstring str = ConvertUTF8ToWString(text);
	MessageBox(0, str.c_str(), L"Toast!", MB_ICONINFORMATION);
#else
	fprintf(stderr, "%*.s", (int)text.length(), text.data());
#endif
}

void System_ShowKeyboard() {
	// Irrelevant on PC
}

void System_Vibrate(int length_ms) {
	// Ignore on PC
}

AudioBackend *System_CreateAudioBackend() {
	// Use legacy mechanisms.
	return nullptr;
}

static void InitializeFilters(std::vector<std::string> &filters, BrowseFileType type) {
	switch (type) {
	case BrowseFileType::BOOTABLE:
		filters.push_back("All supported file types (*.iso *.cso *.chd *.pbp *.elf *.prx *.zip *.ppdmp *.cue *.ccd *.mds *.mdf *.bin *.img)");
		filters.push_back("*.pbp *.elf *.iso *.cso *.chd *.prx *.zip *.ppdmp *.cue *.ccd *.mds *.mdf *.bin *.img");
		break;
	case BrowseFileType::SAVE_STATE:
		filters.push_back("Save state files (*.ppst)");
		filters.push_back("*.ppst");
		break;
	case BrowseFileType::INI:
		filters.push_back("Ini files");
		filters.push_back("*.ini");
		break;
	case BrowseFileType::ZIP:
		filters.push_back("ZIP files");
		filters.push_back("*.zip");
		break;
	case BrowseFileType::DB:
		filters.push_back("Cheat db files");
		filters.push_back("*.db");
		break;
	case BrowseFileType::SOUND_EFFECT:
		filters.push_back("Sound effect files (wav, mp3)");
		filters.push_back("*.wav *.mp3");
		break;
	case BrowseFileType::SYMBOL_MAP:
		filters.push_back("PPSSPP Symbol Map files (ppmap)");
		filters.push_back("*.ppmap");
		break;
	case BrowseFileType::SYMBOL_MAP_NOCASH:
		filters.push_back("No$ symbol Map files (sym)");
		filters.push_back("*.sym");
		break;
	case BrowseFileType::ATRAC3:
		filters.push_back("Atrac3 files (at3)");
		filters.push_back("*.at3");
		break;
	case BrowseFileType::IMAGE:
		filters.push_back("Pictures (jpg, png)");
		filters.push_back("*.jpg *.png");
		break;
	case BrowseFileType::ANY:
		break;
	}
	filters.push_back("All files (*.*)");
	filters.push_back("*");
}

#if PPSSPP_PLATFORM(LINUX) && !PPSSPP_PLATFORM(ANDROID)
// Opens a file or folder in whatever the desktop associates with it, without blocking the caller.
static void LaunchXdgOpen(const std::string &path) {
	pid_t pid = fork();
	if (pid == 0) {
		execlp("xdg-open", "xdg-open", path.c_str(), nullptr);
		_exit(1);
	}
}
#endif

bool System_MakeRequest(SystemRequestType type, int requestId, const std::string &param1, const std::string &param2, int64_t param3, int64_t param4) {
	switch (type) {
	case SystemRequestType::RESTART_APP:
		g_RestartRequested = true;
#if PPSSPP_PLATFORM(SWITCH)
		g_restartParams = param1;
#endif
		// TODO: Also save param1 and then split it into an argv.
		return true;
	case SystemRequestType::EXIT_APP:
		// Do a clean exit
		g_QuitRequested = true;
		return true;
#if PPSSPP_PLATFORM(SWITCH)
	case SystemRequestType::INPUT_TEXT_MODAL:
	{
		// swkbd only works on "real" titles
		if (__nx_applet_type != AppletType_Application && __nx_applet_type != AppletType_SystemApplication) {
			g_requestManager.PostSystemFailure(requestId);
			return true;
		}

		SwkbdConfig kbd;
		Result rc = swkbdCreate(&kbd, 0);

		if (R_SUCCEEDED(rc)) {
			char buf[LIBNX_SWKBD_LIMIT] = {'\0'};
			swkbdConfigMakePresetDefault(&kbd);
			swkbdConfigSetType(&kbd, SwkbdType_All);

			swkbdConfigSetHeaderText(&kbd, param1.c_str());
			swkbdConfigSetInitialText(&kbd, param2.c_str());

			rc = swkbdShow(&kbd, buf, sizeof(buf));

			swkbdClose(&kbd);

			if (R_SUCCEEDED(rc)) {
				g_requestManager.PostSystemSuccess(requestId, buf);
			} else {
				g_requestManager.PostSystemFailure(requestId);
			}
			return true;
		}

		g_requestManager.PostSystemFailure(requestId);
		return true;
	}
#endif // PPSSPP_PLATFORM(SWITCH)
#if PPSSPP_PLATFORM(MAC) || PPSSPP_PLATFORM(IOS)
	case SystemRequestType::BROWSE_FOR_FILE:
	{
		DarwinDirectoryPanelCallback callback = [requestId] (bool success, Path path) {
			if (success) {
				g_requestManager.PostSystemSuccess(requestId, path.c_str());
			} else {
				g_requestManager.PostSystemFailure(requestId);
			}
		};
		BrowseFileType fileType = (BrowseFileType)param3;
		DarwinFileSystemServices::presentDirectoryPanel(callback, /* allowFiles = */ true, /* allowDirectories = */ false, fileType);
		return true;
	}
	case SystemRequestType::BROWSE_FOR_IMAGE:
	{
		DarwinDirectoryPanelCallback callback = [requestId] (bool success, Path path) {
			if (success) {
				g_requestManager.PostSystemSuccess(requestId, path.c_str());
			} else {
				g_requestManager.PostSystemFailure(requestId);
			}
		};
		BrowseFileType fileType = BrowseFileType::IMAGE;
		DarwinFileSystemServices::presentDirectoryPanel(callback, /* allowFiles = */ true, /* allowDirectories = */ false, fileType);
		return true;
	}
	case SystemRequestType::BROWSE_FOR_FOLDER:
	{
		DarwinDirectoryPanelCallback callback = [requestId] (bool success, Path path) {
			if (success) {
				g_requestManager.PostSystemSuccess(requestId, path.c_str());
			} else {
				g_requestManager.PostSystemFailure(requestId);
			}
		};
		DarwinFileSystemServices::presentDirectoryPanel(callback, /* allowFiles = */ false, /* allowDirectories = */ true);
		return true;
	}
#elif !PPSSPP_PLATFORM(SWITCH)
	case SystemRequestType::BROWSE_FOR_IMAGE:
	{
		// TODO: Add non-blocking support.
		const std::string &title = param1;
		std::vector<std::string> filters;
		InitializeFilters(filters, BrowseFileType::IMAGE);
		std::vector<std::string> result = pfd::open_file(title, "", filters).result();
		if (!result.empty()) {
			g_requestManager.PostSystemSuccess(requestId, result[0]);
		} else {
			g_requestManager.PostSystemFailure(requestId);
		}
		return true;
	}
	case SystemRequestType::BROWSE_FOR_FILE:
	case SystemRequestType::BROWSE_FOR_FILE_SAVE:
	{
		// TODO: Add non-blocking support.
		const BrowseFileType browseType = (BrowseFileType)param3;
		std::string initialFilename = param2;
		const std::string &title = param1;
		std::vector<std::string> filters;
		InitializeFilters(filters, browseType);
		if (type == SystemRequestType::BROWSE_FOR_FILE) {
			std::vector<std::string> result = pfd::open_file(title, initialFilename, filters).result();
			if (!result.empty()) {
				g_requestManager.PostSystemSuccess(requestId, result[0]);
			} else {
				g_requestManager.PostSystemFailure(requestId);
			}
		} else {
			std::string result = pfd::save_file(title, initialFilename, filters).result();
			if (!result.empty()) {
				g_requestManager.PostSystemSuccess(requestId, result);
			} else {
				g_requestManager.PostSystemFailure(requestId);
			}
		}
		return true;
	}
	case SystemRequestType::BROWSE_FOR_FOLDER:
	{
		// TODO: Add non-blocking support.
		std::string result = pfd::select_folder(param1, param2).result();
		if (!result.empty()) {
			g_requestManager.PostSystemSuccess(requestId, result);
		} else {
			g_requestManager.PostSystemFailure(requestId);
		}
		return true;
	}
#endif
	case SystemRequestType::APPLY_FULLSCREEN_STATE:
	{
		std::lock_guard<std::mutex> guard(g_mutexWindow);
		g_windowState.update = true;
		g_windowState.applyFullScreenNextFrame = true;
		return true;
	}
	case SystemRequestType::SET_WINDOW_TITLE:
	{
		std::lock_guard<std::mutex> guard(g_mutexWindow);
		const char *app_name = System_GetPropertyBool(SYSPROP_APP_GOLD) ? "PPSSPP Gold" : "PPSSPP";
		g_windowState.title = param1.empty() ? app_name : param1;
		g_windowState.update = true;
		return true;
	}
	case SystemRequestType::COPY_TO_CLIPBOARD:
	{
		std::lock_guard<std::mutex> guard(g_mutexWindow);
		g_windowState.clipboardString = param1;
		g_windowState.clipboardDataAvailable = true;
		g_windowState.update = true;
		return true;
	}
	case SystemRequestType::SHOW_FILE_IN_FOLDER:
	{
#if PPSSPP_PLATFORM(WINDOWS)
		SFGAOF flags;
		PIDLIST_ABSOLUTE pidl = nullptr;
		HRESULT hr = SHParseDisplayName(ConvertUTF8ToWString(ReplaceAll(path, "/", "\\")).c_str(), nullptr, &pidl, 0, &flags);
		if (pidl) {
			if (SUCCEEDED(hr))
				SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
			CoTaskMemFree(pidl);
		}
#elif PPSSPP_PLATFORM(MAC)
		OSXShowInFinder(param1.c_str());
#elif (PPSSPP_PLATFORM(LINUX) && !PPSSPP_PLATFORM(ANDROID))
		LaunchXdgOpen(param1);
#endif /* PPSSPP_PLATFORM(WINDOWS) */
		return true;
	}
	case SystemRequestType::CAMERA_COMMAND:
	{
		if (!strncmp(param1.c_str(), "startVideo", 10)) {
			int width = 0, height = 0;
			sscanf(param1.c_str(), "startVideo_%dx%d", &width, &height);
#if PPSSPP_PLATFORM(MAC)
			__mac_startCapture(width, height);
#elif PPSSPP_PLATFORM(LINUX) && !PPSSPP_PLATFORM(ANDROID)
			__v4l_startCapture(width, height);
#endif
		} else if (!strcmp(param1.c_str(), "stopVideo")) {
#if PPSSPP_PLATFORM(MAC)
			__mac_stopCapture();
#elif PPSSPP_PLATFORM(LINUX) && !PPSSPP_PLATFORM(ANDROID)
			__v4l_stopCapture();
#endif
		} else {
			ERROR_LOG(Log::System, "Unknown camera command: %s", param1.c_str());
			return false;
		}
		return true;
	}
	case SystemRequestType::MICROPHONE_COMMAND:
	{
		std::lock_guard<std::mutex> guard(micMutex);
		if (startsWith(param1, "startRecording:")) {
			int sampleRate = 0;
			if (sscanf(param1.c_str(), "startRecording:%d", &sampleRate) != 1 || sampleRate <= 0) {
				return false;
			}
			StartSDLMicrophone(sampleRate);
		} else if (param1 == "stopRecording") {
			StopSDLMicrophone();
		} else if (param1 == "pollRecording") {
			PollSDLMicrophone();
		} else if (param1 == "deviceChanged") {
			micDeviceChanged = true;
		} else {
			return false;
		}
		return true;
	}
	case SystemRequestType::NOTIFY_UI_EVENT:
	{
		switch ((UIEventNotification)param3) {
		case UIEventNotification::TEXT_GOTFOCUS:
			g_textFocus = true;
			g_textFocusChanged = true;
			break;
		case UIEventNotification::TEXT_LOSTFOCUS:
			g_textFocus = false;
			g_textFocusChanged = true;
			break;
		default:
			break;
		}
		return true;
	}
	case SystemRequestType::SET_KEEP_SCREEN_BRIGHT:
		VERBOSE_LOG(Log::UI, "SET_KEEP_SCREEN_BRIGHT not implemented.");
		return true;
	default:
		INFO_LOG(Log::UI, "Unhandled system request %s", RequestTypeAsString(type));
		return false;
	}
}

void System_AskForPermission(SystemPermission permission) {}
PermissionStatus System_GetPermissionStatus(SystemPermission permission) { return PERMISSION_STATUS_GRANTED; }

std::vector<std::string> System_GetCameraDeviceList() {
#if PPSSPP_PLATFORM(MAC)
	return __mac_getDeviceList();
#elif PPSSPP_PLATFORM(LINUX) && !PPSSPP_PLATFORM(ANDROID)
	return __v4l_getDeviceList();
#else
	return {};
#endif
}

void System_LaunchUrl(LaunchUrlType urlType, std::string_view url) {
	switch (urlType) {
	case LaunchUrlType::BROWSER_URL:
	{
#if PPSSPP_PLATFORM(SWITCH)
		Uuid uuid = { 0 };
		WebWifiConfig conf;
		webWifiCreate(&conf, NULL, std::string(url).c_str(), uuid, 0);
		webWifiShow(&conf, NULL);
#elif defined(MOBILE_DEVICE)
		INFO_LOG(Log::System, "Would have gone to %.*s but LaunchBrowser is not implemented on this platform", STR_VIEW(url));
#elif defined(_WIN32)
		std::wstring wurl = ConvertUTF8ToWString(url);
		ShellExecute(NULL, L"open", wurl.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
		OSXOpenURL(std::string(url).c_str());
#elif PPSSPP_PLATFORM(LINUX)
		// Not through a shell: the URL can come from the game.
		LaunchXdgOpen(std::string(url));
#else
		std::string command = "xdg-open '" + ReplaceAll(url, "'", "'\\''") + "'";
		int err = system(command.c_str());
		if (err) {
			INFO_LOG(Log::System, "Would have gone to %.*s but xdg-utils seems not to be installed", STR_VIEW(url));
		}
#endif
		break;
	}
	case LaunchUrlType::EMAIL_ADDRESS:
	{
#if defined(MOBILE_DEVICE)
		INFO_LOG(Log::System, "Would have opened your email client for %.*s but LaunchEmail is not implemented on this platform", STR_VIEW(url));
#elif defined(_WIN32)
		std::wstring mailto = std::wstring(L"mailto:") + ConvertUTF8ToWString(url);
		ShellExecute(NULL, L"open", mailto.c_str(), NULL, NULL, SW_SHOWNORMAL);
#elif defined(__APPLE__)
		OSXOpenURL(join("mailto:", url).c_str());
#else
		int err = system(join("xdg-email ", url).c_str());
		if (err) {
			INFO_LOG(Log::System, "Would have gone to %.*s but xdg-utils seems not to be installed", STR_VIEW(url));
		}
#endif
		break;
	}
	case LaunchUrlType::LOCAL_FILE:
	case LaunchUrlType::LOCAL_FOLDER:
#if defined(__APPLE__)
		// If it's a folder and we're on a mac, open it in finder.
		OSXShowInFinder(std::string(url).c_str());
#elif PPSSPP_PLATFORM(LINUX) && !PPSSPP_PLATFORM(ANDROID)
		LaunchXdgOpen(std::string(url));
#endif
		// INFO_LOG(Log::System, "LaunchUrlType::LOCAL_FILE not implemented on this platform");
		break;
	default:
		INFO_LOG(Log::System, "Unhandled LaunchUrlType %d", (int)urlType);
		break;
	}
}

std::string System_GetProperty(SystemProperty prop) {
	switch (prop) {
#if PPSSPP_PLATFORM(MAC)
	case SYSPROP_DEVICE_MODEL:
		return "Mac";
#endif
	case SYSPROP_NAME:
#ifdef _WIN32
		return "SDL:Windows";
#elif __linux__
		return "SDL:Linux";
#elif __APPLE__
		return "SDL:macOS";
#elif PPSSPP_PLATFORM(SWITCH)
		return "SDL:Horizon";
#else
		return "SDL:";
#endif
	case SYSPROP_LANGREGION: {
#if PPSSPP_PLATFORM(SWITCH)
		uint64_t languageCode = 0;
		SetLanguage language = SetLanguage_ENUS;
		if (R_SUCCEEDED(setInitialize())) {
			Result rc = setGetSystemLanguage(&languageCode);
			if (R_SUCCEEDED(rc)) {
				rc = setMakeLanguage(languageCode, &language);
			}
			setExit();
			if (R_FAILED(rc)) {
				language = SetLanguage_ENUS;
			}
		}
		switch (language) {
		case SetLanguage_JA: return "ja_JP";
		case SetLanguage_FR:
		case SetLanguage_FRCA: return "fr_FR";
		case SetLanguage_DE: return "de_DE";
		case SetLanguage_IT: return "it_IT";
		case SetLanguage_ES: return "es_ES";
		case SetLanguage_ZHCN:
		case SetLanguage_ZHHANS: return "zh_CN";
		case SetLanguage_KO: return "ko_KR";
		case SetLanguage_NL: return "nl_NL";
		case SetLanguage_PT: return "pt_PT";
		case SetLanguage_RU: return "ru_RU";
		case SetLanguage_ZHTW:
		case SetLanguage_ZHHANT: return "zh_TW";
		case SetLanguage_ES419: return "es_LA";
		case SetLanguage_PTBR: return "pt_BR";
		case SetLanguage_ENUS:
		case SetLanguage_ENGB:
		default: return "en_US";
		}
#else
		// Get user-preferred locale from OS
		setlocale(LC_ALL, "");
		std::string locale(setlocale(LC_ALL, NULL));
		// Set c and c++ strings back to POSIX
		std::locale::global(std::locale("POSIX"));
		if (!locale.empty()) {
			// Technically, this is an opaque string, but try to find the locale code.
			size_t messagesPos = locale.find("LC_MESSAGES=");
			if (messagesPos != std::string::npos) {
				messagesPos += strlen("LC_MESSAGES=");
				size_t semi = locale.find(';', messagesPos);
				locale = locale.substr(messagesPos, semi - messagesPos);
			}

			if (locale.find("_", 0) != std::string::npos) {
				if (locale.find(".", 0) != std::string::npos) {
					return locale.substr(0, locale.find(".",0));
				}
				return locale;
			}
		}
		return "en_US";
#endif
	}
	case SYSPROP_CLIPBOARD_TEXT:
		return SDL_HasClipboardText() ? SDL_GetClipboardText() : "";
	case SYSPROP_AUDIO_DEVICE_LIST:
		{
			std::string result;
			int count = 0;
			SDL_AudioDeviceID *devices = SDL_GetAudioPlaybackDevices(&count);
			for (int i = 0; devices && i < count; ++i) {
				const char *name = SDL_GetAudioDeviceName(devices[i]);
				if (!name) {
					continue;
				}

				if (i == 0) {
					result = name;
				} else {
					result.append(1, '\0');
					result.append(name);
				}
			}
			if (devices) {
				SDL_free(devices);
			}
			return result;
		}
	case SYSPROP_BUILD_VERSION:
		return PPSSPP_GIT_VERSION;
	case SYSPROP_USER_DOCUMENTS_DIR:
	{
		const char *home = getenv("HOME");
		return home ? std::string(home) : "/";
	}
	default:
		return "";
	}
}

std::vector<std::string> System_GetPropertyStringVec(SystemProperty prop) {
	std::vector<std::string> result;

	switch (prop) {
	case SYSPROP_MICROPHONE_DEVICE_LIST:
	{
		int count = 0;
		SDL_AudioDeviceID *devices = SDL_GetAudioRecordingDevices(&count);
		for (int i = 0; i < count; i++) {
			const char *name = SDL_GetAudioDeviceName(devices[i]);
			if (name) {
				result.emplace_back(name);
			}
		}
		SDL_free(devices);
		return result;
	}
	case SYSPROP_TEMP_DIRS:
		if (getenv("TMPDIR") && strlen(getenv("TMPDIR")) != 0)
			result.push_back(getenv("TMPDIR"));
		if (getenv("TMP") && strlen(getenv("TMP")) != 0)
			result.push_back(getenv("TMP"));
		if (getenv("TEMP") && strlen(getenv("TEMP")) != 0)
			result.push_back(getenv("TEMP"));
		return result;

	default:
		return result;
	}
}

#if PPSSPP_PLATFORM(MAC)
extern "C" {
int Apple_GetCurrentBatteryCapacity();
}
#endif

int64_t System_GetPropertyInt(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_AUDIO_SAMPLE_RATE:
		return g_retFmt.freq;
	case SYSPROP_AUDIO_FRAMES_PER_BUFFER:
		return g_audioFramesPerBuffer;
	case SYSPROP_DEVICE_TYPE:
#if defined(MOBILE_DEVICE)
		return DEVICE_TYPE_MOBILE;
#else
		return DEVICE_TYPE_DESKTOP;
#endif
	case SYSPROP_DISPLAY_COUNT:
		{
			int displayCount = 0;
			SDL_DisplayID *displays = SDL_GetDisplays(&displayCount);
			if (displays) {
				SDL_free(displays);
			}
			return displayCount;
		}
	case SYSPROP_KEYBOARD_LAYOUT:
	{
		char q, w, y;
		q = SDL_GetKeyFromScancode(SDL_SCANCODE_Q, SDL_KMOD_NONE, false);
		w = SDL_GetKeyFromScancode(SDL_SCANCODE_W, SDL_KMOD_NONE, false);
		y = SDL_GetKeyFromScancode(SDL_SCANCODE_Y, SDL_KMOD_NONE, false);
		if (q == 'a' && w == 'z' && y == 'y')
			return KEYBOARD_LAYOUT_AZERTY;
		else if (q == 'q' && w == 'w' && y == 'z')
			return KEYBOARD_LAYOUT_QWERTZ;
		return KEYBOARD_LAYOUT_QWERTY;
	}
	case SYSPROP_DISPLAY_XRES:
		return g_DesktopWidth;
	case SYSPROP_DISPLAY_YRES:
		return g_DesktopHeight;
	case SYSPROP_BATTERY_PERCENTAGE:
#if PPSSPP_PLATFORM(MAC)
	// Let's keep using the old code on Mac for safety. Evaluate later if to be deleted.
		return Apple_GetCurrentBatteryCapacity();
#else
		return g_batteryPercent;
#endif
	default:
		return -1;
	}
}

float System_GetPropertyFloat(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_DISPLAY_REFRESH_RATE:
		return g_RefreshRate;
	case SYSPROP_DISPLAY_DPI:
		return (g_ForcedDPI == 0.0f ? g_DesktopDPI : g_ForcedDPI) * 96.0;
	case SYSPROP_DISPLAY_SAFE_INSET_LEFT:
	case SYSPROP_DISPLAY_SAFE_INSET_RIGHT:
	case SYSPROP_DISPLAY_SAFE_INSET_TOP:
	case SYSPROP_DISPLAY_SAFE_INSET_BOTTOM:
		return 0.0f;
	default:
		return -1;
	}
}

bool System_GetPropertyBool(SystemProperty prop) {
	switch (prop) {
	case SYSPROP_MICROPHONE_NEEDS_POLLING:
		return true;
	case SYSPROP_HAS_TEXT_CLIPBOARD:
	case SYSPROP_CAN_SHOW_FILE:
#if PPSSPP_PLATFORM(WINDOWS) || PPSSPP_PLATFORM(MAC) || (PPSSPP_PLATFORM(LINUX) && !PPSSPP_PLATFORM(ANDROID))
		return true;
#else
		return false;
#endif
	case SYSPROP_CAN_LAUNCH_URL:
#if defined(MOBILE_DEVICE) && !PPSSPP_PLATFORM(SWITCH)
		return false;
#else
		return true;
#endif
	case SYSPROP_HAS_OPEN_DIRECTORY:
#if PPSSPP_PLATFORM(WINDOWS)
		return true;
#elif PPSSPP_PLATFORM(MAC) || (PPSSPP_PLATFORM(LINUX) && !PPSSPP_PLATFORM(ANDROID))
		return true;
#endif
	case SYSPROP_HAS_BACK_BUTTON:
		return true;
#if PPSSPP_PLATFORM(SWITCH)
	case SYSPROP_HAS_TEXT_INPUT_DIALOG:
		return __nx_applet_type == AppletType_Application || __nx_applet_type == AppletType_SystemApplication;
	case SYSPROP_KEYBOARD_IS_SOFT:
		return true;
	case SYSPROP_APPLET_MODE:
		return SwitchIsAppletMode();
#endif
	case SYSPROP_HAS_KEYBOARD:
		return true;
	case SYSPROP_APP_GOLD:
#ifdef GOLD
		return true;
#else
		return false;
#endif
	case SYSPROP_CAN_JIT:
		return true;
	case SYSPROP_SUPPORTS_OPEN_FILE_IN_EDITOR:
		return true;  // FileUtil.cpp: OpenFileInEditor
#ifndef HTTPS_NOT_AVAILABLE
	case SYSPROP_SUPPORTS_HTTPS:
		// On Linux this also depends on whether libcurl could be loaded.
		return !g_Config.bDisableHTTPS && net::HTTPSAvailable();
#endif
case SYSPROP_HAS_FOLDER_BROWSER:
case SYSPROP_HAS_FILE_BROWSER:
#if PPSSPP_PLATFORM(MAC)
		return true;
#elif PPSSPP_PLATFORM(SWITCH)
		return false;
#else
		return pfd::settings::available();
#endif
	case SYSPROP_HAS_ACCELEROMETER:
#if defined(MOBILE_DEVICE)
		return true;
#else
		return false;
#endif
	case SYSPROP_CAN_READ_BATTERY_PERCENTAGE:
		return true;
	case SYSPROP_CAN_GET_FREE_SPACE_FAST:
		return true;
	case SYSPROP_ENOUGH_RAM_FOR_FULL_ISO:
#if PPSSPP_ARCH(64BIT) && !defined(MOBILE_DEVICE)
		return true;
#else
		return false;
#endif
	// hack for testing - do not commit
	case SYSPROP_USE_IAP:
		return false;
	default:
		return false;
	}
}

void System_Notify(SystemNotification notification) {
	switch (notification) {
	case SystemNotification::AUDIO_RESET_DEVICE:
		StopSDLAudioDevice();
		InitSDLAudioDevice();
		break;

	default:
		break;
	}
}

void UpdateWindowState(SDL_Window *window) {
	SDL_SetWindowTitle(window, g_windowState.title.c_str());
	if (g_windowState.applyFullScreenNextFrame) {
		g_windowState.applyFullScreenNextFrame = false;
		SDL_SetWindowFullscreen(window, g_Config.bFullScreen);
	}
	if (g_windowState.clipboardDataAvailable) {
		SDL_SetClipboardText(g_windowState.clipboardString.c_str());
		g_windowState.clipboardDataAvailable = false;
		g_windowState.clipboardString.clear();
	}
	g_windowState.update = false;
}

struct InputStateTracker {
	void MouseCaptureControl(SDL_Window *window) {
		bool captureMouseCondition = g_Config.bMouseControl && ((GetUIState() == UISTATE_INGAME && g_Config.bMouseConfine) || g_IsMappingMouseInput);
		if (mouseCaptured != captureMouseCondition) {
			mouseCaptured = captureMouseCondition;
			SDL_SetWindowRelativeMouseMode(window, captureMouseCondition);
		}
	}

	int mouseDown;  // bitflags
	bool mouseCaptured;
	TouchMapper touchMap;
};

SDL_Cursor *g_builtinCursors[SDL_SYSTEM_CURSOR_COUNT];

static SDL_SystemCursor GetSDLCursorFromImgui(ImGuiMouseCursor cursor) {
	switch (cursor) {
	case ImGuiMouseCursor_Arrow:        return SDL_SYSTEM_CURSOR_DEFAULT; break;
	case ImGuiMouseCursor_TextInput:    return SDL_SYSTEM_CURSOR_TEXT; break;
	case ImGuiMouseCursor_ResizeAll:    return SDL_SYSTEM_CURSOR_MOVE; break;
	case ImGuiMouseCursor_ResizeEW:     return SDL_SYSTEM_CURSOR_EW_RESIZE; break;
	case ImGuiMouseCursor_ResizeNS:     return SDL_SYSTEM_CURSOR_NS_RESIZE; break;
	case ImGuiMouseCursor_ResizeNESW:   return SDL_SYSTEM_CURSOR_NESW_RESIZE; break;
	case ImGuiMouseCursor_ResizeNWSE:   return SDL_SYSTEM_CURSOR_NWSE_RESIZE; break;
	case ImGuiMouseCursor_Hand:         return SDL_SYSTEM_CURSOR_POINTER; break;
	case ImGuiMouseCursor_NotAllowed:   return SDL_SYSTEM_CURSOR_NOT_ALLOWED; break;
	default:							return SDL_SYSTEM_CURSOR_DEFAULT; break;
	}
}

void UpdateCursor() {
	static SDL_SystemCursor curCursor = SDL_SYSTEM_CURSOR_DEFAULT;
	auto cursor = ImGui_ImplPlatform_GetCursor();
	SDL_SystemCursor sysCursor = GetSDLCursorFromImgui(cursor);
	if (sysCursor != curCursor) {
		curCursor = sysCursor;
		if (!g_builtinCursors[(int)curCursor]) {
			g_builtinCursors[(int)curCursor] = SDL_CreateSystemCursor(curCursor);
		}
	}
	SDL_SetCursor(g_builtinCursors[(int)curCursor]);
}

// --game-window: a second window showing just the game. For now it's only a way to test multiple outputs on
// the desktop, standing in for a TV over AirPlay. Later, real multi-window support (like ImGui's multi-viewports)
// will take this over.
static SDL_Window *g_gameWindow = nullptr;
static int g_gameOutputId = 0;

static void CreateGameWindow() {
	Draw::SwapchainDesc desc{};
	desc.tag = "GameWindow";
	const GPUBackend backend = (GPUBackend)g_Config.iGPUBackend;
	if (backend == GPUBackend::VULKAN) {
		g_gameWindow = SDL_CreateWindow("PPSSPP game", 960, 544, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
		WindowDesc windowDesc{};
		std::string error;
		if (g_gameWindow && !DetermineVulkanWindowSystem(g_gameWindow, &windowDesc, &error)) {
			fprintf(stderr, "Can't use the game window: %s\n", error.c_str());
			SDL_DestroyWindow(g_gameWindow);
			g_gameWindow = nullptr;
			return;
		}
		desc.winsys = windowDesc.winsys;
		desc.data1 = windowDesc.data1;
		desc.data2 = windowDesc.data2;
#if PPSSPP_PLATFORM(MAC)
	} else if (backend == GPUBackend::METAL) {
		g_gameWindow = SDL_CreateWindow("PPSSPP game", 960, 544, SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
		if (g_gameWindow) {
			desc.winsys = WINDOWSYSTEM_METAL_EXT;
			desc.data1 = SDL_Metal_GetLayer(SDL_Metal_CreateView(g_gameWindow));
		}
#endif
	} else {
		// TODO: Windows (D3D11) and OpenGL.
		fprintf(stderr, "--game-window needs Vulkan, or Metal on Mac\n");
		return;
	}
	if (!g_gameWindow) {
		fprintf(stderr, "Error creating the game window: %s\n", SDL_GetError());
		return;
	}
	SDL_GetWindowSizeInPixels(g_gameWindow, &desc.width, &desc.height);
	g_gameOutputId = Native_AddOutput(OutputRole::Game, desc);
}

// Returns true if the event was for the game window, and has been handled. Keys still go to the emulator.
static bool ProcessGameWindowEvent(const SDL_Event &event) {
	if (!g_gameWindow || SDL_GetWindowFromEvent(&event) != g_gameWindow) {
		return false;
	}
	switch (event.type) {
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
		Native_ResizeOutput(g_gameOutputId, event.window.data1, event.window.data2);
		return true;
	case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
		// The layer has to outlive the swapchain, so the window stays until we quit.
		Native_RemoveOutput(g_gameOutputId);
		SDL_HideWindow(g_gameWindow);
		return true;
	case SDL_EVENT_KEY_DOWN:
	case SDL_EVENT_KEY_UP:
	case SDL_EVENT_TEXT_INPUT:
		return false;
	default:
		// Window events, and the mouse, which the UI in the main window would get the coordinates wrong for.
		return true;
	}
}

static void ProcessSDLEvent(SDL_Window *window, const SDL_Event &event, InputStateTracker *inputTracker) {
	if (ProcessGameWindowEvent(event)) {
		return;
	}
#if PPSSPP_PLATFORM(MAC)
	// The file panel sheet doesn't stop SDL from delivering input to the window under it.
	// Releases still go through, so nothing gets stuck down.
	if (DarwinIsPanelOpen()) {
		switch (event.type) {
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_TEXT_INPUT:
		case SDL_EVENT_FINGER_DOWN:
		case SDL_EVENT_FINGER_MOTION:
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_MOTION:
		case SDL_EVENT_MOUSE_WHEEL:
			return;
		default:
			break;
		}
	}
#endif
	// SDL input uses window coordinates; convert to pixels before applying the UI scale.
	float inputPixelDensity = SDL_GetWindowPixelDensity(window);
	if (inputPixelDensity <= 0.0f) {
		inputPixelDensity = 1.0f;
	}
	switch (event.type) {
	case SDL_EVENT_QUIT:
		g_QuitRequested = 1;
		break;

	case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
		// SDL only sends a quit when the last window closes, and --game-window's may still be open.
		if (SDL_GetWindowFromEvent(&event) == window) {
			g_QuitRequested = 1;
		}
		break;

	#if !defined(MOBILE_DEVICE)
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
		{
			INFO_LOG(Log::UI, "SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: %d x %d", event.window.data1, event.window.data2);
			const int new_width = event.window.data1;
			const int new_height = event.window.data2;

			Native_NotifyWindowHidden(false);

			Uint64 window_flags = SDL_GetWindowFlags(window);
			bool fullscreen = (window_flags & SDL_WINDOW_FULLSCREEN) != 0;

			System_RunOnMainThread([new_width, new_height]() {
				Native_UpdateScreenScale(new_width, new_height, UIScaleFactorToMultiplier(g_Config.iUIScaleFactor));
			});

			// Set variable here in case fullscreen was toggled by hotkey
			if (g_Config.bFullScreen != fullscreen) {
				g_Config.bFullScreen = fullscreen;
			}

			if (!g_Config.bFullScreen) {
				int windowWidth = 0;
				int windowHeight = 0;
				SDL_GetWindowSize(window, &windowWidth, &windowHeight);
				g_Config.iWindowWidth = windowWidth;
				g_Config.iWindowHeight = windowHeight;
			}
			// The cursor visibility is handled by UpdateSDLCursor, which runs every frame.
			break;
		}
	case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
		{
			// The window moved to a display with a different content scale, or the
			// user changed the display's scale setting. Recompute the DPI and
			// re-derive the screen scale from the window's current pixel size.
			UpdateScreenDPI(window);

			int pixelWidth = 0;
			int pixelHeight = 0;
			SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
			System_RunOnMainThread([pixelWidth, pixelHeight]() {
				Native_UpdateScreenScale(pixelWidth, pixelHeight, UIScaleFactorToMultiplier(g_Config.iUIScaleFactor));
			});
			break;
		}
	case SDL_EVENT_WINDOW_MOVED:
		{
			Uint64 window_flags = SDL_GetWindowFlags(window);
			bool fullscreen = (window_flags & SDL_WINDOW_FULLSCREEN) != 0;
			if (!fullscreen) {
				g_Config.iWindowX = (int)event.window.data1;
				g_Config.iWindowY = (int)event.window.data2;
			}
			break;
		}

	case SDL_EVENT_WINDOW_FOCUS_LOST:
		{
			if (g_Config.bPauseOnLostFocus && GetUIState() == UISTATE_INGAME) {
				Core_Break(BreakReason::UIFocus, 0);
			}
		}
		break;

	case SDL_EVENT_WINDOW_FOCUS_GAINED:
		{
			if (Core_BreakReason() == BreakReason::UIFocus) {
				Core_Resume();
			}
		}
		break;

	case SDL_EVENT_WINDOW_MINIMIZED:
	case SDL_EVENT_WINDOW_HIDDEN:
			Native_NotifyWindowHidden(true);
			break;
	case SDL_EVENT_WINDOW_EXPOSED:
	case SDL_EVENT_WINDOW_SHOWN:
			Native_NotifyWindowHidden(false);
			break;
		break;
#endif
	case SDL_EVENT_KEY_DOWN:
		{
			if (g_textInputFallback && g_textFocus && !(event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI))) {
				// Layout-aware, with shift applied. Repeats are wanted here, like real text input.
				SDL_Keycode c = SDL_GetKeyFromScancode(event.key.scancode, event.key.mod, false);
				if (c >= 0x20 && c != 0x7F && !(c & SDLK_SCANCODE_MASK)) {
					KeyInput key;
					key.flags = KeyInputFlags::CHAR;
					key.unicodeChar = c;
					key.deviceId = DEVICE_ID_KEYBOARD;
					NativeKey(key);
				}
			}
			if (event.key.repeat > 0) {
				break;
			}
			int k = event.key.key;
			KeyInput key;
			key.flags = KeyInputFlags::DOWN;
			auto mapped = KeyMapRawSDLtoNative.find(k);
			if (mapped == KeyMapRawSDLtoNative.end() || mapped->second == NKCODE_UNKNOWN) {
				break;
			}
			key.keyCode = mapped->second;
			key.deviceId = DEVICE_ID_KEYBOARD;
			NativeKey(key);

			// Convenience subset of what "Enable standard shortcut keys" does on Windows.
			if (g_Config.bSystemControls) {
				bool ctrl = bool(event.key.mod & SDL_KMOD_CTRL);
				if (ctrl && (k == SDLK_W))
				{
					if (Core_IsStepping())
						Core_Resume();
					Core_Stop();
					System_PostUIMessage(UIMessage::REQUEST_GAME_STOP);
					// NOTE: Unlike Windows version, this
					// does not need Core_WaitInactive();
					// since SDL does not have a separate
					// UI thread.
				}

				/*
				// TODO: Enable this?
				if (k == SDLK_F11) {
#if !defined(MOBILE_DEVICE)
					g_Config.bFullScreen = !g_Config.bFullScreen;
					System_applyFullscreenState("");
#endif
				}
				*/
			}
			break;
		}
	case SDL_EVENT_KEY_UP:
		{
			if (event.key.repeat > 0) { break;}
			int k = event.key.key;
			KeyInput key;
			key.flags = KeyInputFlags::UP;
			auto mapped = KeyMapRawSDLtoNative.find(k);
			if (mapped == KeyMapRawSDLtoNative.end() || mapped->second == NKCODE_UNKNOWN) {
				break;
			}
			key.keyCode = mapped->second;
			key.deviceId = DEVICE_ID_KEYBOARD;
			NativeKey(key);
			break;
		}
	case SDL_EVENT_TEXT_INPUT:
		{
			int pos = 0;
			int c = u8_nextchar(event.text.text, &pos, strlen(event.text.text));
			KeyInput key;
			key.flags = KeyInputFlags::CHAR;
			key.unicodeChar = c;
			key.deviceId = DEVICE_ID_KEYBOARD;
			NativeKey(key);
			break;
		}
// This behavior doesn't feel right on a macbook with a touchpad.
#if !PPSSPP_PLATFORM(MAC)
	case SDL_EVENT_FINGER_MOTION:
		{
			int touchId = inputTracker->touchMap.TouchId(event.tfinger.fingerID);
			if (touchId < 0) {
				break;
			}
			int w, h;
			SDL_GetWindowSize(window, &w, &h);
			TouchInput input{};
			input.id = event.tfinger.fingerID;
			input.x = event.tfinger.x * w * inputPixelDensity * g_display.dpi_scale_x;
			input.y = event.tfinger.y * h * inputPixelDensity * g_display.dpi_scale_y;
			input.flags = TouchInputFlags::MOVE;
			input.timestamp = event.tfinger.timestamp;
			NativeTouch(input);
			break;
		}
	case SDL_EVENT_FINGER_DOWN:
		{
			int touchId = inputTracker->touchMap.TouchId(event.tfinger.fingerID);
			if (touchId < 0) {
				touchId = inputTracker->touchMap.AddNewTouch(event.tfinger.fingerID);
			}
			if (touchId < 0) {
				break;
			}
			int w, h;
			SDL_GetWindowSize(window, &w, &h);
			TouchInput input{};
			input.id = event.tfinger.fingerID;
			input.x = event.tfinger.x * w * inputPixelDensity * g_display.dpi_scale_x;
			input.y = event.tfinger.y * h * inputPixelDensity * g_display.dpi_scale_y;
			input.flags = TouchInputFlags::DOWN;
			input.timestamp = event.tfinger.timestamp;
			NativeTouch(input);

#if !PPSSPP_PLATFORM(SWITCH)
			KeyInput key{};
			key.deviceId = DEVICE_ID_MOUSE;
			key.keyCode = NKCODE_EXT_MOUSEBUTTON_1;
			key.flags = KeyInputFlags::DOWN;
			NativeKey(key);
#endif
			break;
		}
	case SDL_EVENT_FINGER_UP:
		{
			int touchId = inputTracker->touchMap.RemoveTouch(event.tfinger.fingerID);
			if (touchId < 0) {
				break;
			}
			int w, h;
			SDL_GetWindowSize(window, &w, &h);
			TouchInput input{};
			input.id = event.tfinger.fingerID;
			input.x = event.tfinger.x * w * inputPixelDensity * g_display.dpi_scale_x;
			input.y = event.tfinger.y * h * inputPixelDensity * g_display.dpi_scale_y;
			input.flags = TouchInputFlags::UP;
			input.timestamp = event.tfinger.timestamp;
			NativeTouch(input);

#if !PPSSPP_PLATFORM(SWITCH)
			KeyInput key;
			key.deviceId = DEVICE_ID_MOUSE;
			key.keyCode = NKCODE_EXT_MOUSEBUTTON_1;
			key.flags = KeyInputFlags::UP;
			NativeKey(key);
#endif
			break;
		}
#endif
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
		switch (event.button.button) {
		case SDL_BUTTON_LEFT:
			{
				float mx = event.button.x * inputPixelDensity * g_display.dpi_scale_x;
				float my = event.button.y * inputPixelDensity * g_display.dpi_scale_y;
				inputTracker->mouseDown |= 1;
				TouchInput input{};
				input.x = mx;
				input.y = my;
				input.flags = TouchInputFlags::DOWN | TouchInputFlags::MOUSE;
				input.buttons = 1;
				input.id = 0;
				NativeTouch(input);
				KeyInput key(DEVICE_ID_MOUSE, NKCODE_EXT_MOUSEBUTTON_1, KeyInputFlags::DOWN);
				NativeKey(key);
			}
			break;
		case SDL_BUTTON_RIGHT:
			{
				float mx = event.button.x * inputPixelDensity * g_display.dpi_scale_x;
				float my = event.button.y * inputPixelDensity * g_display.dpi_scale_y;
				inputTracker->mouseDown |= 2;
				TouchInput input{};
				input.x = mx;
				input.y = my;
				input.flags = TouchInputFlags::DOWN | TouchInputFlags::MOUSE;
				input.buttons = 2;
				input.id = 0;
				NativeTouch(input);
				KeyInput key(DEVICE_ID_MOUSE, NKCODE_EXT_MOUSEBUTTON_2, KeyInputFlags::DOWN);
				NativeKey(key);
			}
			break;
		case SDL_BUTTON_MIDDLE:
			{
				KeyInput key(DEVICE_ID_MOUSE, NKCODE_EXT_MOUSEBUTTON_3, KeyInputFlags::DOWN);
				NativeKey(key);
			}
			break;
		case SDL_BUTTON_X1:
			{
				KeyInput key(DEVICE_ID_MOUSE, NKCODE_EXT_MOUSEBUTTON_4, KeyInputFlags::DOWN);
				NativeKey(key);
			}
			break;
		case SDL_BUTTON_X2:
			{
				KeyInput key(DEVICE_ID_MOUSE, NKCODE_EXT_MOUSEBUTTON_5, KeyInputFlags::DOWN);
				NativeKey(key);
			}
			break;
		}
		break;
	case SDL_EVENT_MOUSE_WHEEL:
		{
			KeyInput key{};
			key.deviceId = DEVICE_ID_MOUSE;
			key.flags = KeyInputFlags::DOWN;
			float wheelY = event.wheel.y;
			if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
				wheelY = -wheelY;
			}
			if (wheelY != 0.0f) {
				const float scale = 30.0f;
				key.keyCode = wheelY > 0 ? NKCODE_EXT_MOUSEWHEEL_UP : NKCODE_EXT_MOUSEWHEEL_DOWN;
				key.flags |= KeyInputFlags::HAS_WHEEL_DELTA;
				int wheelDelta = (int)(fabsf(wheelY) * scale);
				key.flags = (KeyInputFlags)((u32)key.flags | (wheelDelta << 16));
				NativeKey(key);
				break;
			}
			break;
		}
	case SDL_EVENT_MOUSE_MOTION:
		{
			float mx = event.motion.x * inputPixelDensity * g_display.dpi_scale_x;
			float my = event.motion.y * inputPixelDensity * g_display.dpi_scale_y;
			TouchInput input{};
			input.x = mx;
			input.y = my;
			input.flags = TouchInputFlags::MOVE | TouchInputFlags::MOUSE;
			input.buttons = inputTracker->mouseDown;
			input.id = 0;
			NativeTouch(input);
			NativeMouseDelta(event.motion.xrel, event.motion.yrel);

			// Require a bit of movement to un-hide the cursor, so that jitter doesn't keep it up.
			if (fabsf(event.motion.xrel) > 1.0f || fabsf(event.motion.yrel) > 1.0f) {
				g_lastCursorMoveTime = time_now_d();
			}

			UpdateCursor();
			break;
		}
	case SDL_EVENT_MOUSE_BUTTON_UP:
		switch (event.button.button) {
		case SDL_BUTTON_LEFT:
			{
				float mx = event.button.x * inputPixelDensity * g_display.dpi_scale_x;
				float my = event.button.y * inputPixelDensity * g_display.dpi_scale_y;
				inputTracker->mouseDown &= ~1;
				TouchInput input{};
				input.x = mx;
				input.y = my;
				input.flags = TouchInputFlags::UP | TouchInputFlags::MOUSE;
				input.buttons = 1;
				NativeTouch(input);
				KeyInput key(DEVICE_ID_MOUSE, NKCODE_EXT_MOUSEBUTTON_1, KeyInputFlags::UP);
				NativeKey(key);
			}
			break;
		case SDL_BUTTON_RIGHT:
			{
				float mx = event.button.x * inputPixelDensity * g_display.dpi_scale_x;
				float my = event.button.y * inputPixelDensity * g_display.dpi_scale_y;
				inputTracker->mouseDown &= ~2;
				// Right button only emits mouse move events. This is weird,
				// but consistent with Windows. Needs cleanup.
				TouchInput input{};
				input.x = mx;
				input.y = my;
				input.flags = TouchInputFlags::UP | TouchInputFlags::MOUSE;
				input.buttons = 2;
				NativeTouch(input);
				KeyInput key(DEVICE_ID_MOUSE, NKCODE_EXT_MOUSEBUTTON_2, KeyInputFlags::UP);
				NativeKey(key);
			}
			break;
		case SDL_BUTTON_MIDDLE:
			{
				KeyInput key(DEVICE_ID_MOUSE, NKCODE_EXT_MOUSEBUTTON_3, KeyInputFlags::UP);
				NativeKey(key);
			}
			break;
		case SDL_BUTTON_X1:
			{
				KeyInput key(DEVICE_ID_MOUSE, NKCODE_EXT_MOUSEBUTTON_4, KeyInputFlags::UP);
				NativeKey(key);
			}
			break;
		case SDL_BUTTON_X2:
			{
				KeyInput key(DEVICE_ID_MOUSE, NKCODE_EXT_MOUSEBUTTON_5, KeyInputFlags::UP);
				NativeKey(key);
			}
			break;
		}
		break;

	case SDL_EVENT_AUDIO_DEVICE_ADDED:
		if (event.adevice.recording && SDL_IsAudioDevicePhysical(event.adevice.which)) {
			micDeviceChanged = true;
		}
		// Automatically switch to the new device.
		if (!event.adevice.recording) {
			const char *name = SDL_GetAudioDeviceName(event.adevice.which);
			if (!name) {
				INFO_LOG(Log::Audio, "Got bogus new audio device notification");
				break;
			}
			// Don't start auto switching for a couple of seconds, because some devices init on start.
			bool doAutoSwitch = g_Config.bAutoSwitchAudioDevice;
			if ((time_now_d() - g_audioStartTime) < 3.0) {
				INFO_LOG(Log::Audio, "Ignoring new audio device: %s (current: %s)", name, g_Config.sAudioDevice.c_str());
				doAutoSwitch = false;
			}
			if (doAutoSwitch || g_Config.sAudioDevice == name) {
				StopSDLAudioDevice();

				INFO_LOG(Log::Audio, "!!! Auto-switching to new audio device: '%s'", name);

				InitSDLAudioDevice(name ? name : "");
			}
		}
		break;
	case SDL_EVENT_AUDIO_DEVICE_REMOVED:
		if (event.adevice.recording && (event.adevice.which == micDevice || event.adevice.which == micPhysicalDevice)) {
			micDeviceChanged = true;
		}
		if (!event.adevice.recording && event.adevice.which == audioDev) {
			StopSDLAudioDevice();
			INFO_LOG(Log::Audio, "Audio device removed, reselecting");
			InitSDLAudioDevice();
		}
		break;

	case SDL_EVENT_DROP_BEGIN:
		g_dropBootedFile = false;
		break;
	case SDL_EVENT_DROP_FILE:
		// Only one file can be booted, so a multi-file drop boots the first.
		if (event.drop.data && !g_dropBootedFile) {
			g_dropBootedFile = true;
			System_PostUIMessage(UIMessage::REQUEST_GAME_BOOT, event.drop.data);
		}
		break;

	default:
		if (joystick) {
			joystick->ProcessInput(event);
		}
		break;
	}
}

void UpdateTextFocus(SDL_Window *window) {
#if PPSSPP_PLATFORM(SWITCH)
	g_textFocusChanged = false;
#else
	if (g_textFocusChanged) {
		DEBUG_LOG(Log::System, "Updating text focus: %d", g_textFocus);
		if (g_textFocus) {
			if (!SDL_StartTextInput(window) && !g_textInputFallback) {
				WARN_LOG(Log::System, "SDL_StartTextInput failed (%s), generating text from key events instead. Update SDL, or set SDL_VIDEO_DRIVER=x11, for full text input.", SDL_GetError());
				g_textInputFallback = true;
			}
		} else {
			SDL_StopTextInput(window);
		}
		g_textFocusChanged = false;
	}
#endif
}

void UpdateSDLCursor() {
#if !defined(MOBILE_DEVICE)
	lastUIState = GetUIState();

	// In fullscreen while in-game, the cursor auto-hides once the mouse has been still for a
	// moment, and comes back as soon as it's moved again. Same idea as the Windows version.
	// While a button is held the user is interacting, so keep it visible.
	const bool buttonDown = (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) != 0;
	const bool autoHide = g_Config.bFullScreen && lastUIState == UISTATE_INGAME && !buttonDown;
	const bool visible = !autoHide || time_now_d() - g_lastCursorMoveTime < CURSOR_HIDE_DELAY;

	static bool cursorVisible = true;
	if (visible != cursorVisible) {
		cursorVisible = visible;
		if (visible) {
			SDL_ShowCursor();
		} else {
			SDL_HideCursor();
		}
	}
#endif
}

#ifdef _WIN32
#undef main
#endif

#if PPSSPP_PLATFORM(SWITCH)
// ---- Timestamped logging ---------------------------------------------------------------
// The log file is append-mode, so without a timestamp there is no telling which launch a block of
// output belongs to. Two layers:
//  1. The "new launch" separator carries the wall-clock date, taken from the time service.
//  2. Every completed line is prefixed with [HH:MM:SS.mmm] by a cookie stream that stdout/stderr
//     are pointed at (see main()). While the time service is not up yet - or if the console has no
//     wall clock - the prefix degrades to [up <seconds>s] instead of lying with a wrong date.

static bool g_switchLogWallClock = false;

// Wall-clock date+time from the time service. False when the service is not usable yet.
static bool SwitchGetWallClockDateTime(char *out, size_t outSize) {
	// uint64_t (not PPSSPP's u64 macro, which maps to a different typedef) matches libnx's u64 here.
	uint64_t posixTime = 0;
	if (R_FAILED(timeGetCurrentTime(TimeType_UserSystemClock, &posixTime)) || posixTime == 0) {
		return false;
	}
	const time_t secs = (time_t)posixTime;
	struct tm tmLocal{};
	if (!localtime_r(&secs, &tmLocal)) {
		return false;
	}
	snprintf(out, outSize, "%04d-%02d-%02d %02d:%02d:%02d",
		tmLocal.tm_year + 1900, tmLocal.tm_mon + 1, tmLocal.tm_mday,
		tmLocal.tm_hour, tmLocal.tm_min, tmLocal.tm_sec);
	return true;
}

struct SwitchLogCookie {
	FILE *out = nullptr;
	std::mutex *lock = nullptr;
	bool wallClock = false;
	char pending[512] = {};
	size_t pendingLen = 0;
};

static void SwitchLogLineStamp(const SwitchLogCookie &c, char *out, size_t outSize) {
	struct timespec ts{};
	if (c.wallClock && clock_gettime(CLOCK_REALTIME, &ts) == 0) {
		const time_t secs = (time_t)ts.tv_sec;
		struct tm tmLocal{};
		if (localtime_r(&secs, &tmLocal)) {
			snprintf(out, outSize, "[%02d:%02d:%02d.%03d]", tmLocal.tm_hour, tmLocal.tm_min,
				tmLocal.tm_sec, (int)(ts.tv_nsec / 1000000));
			return;
		}
	}
	// No wall clock: fall back to uptime, clearly marked so it cannot be mistaken for a date.
	struct timespec mono{};
	clock_gettime(CLOCK_MONOTONIC, &mono);
	snprintf(out, outSize, "[up %lld.%03llds]", (long long)mono.tv_sec, (long long)(mono.tv_nsec / 1000000));
}

// Writes one buffered line with its timestamp. Caller holds the lock.
static void SwitchLogFlushPending(SwitchLogCookie &c) {
	if (c.out && c.pendingLen) {
		char stamp[40];
		SwitchLogLineStamp(c, stamp, sizeof(stamp));
		fprintf(c.out, "%s %.*s", stamp, (int)c.pendingLen, c.pending);
		fflush(c.out);
	}
	c.pendingLen = 0;
}

static ssize_t SwitchLogCookieWrite(void *cookie, const char *buf, size_t size) {
	SwitchLogCookie &c = *(SwitchLogCookie *)cookie;
	if (!c.out) {
		return (ssize_t)size;  // target already closed - drop quietly
	}
	std::lock_guard<std::mutex> guard(*c.lock);
	size_t start = 0;
	while (start < size) {
		const char *newline = (const char *)memchr(buf + start, '\n', size - start);
		const size_t chunkLen = newline ? (size_t)(newline - (buf + start) + 1) : (size - start);
		if (c.pendingLen + chunkLen >= sizeof(c.pending)) {
			// Would overflow: emit what is buffered as its own line and continue. Over-long lines
			// come out split, which is acceptable for a diagnostic log.
			if (c.pendingLen) {
				SwitchLogFlushPending(c);
			} else {
				fwrite(buf + start, 1, chunkLen - (newline ? 1 : 0), c.out);
				fputc('\n', c.out);
				start += chunkLen;
				continue;
			}
		}
		memcpy(c.pending + c.pendingLen, buf + start, chunkLen);
		c.pendingLen += chunkLen;
		start += chunkLen;
		if (newline) {
			SwitchLogFlushPending(c);
		}
	}
	return (ssize_t)size;
}

// Only flushes: newlib flushes stdout/stderr during exit but does not close them, and both streams
// share this cookie, so freeing anything here would be a use-after-free at best.
static int SwitchLogCookieClose(void *cookie) {
	SwitchLogCookie &c = *(SwitchLogCookie *)cookie;
	if (c.out && c.pendingLen) {
		std::lock_guard<std::mutex> guard(*c.lock);
		SwitchLogFlushPending(c);
	}
	return 0;
}

// Opens the log file and points stdout/stderr at a timestamping cookie stream over it.
// Returns false if the real file could not be opened (the console streams stay untouched then).
// Tracks whether stdout/stderr currently point at the log (or the silenced sink). Guard both the
// early best-effort open in main() and the authoritative open after config load against double-open.
static bool g_logStreamOpen = false;

static bool SwitchOpenTimestampedLog(const char *logPath, bool append) {
	if (g_logStreamOpen) {
		return true;  // already open - nothing to do, and re-opening would leak the old stream
	}
	FILE *out = fopen(logPath, append ? "a" : "w");
	if (!out) {
		return false;
	}
	setvbuf(out, nullptr, _IONBF, 0);

	timeInitialize();  // best effort; no timeExit() - other components depend on the service

	uint64_t posixTime = 0;
	g_switchLogWallClock = R_SUCCEEDED(timeGetCurrentTime(TimeType_UserSystemClock, &posixTime)) && posixTime != 0;

	// The separator keeps its own full date, independent of the per-line prefix.
	char launchStamp[40]{};
	if (SwitchGetWallClockDateTime(launchStamp, sizeof(launchStamp))) {
		fprintf(out, "\n===== new launch [%s] =====\n", launchStamp);
	} else {
		fprintf(out, "\n===== new launch =====\n");
	}
	fflush(out);

	static std::mutex s_switchLogLock;  // shared by both stdout and stderr views
	static SwitchLogCookie *s_cookie = nullptr;
	if (!s_cookie) {
		s_cookie = new SwitchLogCookie();
		s_cookie->lock = &s_switchLogLock;
	}
	s_cookie->out = out;
	s_cookie->wallClock = g_switchLogWallClock;

	cookie_io_functions_t io{};
	io.write = SwitchLogCookieWrite;
	io.close = SwitchLogCookieClose;
	FILE *stream = fopencookie(s_cookie, "w", io);
	if (!stream) {
		// Cookie streams unavailable on this toolchain: plain redirection, no prefixes.
		WARN_LOG(Log::System, "fopencookie failed - log keeps working without timestamps");
		stdout = out;
		stderr = out;
		g_logStreamOpen = true;
		return true;
	}
	setvbuf(stream, nullptr, _IONBF, 0);  // our callback buffers per line already
	stdout = stream;
	stderr = stream;
	g_logStreamOpen = true;
	INFO_LOG(Log::System, "Log stream opened with timestamps (wall clock: %s)",
		g_switchLogWallClock ? "yes" : "no (uptime prefix)");
	return true;
}
// One-time migration: a legacy install kept everything under /switch/ppsspp-nx/, while the data
// root now follows the NRO. Move the PSP directory over exactly once - rename is instant when both
// sides are on the same SD partition, which is the normal case. The old directory itself is kept
// (it may hold things the user put there), only PSP is moved; cache/ and lsfg/ are caches and are
// rebuilt at the new location.
static void SwitchCopyDirectoryTree(const Path &from, const Path &to) {
	File::CreateFullPath(to);
	std::vector<File::FileInfo> entries;
	if (!File::GetFilesInDir(from, &entries, nullptr, File::GETFILES_GETHIDDEN)) {
		return;
	}
	for (const File::FileInfo &entry : entries) {
		if (entry.isDirectory) {
			SwitchCopyDirectoryTree(entry.fullName, to / entry.name);
		} else if (!File::Copy(entry.fullName, to / entry.name)) {
			WARN_LOG(Log::System, "[migrate] failed to copy %s", entry.fullName.c_str());
		}
	}
}

static void SwitchMigrateLegacyData() {
	const std::string &root = SwitchAppRoot();
	if (root == "/switch/ppsspp-nx/") {
		return;  // data is already where it belongs
	}
	const std::string legacyPsp = "/switch/ppsspp-nx/PSP";
	const std::string newPsp = root + "PSP";
	// Already migrated (or a fresh install that wrote its own config) - never move again.
	if (File::Exists(Path(newPsp) / "SYSTEM" / "ppsspp.ini")) {
		return;
	}
	if (!File::Exists(Path(legacyPsp))) {
		return;  // nothing to migrate
	}
	File::CreateDir(Path(root.substr(0, root.size() - 1)));
	// Same device (both on the SD card): rename is instant and atomic.
	if (rename(legacyPsp.c_str(), newPsp.c_str()) == 0) {
		printf("[migrate] moved /switch/ppsspp-nx/PSP -> %sPSP\n", root.c_str());
		return;
	}
	// Cross-device or blocked by something: copy instead, keeping the old directory in place.
	// The PSP directory can hold a lot of save data, so warn that this may take a while.
	WARN_LOG(Log::System, "[migrate] rename failed (%s), copying instead - this can take a while", strerror(errno));
	SwitchCopyDirectoryTree(Path(legacyPsp), Path(newPsp));
	printf("[migrate] copied /switch/ppsspp-nx/PSP -> %sPSP\n", root.c_str());
}
// Library: the game browser's default games directory and its own config follow the new root too.
//
// The games directory can hold tens of GB, so it is rename-only: if the rename cannot be done
// (cross-device, something holding the directory) it is left exactly where it is and the log says
// so - silently copying that much data on every boot would be far worse than a one-time hint.
static void SwitchMigrateLegacyLibrary() {
	const std::string &root = SwitchAppRoot();
	if (root == "/switch/ppsspp-nx/") {
		return;  // data is already where it belongs
	}
	const Path legacyGames("/switch/ppsspp-nx/games");
	const Path newGames = Path(root) / "games";
	if (File::IsDirectory(legacyGames) && !File::Exists(newGames)) {
		if (rename(legacyGames.c_str(), newGames.c_str()) == 0) {
			printf("[migrate] moved /switch/ppsspp-nx/games -> %sgames\n", root.c_str());
		} else {
			WARN_LOG(Log::System, "[migrate] could not move /switch/ppsspp-nx/games (%s) - it stays there; move it to %sgames manually if wanted", strerror(errno), root.c_str());
		}
	}
	const Path legacyIni("/switch/ppsspp-nx/library.ini");
	const Path newIni = Path(root) / "library.ini";
	if (File::Exists(legacyIni) && !File::Exists(newIni)) {
		if (rename(legacyIni.c_str(), newIni.c_str()) == 0) {
			printf("[migrate] moved /switch/ppsspp-nx/library.ini -> %slibrary.ini\n", root.c_str());
		} else if (File::Copy(legacyIni, newIni)) {
			// Tiny file: copying is a fine fallback.
			printf("[migrate] copied /switch/ppsspp-nx/library.ini -> %slibrary.ini\n", root.c_str());
		} else {
			WARN_LOG(Log::System, "[migrate] could not move library.ini (%s)", strerror(errno));
		}
	}
}
// Turns the log stream off without closing anything: stdout/stderr move to a drop-everything
// cookie stream. (/dev/null does not exist on Horizon, and fclose()ing stdout mid-run is a
// use-after-free trap for anything that still prints.) The cookie machinery already drops writes
// when its FILE* is unset, so this reuses it as-is.
static void SwitchSilenceLogStream() {
	static std::mutex s_nullLock;
	static SwitchLogCookie s_nullCookie{nullptr, &s_nullLock};
	cookie_io_functions_t io{};
	io.write = SwitchLogCookieWrite;
	io.close = SwitchLogCookieClose;
	FILE *sink = fopencookie(&s_nullCookie, "w", io);
	if (sink) {
		setvbuf(sink, nullptr, _IONBF, 0);
		stdout = sink;
		stderr = sink;
	}
	g_logStreamOpen = false;
}

void SwitchApplyLogFileSetting(bool enabled) {
	if (enabled && !g_logStreamOpen) {
		// The early ini guess missed (memstick moved, ini not written yet, ...) - open now, append,
		// so whatever was logged earlier in this run is preserved.
		if (SwitchOpenTimestampedLog("sdmc:/ppsspp_log.txt", true)) {
			printf("[log] authoritative open after config load\n");
		} else {
			WARN_LOG(Log::System, "authoritative log open failed - sdmc:/ppsspp_log.txt not writable");
		}
	} else if (!enabled && g_logStreamOpen) {
		// Config says off but the early guess opened it: silence the stream.
		SwitchSilenceLogStream();
		printf("[log] authoritative close after config load (LogFile=false, early guess was on)\n");
	}
	// Shown on screen only when logging is actually on: when it is off there is nothing to look
	// for, and a constant "off" toast on every start is just noise. Chinese, matching this
	// build's audience (drawn through the same SDL3_ttf + Noto pipeline as the rest of the UI).
	if (enabled) {
		g_OSD.Show(OSDType::MESSAGE_INFO, "日志文件正在记录", 5.0f, "log_file_state");
	}
}
#endif  // PPSSPP_PLATFORM(SWITCH)

int main(int argc, char *argv[]) {
	TimeInit();

#if PPSSPP_PLATFORM(SWITCH)
	// The data root follows the NRO's directory, and a legacy install's PSP directory is moved over
	// once. This has to happen before anything reads or writes under the root - including the log
	// gate below, which reads the ini that the migration may just have moved.
	SwitchSetAppRootFromNro(argc > 0 && argv[0] ? SwitchNormalizeNroPath(argv[0]) : std::string());
	SwitchMigrateLegacyData();
	SwitchMigrateLegacyLibrary();

	// Graphics-backend selection failures are invisible on hardware otherwise, so mirror
	// stdout/stderr to the SD card root. Overwrites on each launch.
	// Append across launches so several runs (e.g. launching a game from the PPSSPP
	// menu vs. from the XMB's UMD slot) can be compared in one file. Reset once it
	// grows past 512 KB so it never fills the SD card.
	{
		// Opt-in: Settings > Tools > "Generate log file" (ppsspp.ini: LogFile = True). A release
		// build should not create files on the SD card unless the user asked for it.
		// The ini is looked for in two places: the migrated data root (next to the NRO) first, then
		// the legacy hardcoded location for installs that have not been migrated yet. Either being
		// set to true turns logging on.
		const std::string rootIniPath = SwitchAppRoot() + "PSP/SYSTEM/ppsspp.ini";
		const char *fixedIniPath = "/switch/ppsspp-nx/PSP/SYSTEM/ppsspp.ini";
		// Escape hatch: if the app dies before the settings screen can be reached, the LogFile
		// switch is useless - the user can never turn it on. An empty marker file in the SD card
		// root forces logging on, so a shared build stays quiet while whoever hits a problem can
		// still produce a log without touching the menu.
		const bool debugMarker = File::Exists(Path("sdmc:/ppsspp_debug.txt"));
		const bool iniEnabled = SwitchReadBoolSettingFromIni(fixedIniPath, "LogFile")
			|| SwitchReadBoolSettingFromIni(rootIniPath.c_str(), "LogFile");
		// Marker file wins: it exists precisely because the ini route was not usable.
		const bool logFileEnabled = debugMarker || iniEnabled;
		if (debugMarker) {
			printf("[log] file logging forced by sdmc:/ppsspp_debug.txt\n");
		} else {
			printf("[log] file logging %s (LogFile '%s' in '%s' or '%s')\n",
				logFileEnabled ? "enabled" : "disabled", logFileEnabled ? "LogFile=True" : "LogFile not set",
				fixedIniPath, rootIniPath.c_str());
		}
		if (logFileEnabled) {
			const char *logPath = "sdmc:/ppsspp_log.txt";
			bool append = false;
			if (FILE *existing = fopen(logPath, "rb")) {
				fseek(existing, 0, SEEK_END);
				const long size = ftell(existing);
				fclose(existing);
				append = size > 0 && size < 512 * 1024;
			}
			if (!SwitchOpenTimestampedLog(logPath, append)) {
				fprintf(stderr, "[log] failed to open %s\n", logPath);
			}
		}
	}
#endif

#if PPSSPP_PLATFORM(SWITCH)
	std::string restartHandoff = SwitchTakeRestartHandoff();

	// Registered first, so it runs last: atexit handlers and static destructors run in reverse
	// order of registration, so this line is printed once every global destructor in the
	// program has finished, right before libnx's exit path takes over. If the log ends with
	// this line and the console still reports an error, the fault is in libnx's __appExit()
	// (see the tracing in __nx_win_exit) and not in our own teardown.
	atexit([]() { printf("[exit] atexit: static destructors finished\n"); });
#endif
	CommandLineOptions cmdLineOptions;
	CommandLineParseResult parseResult = cmdLineOptions.Parse(argc, (const char **)argv);
	switch (parseResult) {
	case CommandLineParseResult::Exit:
		return 0;
	case CommandLineParseResult::Error:
		return 1;
	default:
		// Continue with launch.
		break;
	}

#if PPSSPP_PLATFORM(SWITCH)
	if (!restartHandoff.empty()) {
		const char *restartArgv[] = { argc > 0 && argv[0] ? argv[0] : "PPSSPP", restartHandoff.c_str() };
		parseResult = cmdLineOptions.Parse((int)ARRAY_SIZE(restartArgv), restartArgv);
		if (parseResult != CommandLineParseResult::Continue) {
			return parseResult == CommandLineParseResult::Exit ? 0 : 1;
		}
	}
	SetCurrentThreadToProcessAffinity();
	if (argc > 0 && argv[0]) {
		g_switchExecutablePath = SwitchNormalizeNroPath(argv[0]);
	}
	if (g_switchExecutablePath.empty()) {
		// No usable argv[0]: fall back to the NRO inside the data root.
		g_switchExecutablePath = "sdmc:" + SwitchAppRoot() + "PPSSPP.nro";
	}
	SwitchUpdate_SetExecutablePath(g_switchExecutablePath);
	std::string updateRecoveryError;
	if (!SwitchUpdate_RecoverInstallation(&updateRecoveryError)) {
		fprintf(stderr, "Update recovery failed: %s\n", updateRecoveryError.c_str());
	}
#endif

	g_logManager.EnableOutput(LogOutput::Stdio);

#ifdef HAVE_LIBNX
	if (R_FAILED(romfsInit())) {
		fprintf(stderr, "Unable to mount RomFS.\n");
		return 1;
	}
	// Ask for the highest official performance configuration now that the runtime services are up.
	SwitchRequestMaxPerformance();
	if (R_SUCCEEDED(socketInitializeDefault())) {
		g_socketInitialized = true;
		g_nxlinkSocket = nxlinkStdio();
	}
#else // HAVE_LIBNX
	// Ignore sigpipe.
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		perror("Unable to ignore SIGPIPE");
	}
#endif // HAVE_LIBNX

	PROFILE_INIT();
	glslang::InitializeProcess();

#if PPSSPP_PLATFORM(RPI)
	bcm_host_init();
#endif
	putenv((char*)"SDL_VIDEO_CENTERED=1");
	SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
#ifdef SDL_HINT_ENABLE_SCREEN_KEYBOARD
	SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0");
#endif

#ifdef SDL_HINT_TOUCH_MOUSE_EVENTS
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif

	// SDL's automatic pick between Wayland and X11 doesn't always land on Wayland even in a
	// Wayland session, and going through XWayland instead costs us scaling and input quality.
	// So ask for it explicitly when the session looks like Wayland - unless the user has set
	// SDL_VIDEO_DRIVER, in which case they've already told us what they want.
	// WAYLAND_DISPLAY is only set on Wayland sessions, so this is a no-op elsewhere.
	bool preferWayland = false;
	if (getenv("WAYLAND_DISPLAY") && !getenv("SDL_VIDEO_DRIVER")) {
		SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
		preferWayland = true;
	}

	bool vulkanMayBeAvailable = false;
	if (VulkanMayBeAvailable()) {
		fprintf(stderr, "DEBUG: Vulkan might be available.\n");
		vulkanMayBeAvailable = true;
	} else {
		fprintf(stderr, "DEBUG: Vulkan is not available, not using Vulkan.\n");
	}

	const int compiled = SDL_VERSION;
	const int linked = SDL_GetVersion();
	int set_xres = cmdLineOptions.xres.value_or(-1);
	int set_yres = cmdLineOptions.yres.value_or(-1);
	float set_dpi = (float)cmdLineOptions.dpi.value_or(0.0);
	float set_scale = (float)cmdLineOptions.scale.value_or(1.0);

	Uint32 mode = 0;

	std::string app_name;
	std::string app_name_nice;
	std::string version;
	bool landscape;
	NativeGetAppInfo(&app_name, &app_name_nice, &landscape, &version);

	bool joystick_enabled = true;
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
		fprintf(stderr, "Failed to initialize SDL with joystick support. Retrying without.\n");
		joystick_enabled = false;
		if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
			bool initialized = false;
			if (preferWayland) {
				// Asking for Wayland may be exactly what failed, so let SDL pick instead.
				fprintf(stderr, "Unable to initialize SDL with the Wayland video driver (%s). Letting SDL choose.\n", SDL_GetError());
				SDL_SetHint(SDL_HINT_VIDEO_DRIVER, nullptr);
				preferWayland = false;
				// Retry with joystick support - it was the video driver that failed, not the joysticks.
				if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
					joystick_enabled = true;
					initialized = true;
				} else {
					initialized = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
				}
			}
			if (!initialized) {
				fprintf(stderr, "Unable to initialize SDL: %s\n", SDL_GetError());
				return 1;
			}
		}
	}

	const char *videoDriver = SDL_GetCurrentVideoDriver();
	fprintf(stderr, "Info: SDL video driver: %s\n", videoDriver ? videoDriver : "(none)");

	fprintf(stderr, "Info: We compiled against SDL version %d.%d.%d", SDL_VERSIONNUM_MAJOR(compiled), SDL_VERSIONNUM_MINOR(compiled), SDL_VERSIONNUM_MICRO(compiled));
	if (compiled != linked) {
		fprintf(stderr, ", but we are linking against SDL version %d.%d.%d., be aware that this can lead to unexpected behaviors\n", SDL_VERSIONNUM_MAJOR(linked), SDL_VERSIONNUM_MINOR(linked), SDL_VERSIONNUM_MICRO(linked));
	} else {
		fprintf(stderr, " and we are linking against SDL version %d.%d.%d. :)\n", SDL_VERSIONNUM_MAJOR(linked), SDL_VERSIONNUM_MINOR(linked), SDL_VERSIONNUM_MICRO(linked));
	}

	// Get the video info before doing anything else, so we don't get skewed resolution results.
	// TODO: support multiple displays correctly
	int displayCount = 0;
	SDL_DisplayID *displayIDs = SDL_GetDisplays(&displayCount);
	if (!displayIDs || displayCount == 0) {
		fprintf(stderr, "Could not enumerate displays: %s\n", SDL_GetError());
		return 1;
	}
	const SDL_DisplayMode *displayMode = SDL_GetCurrentDisplayMode(displayIDs[0]);
	if (!displayMode) {
		fprintf(stderr, "Could not get display mode: %s\n", SDL_GetError());
		SDL_free(displayIDs);
		return 1;
	}
	g_DesktopWidth = displayMode->w;
	g_DesktopHeight = displayMode->h;
	g_RefreshRate = displayMode->refresh_rate;
	SDL_free(displayIDs);

	// TODO: Should only call this if we actually intend to use OpenGL.
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	// Force fullscreen if the resolution is too low to run windowed.
	if (g_DesktopWidth < 480 * 2 && g_DesktopHeight < 272 * 2) {
		cmdLineOptions.fullscreen = true;
	}

	// If we're on mobile, don't try for windowed either.
#if defined(MOBILE_DEVICE) && !PPSSPP_PLATFORM(SWITCH)
	cmdLineOptions.fullscreen = true;
#elif defined(USING_FBDEV) || PPSSPP_PLATFORM(SWITCH)
	cmdLineOptions.fullscreen = true;
#else
	mode |= SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif

	if (cmdLineOptions.fullscreen) {
		g_display.pixel_xres = g_DesktopWidth;
		g_display.pixel_yres = g_DesktopHeight;
		g_Config.bFullScreen = true;
	} else {
		// set a sensible default resolution (2x)
		g_display.pixel_xres = 480 * 2 * set_scale;
		g_display.pixel_yres = 272 * 2 * set_scale;
		g_Config.bFullScreen = false;
	}

	if (!landscape) {
		std::swap(g_display.pixel_xres, g_display.pixel_yres);
	}

	if (set_xres > 0) {
		g_display.pixel_xres = set_xres;
	}
	if (set_yres > 0) {
		g_display.pixel_yres = set_yres;
	}
	if (set_dpi > 0) {
		g_ForcedDPI = set_dpi;
	}

	// Mac / Linux
	char path[2048] = {};
#if PPSSPP_PLATFORM(SWITCH)
	// Data root follows the NRO's directory (see SwitchAppRoot), with the historical location as
	// the fallback when argv[0] was unusable.
	snprintf(path, sizeof(path), "%s", SwitchAppRoot().c_str());
#else
	const char *the_path = getenv("HOME");
	if (!the_path) {
		struct passwd *pwd = getpwuid(getuid());
		if (pwd)
			the_path = pwd->pw_dir;
	}
	if (the_path)
		snprintf(path, sizeof(path), "%s", the_path);
#endif
	if (path[0] != '\0' && path[strlen(path) - 1] != '/')
		strncat(path, "/", sizeof(path) - strlen(path) - 1);

#if PPSSPP_PLATFORM(MAC)
	std::string external_dir_str;
	if (SDL_GetBasePath())
		external_dir_str = std::string(SDL_GetBasePath()) + "/assets";
	else
		external_dir_str = "/tmp";
	const char *external_dir = external_dir_str.c_str();
#else
	const char *external_dir = "/tmp";
#endif

	// After NativeInit, code should no longer look at cmdLineOptions, they should have been translated
	// into g_Config settings. This is because NativeInit may modify g_Config settings based on the command line options.
	NativeInit(argc, (const char **)argv, cmdLineOptions, path, external_dir, nullptr);

#if PPSSPP_PLATFORM(SWITCH)
	File::CreateFullPath(Path(SwitchAppRoot() + "lsfg"));
	SwitchLSFG_Configure(g_Config.bSwitchFrameGeneration && g_Config.iGPUBackend == (int)GPUBackend::VULKAN, 0.25f, true);
	SwitchLibrary::Initialize();
	const std::string mesaCacheDir = SwitchAppRoot() + "cache/mesa";
	File::CreateFullPath(Path(mesaCacheDir));
	setenv("MESA_SHADER_CACHE_DIR", mesaCacheDir.c_str(), 1);
	setenv("MESA_SHADER_CACHE_MAX_SIZE", "256M", 1);
#endif

	// Use the setting from the config when initing the window.
	if (g_Config.bFullScreen) {
		mode |= SDL_WINDOW_FULLSCREEN;
		g_display.pixel_xres = g_DesktopWidth;
		g_display.pixel_yres = g_DesktopHeight;
	}

	int x = SDL_WINDOWPOS_UNDEFINED_DISPLAY(getDisplayNumber());
	int y = SDL_WINDOWPOS_UNDEFINED;
	int w = g_display.pixel_xres;
	int h = g_display.pixel_yres;

	if (!g_Config.bFullScreen) {
		if (g_Config.iWindowX != -1)
			x = g_Config.iWindowX;
		if (g_Config.iWindowY != -1)
			y = g_Config.iWindowY;
		if (g_Config.iWindowWidth > 0 && set_xres <= 0)
			w = g_Config.iWindowWidth;
		if (g_Config.iWindowHeight > 0 && set_yres <= 0)
			h = g_Config.iWindowHeight;
	}

	// Switch away from Vulkan if not available.
	int fallbackGPUBackend = -1;
	switch ((GPUBackend)g_Config.iGPUBackend) {
	case GPUBackend::VULKAN:
		if (!vulkanMayBeAvailable) {
			printf("[backend] Vulkan reported unavailable, switching to OpenGL\n");
			fprintf(stderr, "Vulkan is not available, switching to OpenGL.\n");
			g_Config.iGPUBackend = (int)GPUBackend::OPENGL;
		} else {
			fallbackGPUBackend = (int)GPUBackend::OPENGL;
		}
		break;
	case GPUBackend::OPENGL:
		fallbackGPUBackend = (int)GPUBackend::VULKAN;
		break;
	case GPUBackend::ZINK:
		fallbackGPUBackend = (int)GPUBackend::VULKAN;
		break;
#if PPSSPP_PLATFORM(MAC)
	case GPUBackend::METAL:
		fallbackGPUBackend = vulkanMayBeAvailable ? (int)GPUBackend::VULKAN : (int)GPUBackend::OPENGL;
		break;
#endif
	default:
		fprintf(stderr, "Unknown GPU backend %d, switching to Vulkan.\n", g_Config.iGPUBackend);
		g_Config.iGPUBackend = (int)GPUBackend::VULKAN;
		fallbackGPUBackend = (int)GPUBackend::OPENGL;
		break;
	}

	SDL_Window *window = nullptr;
	WindowDesc windowDesc;
	auto initializeBackend = [&](GPUBackend backend, GraphicsContext **graphicsContext, std::string *errorMessage) -> bool {
		GraphicsContext *ctx = nullptr;
		windowDesc = {};
		if (backend == GPUBackend::OPENGL || backend == GPUBackend::ZINK) {
#if PPSSPP_PLATFORM(SWITCH)
			setenv("GALLIUM_DRIVER", backend == GPUBackend::ZINK ? "zink" : "nouveau", 1);
#endif
			SDL_GLContext glContext = nullptr;
			window = CreateSDLGLWindowAndContext(x, y, w, h, mode, cmdLineOptions.force_gl_version, &glContext, errorMessage);

			windowDesc.winsys = WINDOWSYSTEM_SDL;
			windowDesc.data1 = (void *)window;
			windowDesc.data2 = (void *)glContext;

			ctx = new SDLGLGraphicsContext();
#if PPSSPP_PLATFORM(MAC)
		} else if (backend == GPUBackend::METAL) {
			window = SDL_CreateWindow("Initializing graphics...", w, h, (SDL_WindowFlags)(mode | SDL_WINDOW_METAL | SDL_WINDOW_HIDDEN));
			if (!window) {
				*errorMessage = StringFromFormat("Error creating SDL window: %s", SDL_GetError());
				return false;
			}
			if (x != SDL_WINDOWPOS_UNDEFINED && y != SDL_WINDOWPOS_UNDEFINED) {
				SDL_SetWindowPosition(window, x, y);
			}
			SDL_MetalView metalView = SDL_Metal_CreateView(window);
			if (!metalView) {
				*errorMessage = StringFromFormat("Error creating Metal view: %s", SDL_GetError());
				return false;
			}
			windowDesc.winsys = WINDOWSYSTEM_METAL_EXT;
			windowDesc.data1 = SDL_Metal_GetLayer(metalView);
			windowDesc.data2 = (void *)window;
			ctx = new MetalGraphicsContext();
#endif
		} else {
			// Use a local copy of mode: this flag combination is Vulkan-specific, and if we fall back to
			// OpenGL below, we don't want SDL_WINDOW_VULKAN to stick around and get OR'd in there too.
			Uint32 vulkanMode = mode | SDL_WINDOW_HIDDEN;
#if PPSSPP_PLATFORM(SWITCH)
			SDL_PropertiesID windowProperties = SDL_CreateProperties();
			if (windowProperties) {
				SDL_SetStringProperty(windowProperties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Initializing graphics...");
				SDL_SetNumberProperty(windowProperties, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
				SDL_SetNumberProperty(windowProperties, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
				SDL_SetNumberProperty(windowProperties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
				SDL_SetNumberProperty(windowProperties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
				SDL_SetNumberProperty(windowProperties, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, vulkanMode);
				SDL_SetBooleanProperty(windowProperties, SDL_PROP_WINDOW_CREATE_EXTERNAL_GRAPHICS_CONTEXT_BOOLEAN, true);
				window = SDL_CreateWindowWithProperties(windowProperties);
				SDL_DestroyProperties(windowProperties);
			}
#else
			vulkanMode |= SDL_WINDOW_VULKAN;
			window = SDL_CreateWindow("Initializing graphics...", w, h, (SDL_WindowFlags)vulkanMode);
#endif
			if (!window) {
				if (errorMessage) {
					*errorMessage = StringFromFormat("Error creating SDL window: %s", SDL_GetError());
				}
				return false;
			}
			if (x != SDL_WINDOWPOS_UNDEFINED && y != SDL_WINDOWPOS_UNDEFINED) {
				SDL_SetWindowPosition(window, x, y);
			}

			// Overwrite the surface init params with what we need for Vulkan..
			if (!DetermineVulkanWindowSystem(window, &windowDesc, errorMessage)) {
				return false;
			}
			// NOTE : This should match the lines below in the Vulkan case.
			ctx = new VulkanGraphicsContext();
		}

		if (!ctx->InitAPI(nullptr, &g_Config.sVulkanDevice, errorMessage)) {
			fprintf(stderr, "Graphics initialization failed: %s\n", errorMessage->c_str());
			delete ctx;
			if (window) {
				SDL_DestroyWindow(window);
				window = nullptr;
			}
			return false;
		}

		if (backend == GPUBackend::VULKAN) {
			// linux wayland can give -1x-1 during vkGetPhysicalDeviceSurfaceCapabilitiesKHR
			VulkanGraphicsContext *vkgfxctx = (VulkanGraphicsContext *)ctx;
			VulkanContext *vkctx = (VulkanContext *)vkgfxctx->GetAPIContext();
			vkctx->SetCbGetDrawSize([window]() {
				int w=1,h=1;
#if PPSSPP_PLATFORM(SWITCH)
				u32 width = 1, height = 1;
				if (R_SUCCEEDED(nwindowGetDimensions(nwindowGetDefault(), &width, &height))) {
					w = (int)width;
					h = (int)height;
				}
#else
				SDL_GetWindowSizeInPixels(window, &w, &h);
#endif
				return VkExtent2D {(uint32_t)w, (uint32_t)h};
			});
		}

		if (!ctx->InitSurface(windowDesc.winsys, windowDesc.data1, windowDesc.data2, errorMessage)) {
			fprintf(stderr, "Surface creation failed: %s\n", errorMessage->c_str());
			ctx->ShutdownAPI();
			delete ctx;
			if (window) {
				SDL_DestroyWindow(window);
				window = nullptr;
			}
			return false;
		}

		*graphicsContext = ctx;
		return true;
	};

	GraphicsContext *graphicsContext = nullptr;
	std::string error_message;
	// printf (stdout) rather than stderr: on Switch only stdout reaches the SD card log.
	printf("[backend] trying %s\n", GPUBackendToString((GPUBackend)g_Config.iGPUBackend).c_str());
	if (!initializeBackend((GPUBackend)g_Config.iGPUBackend, &graphicsContext, &error_message)) {
		printf("[backend] FAILED %s: %s\n", GPUBackendToString((GPUBackend)g_Config.iGPUBackend).c_str(), error_message.c_str());
		fprintf(stderr, "Failed to initialize graphics backend: %s\n", error_message.c_str());
		if (fallbackGPUBackend != -1) {
		printf("[backend] falling back to %s\n", GPUBackendToString((GPUBackend)fallbackGPUBackend).c_str());
		fprintf(stderr, "Attempting to fall back to %s...\n", GPUBackendToString((GPUBackend)fallbackGPUBackend).c_str());
			g_Config.iGPUBackend = fallbackGPUBackend;
			error_message.clear();
			if (!initializeBackend((GPUBackend)g_Config.iGPUBackend, &graphicsContext, &error_message)) {
				printf("[backend] FALLBACK FAILED: %s\n", error_message.c_str());
				fprintf(stderr, "Fallback failed: %s\n", error_message.c_str());
				SDL_Quit();
				return 1;
			}
		} else {
			fprintf(stderr, "No fallback GPU backend available. Exiting.\n");
			SDL_Quit();
			return 1;
		}
	}

	// At this point, we have a window that we can show finally.
	printf("[backend] initialized %s\n", GPUBackendToString((GPUBackend)g_Config.iGPUBackend).c_str());
	SDL_ShowWindow(window);
	if (x != SDL_WINDOWPOS_UNDEFINED && y != SDL_WINDOWPOS_UNDEFINED) {
		SDL_SetWindowPosition(window, x, y);
	}
	UpdateScreenDPI(window);

	// Initialize g_display synchronously before starting the EmuThread below, since it renders
	// immediately without waiting for us to process an initial SDL resize/scale event -
	// otherwise the first frames can render with dp_xres/dp_yres still at their defaults.
	int initialPixelWidth = 0, initialPixelHeight = 0;
	SDL_GetWindowSizeInPixels(window, &initialPixelWidth, &initialPixelHeight);
	Native_UpdateScreenScale(initialPixelWidth, initialPixelHeight, UIScaleFactorToMultiplier(g_Config.iUIScaleFactor));

	SDL_SetWindowTitle(window, (app_name_nice + " " + PPSSPP_GIT_VERSION).c_str());

	if (cmdLineOptions.gameWindow.value_or(false)) {
		CreateGameWindow();
	}

	char iconPath[PATH_MAX];
#if defined(ASSETS_DIR)
	snprintf(iconPath, PATH_MAX, "%sui_images/icon.png", ASSETS_DIR);
	if (access(iconPath, F_OK) != 0)
		snprintf(iconPath, PATH_MAX, "%sassets/ui_images/icon.png", SDL_GetBasePath() ? SDL_GetBasePath() : "");
#else
	snprintf(iconPath, PATH_MAX, "%sassets/ui_images/icon.png", SDL_GetBasePath() ? SDL_GetBasePath() : "");
#endif
	int width = 0, height = 0;
	unsigned char *imageData;
	if (pngLoad(iconPath, &width, &height, &imageData) == 1) {
		SDL_Surface *surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
		if (surface) {
			if (surface->pitch == width * 4) {
				memcpy(surface->pixels, imageData, width * height * 4);
			} else {
				for (int y = 0; y < height; ++y) {
					memcpy((uint8_t *)surface->pixels + y * surface->pitch, imageData + y * width * 4, width * 4);
				}
			}
			SDL_SetWindowIcon(window, surface);
			SDL_DestroySurface(surface);
		}
		free(imageData);
		imageData = NULL;
	}

	// OK, we have a valid graphics backend selected. Let's clear the failures.
	g_Config.sFailedGPUBackends.clear();

#ifdef MOBILE_DEVICE
	SDL_HideCursor();
#endif

	// Avoid the IME popup when holding keys. This doesn't affect all versions of SDL.
	// Note: We re-enable it in text input fields! This is necessary otherwise we don't receive
	// KeyInputFlags::CHAR events.
	SDL_StopTextInput(window);

	InitSDLAudioDevice();
	g_audioStartTime = time_now_d();

	if (joystick_enabled) {
		joystick = new SDLJoystick();
	} else {
		joystick = nullptr;
	}
	EnableFZ();

	// We use the emuthread both for OpenGL and Vulkan, but in OpenGL mode we also render from the main thread.
	_dbg_assert_(graphicsContext);

	InputStateTracker inputTracker{};

#if PPSSPP_PLATFORM(MAC)
	// setup menu items for macOS
	initializeOSXExtras();
#endif

	// Check if the path to a directory containing an unpacked ISO is passed as a command line argument
	for (int i = 1; i < argc; i++) {
		if (File::IsDirectory(Path(argv[i]))) {
			// Display the toast warning
			break;
		}
	}

#if !PPSSPP_PLATFORM(MAC)
	// linux SDL_GetPowerInfo asks upower on dbus for power information by default, can cause DrawFPS() in DebugOverlay.cpp to block and affect frame pacing
	bool stop_battery_poll_thread = false;
	double last_battery_poll = 0;
	SDL_Semaphore *battery_poll_thread_sema = SDL_CreateSemaphore(0); // c++20 and onwards has semaphore

	std::thread battery_poll_thread([&battery_poll_thread_sema, &stop_battery_poll_thread] {
		SetCurrentThreadAffinity(ThreadAffinityRole::BACKGROUND);
		while (!stop_battery_poll_thread) {
			SDL_WaitSemaphore(battery_poll_thread_sema);
			SDL_GetPowerInfo(nullptr, &g_batteryPercent);
		}
	});

	// Always polled: the status overlay shows it, and so does the HOME screen.
	auto kick_battery_poll_thread = [&last_battery_poll, &battery_poll_thread_sema] {
		double now = time_now_d();
		if (now - last_battery_poll >= 5.0) {
			last_battery_poll = now;
			SDL_SignalSemaphore(battery_poll_thread_sema);
		}
	};

	kick_battery_poll_thread();
#endif

	const bool needsSeparateEmuThread = graphicsContext->NeedsSeparateEmuThread();
	if (!needsSeparateEmuThread) {
		// Vulkan mode uses this.

		std::thread emuThread = std::thread([&] {
			RunMainLoop(graphicsContext, new NativeApplication(), [&](GraphicsContext *graphicsContext) {
				NativeFrame(graphicsContext);
				bool keepRunning = !(g_QuitRequested || g_RestartRequested);
				if (!keepRunning) {
					INFO_LOG(Log::System, "EmuThread was requested to exit normally.");
				}
				return keepRunning;
			});
		});
		SetCurrentThreadAffinity(ThreadAffinityRole::EVENT);

		// The SDL main thread only becomes a plain message pump. This allows for lower latency
		// input events, and so on. The spawned main thread runs emulation and rendering.
		while (true) {
#if PPSSPP_PLATFORM(SWITCH)
			CheckSwitchOperationMode();
#endif
			SDL_Event event;
			if (SDL_WaitEventTimeout(&event, 100)) {
				do {
					ProcessSDLEvent(window, event, &inputTracker);

					if (g_QuitRequested || g_RestartRequested)
						break;
				} while (SDL_PollEvent(&event));
			}

			if (g_QuitRequested || g_RestartRequested)
				break;

			UpdateTextFocus(window);
			UpdateSDLCursor();
#if !PPSSPP_PLATFORM(MAC)
			kick_battery_poll_thread();
#endif

			inputTracker.MouseCaptureControl(window);

			{
				std::lock_guard<std::mutex> guard(g_mutexWindow);
				if (g_windowState.update) {
					UpdateWindowState(window);
				}
			}
		}
		INFO_LOG(Log::System, "Joining main thread...");
		emuThread.join();
	} else {
		// OpenGL mode uses this path.
		SetCurrentThreadAffinity(ThreadAffinityRole::RENDER);
		std::thread emuThread = EmuThread_Start(graphicsContext, new NativeApplication(), [&](GraphicsContext *graphicsContext){
			NativeFrame(graphicsContext);
			return true;
		});
		while (true) {
#if PPSSPP_PLATFORM(SWITCH)
			CheckSwitchOperationMode();
#endif
			// OpenGL mode uses this.
			{
				SDL_Event event;
				while (SDL_PollEvent(&event)) {
					ProcessSDLEvent(window, event, &inputTracker);
				}
			}
			if (g_QuitRequested || g_RestartRequested)
				break;

			UpdateTextFocus(window);
			UpdateSDLCursor();
#if !PPSSPP_PLATFORM(MAC)
			kick_battery_poll_thread();
#endif

			inputTracker.MouseCaptureControl(window);

			bool renderThreadPaused = Native_IsWindowHidden() && g_Config.bPauseWhenMinimized;
			if (graphicsContext->NeedsSeparateEmuThread() && !renderThreadPaused) {
				if (!graphicsContext->ThreadFrame()) {
					// The render thread was instructed to exit by the emu thread,
					// and has now reached the end of the submitted frames.
					// EmuThread will be in the process of exiting, so below
					// we can just EmuThread_Join().
					break;
				}
			}

			{
				std::lock_guard<std::mutex> guard(g_mutexWindow);
				if (g_windowState.update) {
					UpdateWindowState(window);
				}
			}
		}
		INFO_LOG(Log::System, "Requesting render thread exit...");
		EXIT_TRACE("emu thread join: begin");
		EmuThread_Join(graphicsContext, emuThread);
		EXIT_TRACE("emu thread join: done");
	}

#if !PPSSPP_PLATFORM(MAC)
	stop_battery_poll_thread = true;
	SDL_SignalSemaphore(battery_poll_thread_sema);
	battery_poll_thread.join();
	SDL_DestroySemaphore(battery_poll_thread_sema);
	EXIT_TRACE("battery poll thread joined");
#endif

	delete joystick;
	EXIT_TRACE("joystick deleted");

	// Destroys Draw, which is used in NativeShutdown to shutdown.
	EXIT_TRACE("ShutdownSurface: begin");
	graphicsContext->ShutdownSurface();
	EXIT_TRACE("ShutdownSurface: done");
	EXIT_TRACE("ShutdownAPI: begin");
	graphicsContext->ShutdownAPI();
	EXIT_TRACE("ShutdownAPI: done");
	delete graphicsContext;
	EXIT_TRACE("graphicsContext deleted");
	EXIT_TRACE("SDL_DestroyWindow: begin");
	SDL_DestroyWindow(window);
	window = nullptr;
	EXIT_TRACE("SDL_DestroyWindow: done");

	EXIT_TRACE("NativeShutdown: begin");
	NativeShutdown();
	EXIT_TRACE("NativeShutdown: done");

#if PPSSPP_PLATFORM(SWITCH)
	EXIT_TRACE("SwitchLibrary::Shutdown: begin");
	SwitchLibrary::Shutdown();
	EXIT_TRACE("SwitchLibrary::Shutdown: done");
#endif


	for (int i = 0; i < SDL_SYSTEM_CURSOR_COUNT; ++i) {
		if (g_builtinCursors[i]) {
			SDL_DestroyCursor(g_builtinCursors[i]);
			g_builtinCursors[i] = nullptr;
		}
	}
	EXIT_TRACE("cursors destroyed");

	StopSDLAudioDevice();
	EXIT_TRACE("audio device stopped");
	SDL_Quit();
	EXIT_TRACE("SDL_Quit: done");
#if PPSSPP_PLATFORM(RPI)
	bcm_host_deinit();
#endif

	EXIT_TRACE("glslang::FinalizeProcess: begin");
	glslang::FinalizeProcess();
	EXIT_TRACE("glslang::FinalizeProcess: done");
	fprintf(stderr, "Leaving main\n");
#ifdef HAVE_LIBNX
	if (g_nxlinkSocket >= 0) {
		close(g_nxlinkSocket);
		g_nxlinkSocket = -1;
	}
	if (g_socketInitialized) {
		socketExit();
		g_socketInitialized = false;
	}
	EXIT_TRACE("network sockets closed");
	romfsExit();
	EXIT_TRACE("romfsExit: done");
#endif

#if PPSSPP_PLATFORM(SWITCH)
	if (g_RestartRequested) {
		EXIT_TRACE("restart requested: writing handoff");
		bool handoffReady = g_restartParams.empty() || SwitchWriteRestartHandoff(g_restartParams);
		if (handoffReady && (__nx_applet_type == AppletType_Application || __nx_applet_type == AppletType_SystemApplication)) {
			appletRestartProgram(nullptr, 0);
			remove(SwitchRestartHandoffPath().c_str());
		}
		std::string restartArgs = g_switchExecutablePath;
		if (!g_restartParams.empty()) {
			restartArgs += " ";
			restartArgs += g_restartParams;
		}
		Result rc = envSetNextLoad(g_switchExecutablePath.c_str(), restartArgs.c_str());
		printf("[exit] restart: envSetNextLoad -> 0x%08x\n", rc);
		if (R_SUCCEEDED(rc)) {
			return 0;
		}
	}
#endif

	// If a restart was requested (and supported on this platform), respawn the executable.
	if (g_RestartRequested) {
#if PPSSPP_PLATFORM(MAC)
		RestartMacApp();
#elif PPSSPP_PLATFORM(LINUX)
		// Hackery from https://unix.stackexchange.com/questions/207935/how-to-restart-or-reset-a-running-process-in-linux,
		char *exec_argv[] = { argv[0], nullptr };
		execv("/proc/self/exe", exec_argv);
#endif
	}
	// Everything from here on happens after main() returns: static destructors, then libnx's
	// __appExit()/__nx_win_exit(). Both are traced - see SwitchExitStage and the atexit hook
	// registered at the top of main.
	EXIT_TRACE("returning from main: static destructors and libnx exit path are next");
	return 0;
}

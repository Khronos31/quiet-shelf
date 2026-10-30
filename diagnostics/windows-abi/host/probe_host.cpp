// Host-side probe for the Windows BonDriver ABI.
//
// Loads the DLL with LoadLibrary and reaches it only through the two public
// entry points, using an independent copy of the host declarations.  Each
// stage is printed and flushed before the call so a crash still identifies the
// step that failed.  A real host calls Release on the base interface even when
// dynamic_cast to IBonDriver2 fails, so that path is exercised too.
#include "host_abi.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#define PROBE_STRINGIZE2(x) #x
#define PROBE_STRINGIZE(x) PROBE_STRINGIZE2(x)

namespace {

int g_failures = 0;

// EDCB BonCtrl and LibISDB both use 0xFFFFFFFF for "no current channel".
constexpr DWORD kSpaceInvalid = 0xFFFFFFFFu;
constexpr DWORD kChannelInvalid = 0xFFFFFFFFu;

using CreateDriverFn = IBonDriver *(*)();
using CreateStructFn = const STRUCT_IBONDRIVER *(*)();
using BuildInfoFn = const char *(*)();

void stage(const char *name)
{
	std::printf("STAGE %s\n", name);
	std::fflush(stdout);
}

void check(bool cond, const std::string &name)
{
	if (cond) {
		std::printf("CHECK %s ok\n", name.c_str());
	} else {
		std::printf("CHECK %s FAIL\n", name.c_str());
		++g_failures;
	}
	std::fflush(stdout);
}

bool u16Equals(const BON16CHAR *actual, const char *ascii)
{
	if (actual == nullptr) {
		return false;
	}
	for (std::size_t i = 0;; ++i) {
		const unsigned int expected = static_cast<unsigned char>(ascii[i]);
		if (static_cast<unsigned int>(actual[i]) != expected) {
			return false;
		}
		if (expected == 0) {
			return true;
		}
	}
}

bool markerContains(const char *needle)
{
	const char *path = std::getenv("ABI_PROBE_MARKER");
	if (path == nullptr || *path == '\0') {
		return false;
	}
	std::FILE *file = std::fopen(path, "rb");
	if (file == nullptr) {
		return false;
	}
	std::string text;
	char buffer[256];
	std::size_t n;
	while ((n = std::fread(buffer, 1, sizeof buffer, file)) != 0) {
		text.append(buffer, n);
	}
	std::fclose(file);
	return text.find(needle) != std::string::npos;
}

std::string hostBuildInfo()
{
	std::string info =
#if defined(_MSC_VER)
		"compiler=MSVC " PROBE_STRINGIZE(_MSC_VER)
#elif defined(__clang__)
		"compiler=clang " __clang_version__
#elif defined(__GNUC__)
		"compiler=GCC " __VERSION__
#else
		"compiler=unknown"
#endif
#if defined(_MSC_VER)
#  if defined(_DLL)
		";crt=msvc-dynamic"
#  else
		";crt=msvc-static"
#  endif
#elif defined(__MINGW32__)
#  if defined(_UCRT)
		";crt=ucrt"
#  else
		";crt=msvcrt"
#  endif
#else
		";crt=unknown"
#endif
#if defined(_ITERATOR_DEBUG_LEVEL)
		";iterator_debug_level=" PROBE_STRINGIZE(_ITERATOR_DEBUG_LEVEL)
#endif
#ifdef __MSVCRT_VERSION__
		";msvcrt_version=" PROBE_STRINGIZE(__MSVCRT_VERSION__)
#endif
		;
	return info;
}

void *libOpen(const char *path)
{
	return reinterpret_cast<void *>(LoadLibraryA(path));
}

void *libSym(void *handle, const char *name)
{
	return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(handle), name));
}

void logDllBuildInfo(void *handle)
{
	auto info = reinterpret_cast<BuildInfoFn>(libSym(handle, "ProbeDriverBuildInfo"));
	if (info != nullptr) {
		std::printf("BUILD dll %s\n", info());
	} else {
		std::printf("BUILD dll unavailable\n");
	}
	std::fflush(stdout);
}

void logLoadFailure(void *handle)
{
	std::printf("LOAD_ERROR code=%lu\n",
	            static_cast<unsigned long>(GetLastError()));
	std::fflush(stdout);
	(void)handle;
}

int runDirect(const char *dllPath)
{
	stage("load_library");
	void *handle = libOpen(dllPath);
	if (handle == nullptr) {
		logLoadFailure(handle);
		std::printf("RESULT direct load_failed\n");
		return 2;
	}
	logDllBuildInfo(handle);

	stage("get_create_driver");
	auto create = reinterpret_cast<CreateDriverFn>(libSym(handle, "CreateBonDriver"));
	if (create == nullptr) {
		std::printf("RESULT direct missing_create\n");
		return 2;
	}

	stage("create_driver");
	IBonDriver *base = create();
	if (base == nullptr) {
		std::printf("RESULT direct create_null\n");
		return 2;
	}

	stage("cast_ibondriver2");
	IBonDriver2 *driver2 = dynamic_cast<IBonDriver2 *>(base);
	std::printf("CAST ibondriver2 %s\n", driver2 != nullptr ? "ok" : "null");
	std::fflush(stdout);

	if (driver2 == nullptr) {
		// LibISDB and EDCB still release the base interface when the optional
		// IBonDriver2 view is unavailable.
		stage("base_open_after_cast_null");
		check(base->OpenTuner() == TRUE, "base OpenTuner after cast null");
		stage("release_after_cast_null");
		base->Release();
		check(markerContains("release"), "base Release after cast null");
		std::printf("RESULT direct cast_null release=%s\n",
		            markerContains("release") ? "ok" : "absent");
		FreeLibrary(static_cast<HMODULE>(handle));
		return 3;
	}

	stage("open_tuner");
	check(base->OpenTuner() == TRUE, "OpenTuner");

	stage("get_tuner_name");
	check(u16Equals(driver2->GetTunerName(), "ABI Probe Tuner"), "GetTunerName UTF-16");

	stage("is_tuner_opening");
	check(driver2->IsTunerOpening() == FALSE, "IsTunerOpening");

	stage("enum_space");
	check(u16Equals(driver2->EnumTuningSpace(0), "GR"), "EnumTuningSpace(0)");
	check(driver2->EnumTuningSpace(1) == nullptr, "EnumTuningSpace(1) null");

	stage("enum_channel");
	check(u16Equals(driver2->EnumChannelName(0, 0), "T13"), "EnumChannelName(0,0)");
	check(driver2->EnumChannelName(0, 1) == nullptr, "EnumChannelName(0,1) null");

	stage("get_cur_before_set");
	check(driver2->GetCurSpace() == kSpaceInvalid, "GetCurSpace invalid before set");
	check(driver2->GetCurChannel() == kChannelInvalid, "GetCurChannel invalid before set");

	stage("set_channel_byte");
	check(base->SetChannel(static_cast<BYTE>(13)) == TRUE, "SetChannel(BYTE)");
	check(driver2->GetCurSpace() == 0, "GetCurSpace after SetChannel(BYTE)");
	check(driver2->GetCurChannel() == 13, "GetCurChannel after SetChannel(BYTE)");

	stage("set_channel_2");
	check(driver2->SetChannel(0, 0) == TRUE, "SetChannel(DWORD,DWORD)");
	check(driver2->GetCurSpace() == 0, "GetCurSpace after SetChannel(2)");
	check(driver2->GetCurChannel() == 0, "GetCurChannel after SetChannel(2)");

	stage("signal");
	check(base->GetSignalLevel() == 42.5f, "GetSignalLevel");

	stage("wait_ts");
	check(base->WaitTsStream(100) == 0, "WaitTsStream");

	stage("ready");
	check(base->GetReadyCount() == 0, "GetReadyCount");

	stage("get_ts_copy");
	{
		BYTE buffer[1] = {};
		DWORD size = 1;
		DWORD remain = 1;
		check(base->GetTsStream(buffer, &size, &remain) == FALSE, "GetTsStream(BYTE*)");
		check(size == 0 && remain == 0, "GetTsStream(BYTE*) outputs");
	}

	stage("get_ts_ptr");
	{
		DWORD sentinel = 0;
		BYTE *buffer = reinterpret_cast<BYTE *>(&sentinel);
		DWORD size = 1;
		DWORD remain = 1;
		check(base->GetTsStream(&buffer, &size, &remain) == FALSE, "GetTsStream(BYTE**)");
		check(buffer == nullptr && size == 0 && remain == 0, "GetTsStream(BYTE**) outputs");
	}

	stage("purge");
	base->PurgeTsStream();

	stage("close_tuner");
	base->CloseTuner();

	stage("release");
	base->Release();
	check(markerContains("release"), "Release marker");

	stage("done");
	std::printf("RESULT direct pass failures=%d\n", g_failures);
	FreeLibrary(static_cast<HMODULE>(handle));
	return g_failures == 0 ? 0 : 4;
}

int runStruct(const char *dllPath)
{
	stage("load_library");
	void *handle = libOpen(dllPath);
	if (handle == nullptr) {
		logLoadFailure(handle);
		std::printf("RESULT struct load_failed\n");
		return 2;
	}
	logDllBuildInfo(handle);

	stage("get_create_struct");
	auto create = reinterpret_cast<CreateStructFn>(libSym(handle, "CreateBonStruct"));
	if (create == nullptr) {
		std::printf("RESULT struct missing_create\n");
		return 2;
	}

	stage("create_struct");
	const STRUCT_IBONDRIVER *st = create();
	if (st == nullptr) {
		std::printf("RESULT struct create_null\n");
		return 2;
	}

	stage("struct_layout");
	const auto *st2 = reinterpret_cast<const STRUCT_IBONDRIVER2 *>(st);
	const std::ptrdiff_t span =
		static_cast<const char *>(st->pEnd) - reinterpret_cast<const char *>(st2);
	check(span >= static_cast<std::ptrdiff_t>(sizeof(STRUCT_IBONDRIVER2)), "pEnd covers IBonDriver2");
	if (span < static_cast<std::ptrdiff_t>(sizeof(STRUCT_IBONDRIVER2))) {
		std::printf("RESULT struct short_struct\n");
		return 4;
	}

	void *ctx = st->pCtx;
	check(ctx != nullptr, "struct pCtx");

	stage("struct_open");
	check(st2->st.pF00(ctx) == TRUE, "struct OpenTuner");

	stage("struct_name");
	check(u16Equals(st2->pF10(ctx), "ABI Probe Tuner"), "struct GetTunerName");

	stage("struct_opening");
	check(st2->pF11(ctx) == FALSE, "struct IsTunerOpening");

	stage("struct_space");
	check(u16Equals(st2->pF12(ctx, 0), "GR"), "struct EnumTuningSpace(0)");
	check(st2->pF12(ctx, 1) == nullptr, "struct EnumTuningSpace(1) null");

	stage("struct_channel");
	check(u16Equals(st2->pF13(ctx, 0, 0), "T13"), "struct EnumChannelName(0,0)");
	check(st2->pF13(ctx, 0, 1) == nullptr, "struct EnumChannelName(0,1) null");

	stage("struct_cur_before_set");
	check(st2->pF15(ctx) == kSpaceInvalid, "struct GetCurSpace invalid");
	check(st2->pF16(ctx) == kChannelInvalid, "struct GetCurChannel invalid");

	stage("struct_set_byte");
	check(st2->st.pF02(ctx, static_cast<BYTE>(13)) == TRUE, "struct SetChannel(BYTE)");
	check(st2->pF15(ctx) == 0, "struct GetCurSpace after SetChannel(BYTE)");
	check(st2->pF16(ctx) == 13, "struct GetCurChannel after SetChannel(BYTE)");

	stage("struct_set_2");
	check(st2->pF14(ctx, 0, 0) == TRUE, "struct SetChannel(DWORD,DWORD)");
	check(st2->pF15(ctx) == 0, "struct GetCurSpace after SetChannel(2)");
	check(st2->pF16(ctx) == 0, "struct GetCurChannel after SetChannel(2)");

	stage("struct_signal");
	check(st2->st.pF03(ctx) == 42.5f, "struct GetSignalLevel");

	stage("struct_wait");
	check(st2->st.pF04(ctx, 100) == 0, "struct WaitTsStream");

	stage("struct_ready");
	check(st2->st.pF05(ctx) == 0, "struct GetReadyCount");

	stage("struct_get_ts_copy");
	{
		BYTE buffer[1] = {};
		DWORD size = 1;
		DWORD remain = 1;
		check(st2->st.pF06(ctx, buffer, &size, &remain) == FALSE, "struct GetTsStream(BYTE*)");
		check(size == 0 && remain == 0, "struct GetTsStream(BYTE*) outputs");
	}

	stage("struct_get_ts_ptr");
	{
		DWORD sentinel = 0;
		BYTE *buffer = reinterpret_cast<BYTE *>(&sentinel);
		DWORD size = 1;
		DWORD remain = 1;
		check(st2->st.pF07(ctx, &buffer, &size, &remain) == FALSE, "struct GetTsStream(BYTE**)");
		check(buffer == nullptr && size == 0 && remain == 0, "struct GetTsStream(BYTE**) outputs");
	}

	stage("struct_purge");
	st2->st.pF08(ctx);

	stage("struct_close");
	st2->st.pF01(ctx);

	stage("struct_release");
	st2->st.pF09(ctx);
	check(markerContains("release"), "struct Release marker");

	stage("done");
	std::printf("RESULT struct pass failures=%d\n", g_failures);
	FreeLibrary(static_cast<HMODULE>(handle));
	return g_failures == 0 ? 0 : 4;
}

} // namespace

int main(int argc, char **argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("BUILD host %s\n", hostBuildInfo().c_str());
	std::fflush(stdout);
	if (argc != 3) {
		std::printf("usage: %s direct|struct <BonDriver.dll>\n", argv[0]);
		return 1;
	}
	try {
		if (std::strcmp(argv[1], "direct") == 0) {
			return runDirect(argv[2]);
		}
		if (std::strcmp(argv[1], "struct") == 0) {
			return runStruct(argv[2]);
		}
		std::printf("RESULT unknown_mode\n");
		return 1;
	} catch (const std::exception &e) {
		std::printf("EXCEPTION %s\n", e.what());
		return 5;
	} catch (...) {
		std::printf("EXCEPTION unknown\n");
		return 5;
	}
}

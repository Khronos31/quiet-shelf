// Minimal device-free BonDriver used by the Windows ABI probe.
//
// The class implements the production IBonDriver2 declaration from
// abi/bondriver_abi.h and exports the same CreateBonDriver / CreateBonStruct
// entry points as the real library.  Return values are fixed and deterministic
// so a host can verify which vtable slot or struct slot it reached.
#include "bondriver_abi.h"

#include <cstdio>
#include <cstdlib>
#include <new>

#define PROBE_STRINGIZE2(x) #x
#define PROBE_STRINGIZE(x) PROBE_STRINGIZE2(x)

namespace {

void writeMarker(const char *tag)
{
	const char *path = std::getenv("ABI_PROBE_MARKER");
	if (path == nullptr || *path == '\0') {
		return;
	}
	std::FILE *file = std::fopen(path, "ab");
	if (file == nullptr) {
		return;
	}
	std::fprintf(file, "%s\n", tag);
	std::fclose(file);
}

class FakeDriver final : public IBonDriver2
{
public:
	FakeDriver() = default;

	const BOOL OpenTuner(void) override
	{
		opened_ = true;
		return TRUE;
	}

	void CloseTuner(void) override
	{
		opened_ = false;
	}

	const BOOL SetChannel(const BYTE bCh) override
	{
		space_ = 0;
		channel_ = bCh;
		return TRUE;
	}

	const float GetSignalLevel(void) override
	{
		return 42.5f;
	}

	const DWORD WaitTsStream(const DWORD) override
	{
		return 0;
	}

	const DWORD GetReadyCount(void) override
	{
		return 0;
	}

	const BOOL GetTsStream(BYTE *, DWORD *pdwSize, DWORD *pdwRemain) override
	{
		if (pdwSize != nullptr) {
			*pdwSize = 0;
		}
		if (pdwRemain != nullptr) {
			*pdwRemain = 0;
		}
		return FALSE;
	}

	const BOOL GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain) override
	{
		if (ppDst != nullptr) {
			*ppDst = nullptr;
		}
		if (pdwSize != nullptr) {
			*pdwSize = 0;
		}
		if (pdwRemain != nullptr) {
			*pdwRemain = 0;
		}
		return FALSE;
	}

	void PurgeTsStream(void) override
	{
	}

	void Release(void) override
	{
		writeMarker("release");
		delete this;
	}

	const BON16CHAR *GetTunerName(void) override
	{
		return L"ABI Probe Tuner";
	}

	const BOOL IsTunerOpening(void) override
	{
		return FALSE;
	}

	const BON16CHAR *EnumTuningSpace(const DWORD dwSpace) override
	{
		return dwSpace == 0 ? L"GR" : nullptr;
	}

	const BON16CHAR *EnumChannelName(const DWORD dwSpace, const DWORD dwChannel) override
	{
		return (dwSpace == 0 && dwChannel == 0) ? L"T13" : nullptr;
	}

	const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel) override
	{
		space_ = dwSpace;
		channel_ = dwChannel;
		return TRUE;
	}

	const DWORD GetCurSpace(void) override
	{
		return space_;
	}

	const DWORD GetCurChannel(void) override
	{
		return channel_;
	}

private:
	bool opened_ = false;
	DWORD space_ = BONDRIVER_SPACE_INVALID;
	DWORD channel_ = BONDRIVER_CHANNEL_INVALID;
};

struct StructHolder
{
	STRUCT_IBONDRIVER2 st2;
	IBonDriver2 *driver = nullptr;
	bool active = false;
};

StructHolder g_holder;

BOOL stOpen(void *p) { return static_cast<IBonDriver *>(p)->OpenTuner(); }
void stClose(void *p) { static_cast<IBonDriver *>(p)->CloseTuner(); }
BOOL stSetByte(void *p, BYTE b) { return static_cast<IBonDriver *>(p)->SetChannel(b); }
float stSignal(void *p) { return static_cast<IBonDriver *>(p)->GetSignalLevel(); }
DWORD stWait(void *p, DWORD t) { return static_cast<IBonDriver *>(p)->WaitTsStream(t); }
DWORD stReady(void *p) { return static_cast<IBonDriver *>(p)->GetReadyCount(); }
BOOL stCopy(void *p, BYTE *d, DWORD *s, DWORD *r) { return static_cast<IBonDriver *>(p)->GetTsStream(d, s, r); }
BOOL stPtr(void *p, BYTE **d, DWORD *s, DWORD *r) { return static_cast<IBonDriver *>(p)->GetTsStream(d, s, r); }
void stPurge(void *p) { static_cast<IBonDriver *>(p)->PurgeTsStream(); }
void stRelease(void *p)
{
	static_cast<IBonDriver *>(p)->Release();
	g_holder.driver = nullptr;
	g_holder.active = false;
}
const BON16CHAR *stName(void *p)
{
	return static_cast<IBonDriver2 *>(static_cast<IBonDriver *>(p))->GetTunerName();
}
BOOL stOpening(void *p)
{
	return static_cast<IBonDriver2 *>(static_cast<IBonDriver *>(p))->IsTunerOpening();
}
const BON16CHAR *stSpace(void *p, DWORD s)
{
	return static_cast<IBonDriver2 *>(static_cast<IBonDriver *>(p))->EnumTuningSpace(s);
}
const BON16CHAR *stChannel(void *p, DWORD s, DWORD c)
{
	return static_cast<IBonDriver2 *>(static_cast<IBonDriver *>(p))->EnumChannelName(s, c);
}
BOOL stSet2(void *p, DWORD s, DWORD c)
{
	return static_cast<IBonDriver2 *>(static_cast<IBonDriver *>(p))->SetChannel(s, c);
}
DWORD stCurSpace(void *p)
{
	return static_cast<IBonDriver2 *>(static_cast<IBonDriver *>(p))->GetCurSpace();
}
DWORD stCurChannel(void *p)
{
	return static_cast<IBonDriver2 *>(static_cast<IBonDriver *>(p))->GetCurChannel();
}

void structFill(STRUCT_IBONDRIVER2 &st2, IBonDriver2 *core, const void *end)
{
	STRUCT_IBONDRIVER &base = st2.st;
	base.pCtx = core;
	base.pEnd = end != nullptr ? end : &st2 + 1;
	base.pF00 = stOpen;
	base.pF01 = stClose;
	base.pF02 = stSetByte;
	base.pF03 = stSignal;
	base.pF04 = stWait;
	base.pF05 = stReady;
	base.pF06 = stCopy;
	base.pF07 = stPtr;
	base.pF08 = stPurge;
	base.pF09 = stRelease;
	st2.pF10 = stName;
	st2.pF11 = stOpening;
	st2.pF12 = stSpace;
	st2.pF13 = stChannel;
	st2.pF14 = stSet2;
	st2.pF15 = stCurSpace;
	st2.pF16 = stCurChannel;
}

} // namespace

// Diagnostic-only export; it is not part of the BonDriver contract.  It lets
// the host log which compiler and CRT produced the DLL.
extern "C" __declspec(dllexport) const char *ProbeDriverBuildInfo()
{
	static const char kInfo[] =
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
	return kInfo;
}

BONDRIVER_API IBonDriver2 *CreateBonDriver()
{
	return new (std::nothrow) FakeDriver();
}

BONDRIVER_API const STRUCT_IBONDRIVER *CreateBonStruct()
{
	if (g_holder.active) {
		return &g_holder.st2.st;
	}
	auto *driver = new (std::nothrow) FakeDriver();
	if (driver == nullptr) {
		return nullptr;
	}
	g_holder.st2 = STRUCT_IBONDRIVER2{};
	structFill(g_holder.st2, driver, &g_holder.st2 + 1);
	g_holder.st2.st.pF09 = stRelease;
	g_holder.driver = driver;
	g_holder.active = true;
	return &g_holder.st2.st;
}

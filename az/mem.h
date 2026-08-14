#pragma once
#include <vector>
#include <string>
#include <windows.h>

namespace mem
{
	static void read(uintptr_t addr, void* buf, size_t len) {
		if (!IsBadReadPtr(reinterpret_cast<void*>(addr), len)) {
			memcpy(buf, reinterpret_cast<void*>(addr), len);
		}
	}

	static void write(uintptr_t addr, void* buf, size_t len) {
		auto old = 0ul;
		if (VirtualProtect(reinterpret_cast<void*>(addr), len, PAGE_EXECUTE_READWRITE, &old))
		{
			memcpy(reinterpret_cast<void*>(addr), buf, len);
			FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), len);
			VirtualProtect(reinterpret_cast<void*>(addr), len, old, &old);
		}
	}

	template <class T>
	void write(uintptr_t addr, T o)
	{
		write(addr, reinterpret_cast<void*>(&o), sizeof(T));
	}

	template <class T>
	T read(uintptr_t addr) {
		T o;
		read(addr, reinterpret_cast<void*>(&o), sizeof(o));
		return o;
	}

	template <class CharT>
	std::basic_string<CharT> read_str(uintptr_t addr, size_t max_length = 256) {
		std::basic_string<CharT> str(max_length, CharT());
		read(addr, reinterpret_cast<void*>(&str[0]), sizeof(CharT) * max_length);
		return str;
	}

	static void patch(uintptr_t addr, const std::vector<uint8_t>& bytes)
	{
		write(addr, (void*)bytes.data(), bytes.size());
	}

	static void jmphook(uintptr_t addr, uintptr_t proxy)
	{
#ifdef _WIN64
		mem::write(addr, 0xB848ui16);
		mem::write(addr + 2, proxy);
		mem::write(addr + 10, 0xE0FFui16);
#else
		mem::write(addr, 0xE9ui8);
		mem::write(addr + 1, (proxy - addr - 5));
#endif // _WIN64
	}

	static void callhook(uintptr_t addr, uintptr_t proxy)
	{
#ifdef _WIN64
		mem::write(addr, 0xB848ui16);
		mem::write(addr + 2, proxy);
		mem::write(addr + 10, 0xD0FFui16);
#else
		mem::write(addr, 0xE8ui8);
		mem::write(addr + 1, (proxy - addr - 5));
#endif // _WIN64
	}
};
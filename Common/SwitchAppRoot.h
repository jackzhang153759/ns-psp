// Single definition of where the port keeps its data.
//
// Everything that used to be hardcoded under /switch/ppsspp-nx/ (memstick, shader caches, restart
// handoff, LSFG assets) is derived from SwitchAppRoot(), which is the directory the NRO itself sits
// in - so a user can move PPSSPP.nro to /switch/PPSSPP/ and the data follows.
//
// SwitchSetAppRootFromNro() must be called as early as possible in main(), before anything reads the
// root. Until then - and whenever argv[0] was missing or unusable - the historical fallback is
// returned, so a broken argv can never leave the app without a working data directory.
//
// Header-only on purpose: it is used from SDLMain (UI target) and from the Vulkan helpers in Common,
// and there is nothing here worth adding a translation unit for.

#pragma once

#include <string>

inline std::string &SwitchAppRootStorage() {
	static std::string appRoot;
	return appRoot;
}

// Derives the data root from the (already normalized) NRO path. Accepts both "sdmc:/switch/..." and
// plain "/switch/..." forms; the root is always a plain absolute path ending in '/', matching the
// style the memstick code has always used ("/switch/ppsspp-nx/").
inline void SwitchSetAppRootFromNro(const std::string &nroPath) {
	std::string root;
	if (!nroPath.empty()) {
		std::string normalized = nroPath;
		if (normalized.rfind("sdmc:/", 0) == 0) {
			normalized = "/" + normalized.substr(6);  // "sdmc:/switch/x" -> "/switch/x"
		}
		const size_t lastSlash = normalized.find_last_of('/');
		if (lastSlash != std::string::npos) {
			root = normalized.substr(0, lastSlash + 1);
		}
	}
	if (root.empty()) {
		root = "/switch/ppsspp-nx/";  // historical fallback, so a broken argv still yields a root
	}
	if (root.back() != '/') {
		root += '/';
	}
	SwitchAppRootStorage() = root;
}

// The data root, ending in '/'. Never empty.
inline const std::string &SwitchAppRoot() {
	static const std::string fallback = "/switch/ppsspp-nx/";
	const std::string &root = SwitchAppRootStorage();
	return root.empty() ? fallback : root;
}

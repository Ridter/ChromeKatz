#include <Windows.h>
#include <cstdint>
#include "udmp-parser.h"
#include "Helper.h"

#pragma region structs
struct OptimizedString {
	char buf[23];
	UCHAR len;
};

struct RemoteString {
	uintptr_t dataAddress;
	size_t strLen; //This won't include the null terminator
	int strMax; //Maximum string length
	char unk[3]; //I just couldn't figure out the last data type :(
	UCHAR strAlloc; //Seems to always be 0x80, honestly no idea what it should mean
};

#pragma region Chrome
enum class CookieSameSite {
	UNSPECIFIED = -1,
	NO_RESTRICTION = 0,
	LAX_MODE = 1,
	STRICT_MODE = 2,
	// Reserved 3 (was EXTENDED_MODE), next number is 4.

	// Keep last, used for histograms.
	kMaxValue = STRICT_MODE
};

enum class CookieSourceScheme {
	kUnset = 0,
	kNonSecure = 1,
	kSecure = 2,

	kMaxValue = kSecure  // Keep as the last value.
};

enum CookiePriority {
	COOKIE_PRIORITY_LOW = 0,
	COOKIE_PRIORITY_MEDIUM = 1,
	COOKIE_PRIORITY_HIGH = 2,
	COOKIE_PRIORITY_DEFAULT = COOKIE_PRIORITY_MEDIUM
};

enum class CookieSourceType {
	// 'unknown' is used for tests or cookies set before this field was added.
	kUnknown = 0,
	// 'http' is used for cookies set via HTTP Response Headers.
	kHTTP = 1,
	// 'script' is used for cookies set via document.cookie.
	kScript = 2,
	// 'other' is used for cookies set via browser login, iOS, WebView APIs,
	// Extension APIs, or DevTools.
	kOther = 3,

	kMaxValue = kOther,  // Keep as the last value.
};

//There is now additional cookie type "CookieBase", but I'm not going to add that here yet
struct CanonicalCookieChrome {
	uintptr_t _vfptr; //CanonicalCookie Virtual Function table address. This could also be used to scrape all cookies as it is backed by the chrome.dll
	OptimizedString name;
	OptimizedString domain;
	OptimizedString path;
	int64_t creation_date;
	bool secure;
	bool httponly;
	CookieSameSite same_site;
	char partition_key[120];  //Not implemented //This really should be 128 like in Edge... but for some reason it is not?
	CookieSourceScheme source_scheme;
	int source_port;    //Not implemented //End of Net::CookieBase
	OptimizedString value;
	int64_t expiry_date;
	int64_t last_access_date;
	int64_t last_update_date;
	CookiePriority priority;       //Not implemented
	CookieSourceType source_type;    //Not implemented
};

#pragma endregion

#pragma region Edge
struct CanonicalCookieEdge {
	uintptr_t _vfptr; //CanonicalCookie Virtual Function table address. This could also be used to scrape all cookies as it is backed by the chrome.dll
	OptimizedString name;
	OptimizedString domain;
	OptimizedString path;
	int64_t creation_date;
	bool secure;
	bool httponly;
	CookieSameSite same_site;
	char partition_key[128];  //Not implemented
	CookieSourceScheme source_scheme;
	int source_port;    //Not implemented //End of Net::CookieBase
	OptimizedString value;
	int64_t expiry_date;
	int64_t last_access_date;
	int64_t last_update_date;
	CookiePriority priority;       //Not implemented
	CookieSourceType source_type;    //Not implemented
};
#pragma endregion

struct Node {
	uintptr_t left;
	uintptr_t right;
	uintptr_t parent;
	bool is_black; //My guess is that data is stored in red-black tree
	char padding[7];
	OptimizedString key;
	uintptr_t valueAddress;
};

struct RootNode {
	uintptr_t beginNode;
	uintptr_t firstNode;
	size_t size;
};
#pragma endregion

BOOL MyMemCmp(const BYTE* source, const BYTE* searchPattern, size_t num) {

	for (size_t i = 0; i < num; ++i) {
		if (searchPattern[i] == 0xAA)
			continue;
		if (source[i] != searchPattern[i]) {
			return FALSE;
		}
	}

	return TRUE;
}

BOOL PatternSearch(const BYTE* pattern, size_t patternSize, const uint8_t* source, SIZE_T sourcesize, uintptr_t& offset) {

	for (size_t i = 0; i <= sourcesize - patternSize; ++i) {
		if (MyMemCmp(source + i, pattern, patternSize)) {
			offset = i;
			return TRUE;
		}
	}
	return FALSE;
}

BOOL FindDLLPattern(udmpparser::UserDumpParser& dump, const char* dllName, const BYTE* pattern, size_t patternSize, uintptr_t& resultAddress) {

	for (const auto& [_, Descriptor] : dump.GetMem()) {
		const char* State = StateToString(Descriptor.State);

		if (strcmp(State, "MEM_FREE") == 0)
			continue;

		//Check if memory area is a module
		const auto& Module = dump.GetModule(Descriptor.BaseAddress);

		//This is a module if not nullptr
		if (Module != nullptr) {

			const auto& ModulePathName = Module->ModuleName;
			auto ModuleNameOffset = ModulePathName.find_last_of('\\');
			if (ModuleNameOffset == ModulePathName.npos) {
				ModuleNameOffset = 0;
			}
			else {
				ModuleNameOffset++;
			}


			if (strcmp(&ModulePathName[ModuleNameOffset], dllName) == 0)
			{
				printf("[*] Found target module: %s\n", &ModulePathName[ModuleNameOffset]);

				uintptr_t memoryOffset = 0;
				if (PatternSearch(pattern, patternSize, Descriptor.Data, Module->SizeOfImage, memoryOffset)) {
					resultAddress = Module->BaseOfImage + memoryOffset;
					return TRUE;
				}
				else {
					printf("[-] Failed to find the first pattern!\n");
					return FALSE;
				}
				break;
			}
		}
	}

	printf("[-] Failed to find the target module: %s\n", dllName);

	return FALSE;
}

BOOL FindPattern(udmpparser::UserDumpParser& dump, const BYTE* pattern, size_t patternSize, uintptr_t* CookieMonsterInstances, size_t& instanceCount) {

	for (const auto& [_, Descriptor] : dump.GetMem()) {
		const char* State = StateToString(Descriptor.State);
		const char* Type = TypeToString(Descriptor.Type);

		if (strcmp(State, "MEM_COMMIT") != 0)
			continue;
		if (Descriptor.DataSize == 0)
			continue;

		//Check if memory area is a module
		const auto& Module = dump.GetModule(Descriptor.BaseAddress);

		//Skip over modules
		if (Module != nullptr)
			continue;

		uintptr_t resultAddress = 0;
		uintptr_t memoryOffset = 0;

		for (size_t i = 0; i <= Descriptor.DataSize - patternSize; ++i) {
			if (MyMemCmp(Descriptor.Data + i, pattern, patternSize)) {
				CookieMonsterInstances[instanceCount] = Descriptor.BaseAddress + i;
				instanceCount++;
			}
		}
	}

	if (instanceCount > 0)
		return TRUE;
	return FALSE;
}

BOOL ReadDumpMemory(udmpparser::UserDumpParser& dump, uint64_t address, LPVOID target, size_t readsize) {
	auto data = dump.ReadMemory(address, readsize);
	if (!data.has_value())
		return FALSE;

	if (data.value().size() != readsize)
		return FALSE;

	memcpy_s(target, readsize, data.value().begin()._Ptr, readsize);
	return TRUE;
}

void ReadString(udmpparser::UserDumpParser& dump, OptimizedString string) {

	if (string.len > 23)
	{
		RemoteString longString = { 0 };
		std::memcpy(&longString, &string.buf, sizeof(RemoteString));

		if (longString.dataAddress != 0) {
			unsigned char* buf = (unsigned char*)malloc(longString.strMax);
			if (buf == 0 || !ReadDumpMemory(dump, longString.dataAddress, buf, longString.strLen + 1)) {
				printf("[-] Failed to read cookie value at: 0x%p", (void*)longString.dataAddress);
				free(buf);
				return;
			}
			printf("%s\n", buf);
			free(buf);
		}
	}
	else
		printf("%s\n", string.buf);

}

void PrintTimeStamp(int64_t timeStamp) {
	ULONGLONG fileTimeTicks = timeStamp * 10;

	FILETIME fileTime;
	fileTime.dwLowDateTime = static_cast<DWORD>(fileTimeTicks & 0xFFFFFFFF);
	fileTime.dwHighDateTime = static_cast<DWORD>(fileTimeTicks >> 32);

	SYSTEMTIME systemTime;
	FileTimeToSystemTime(&fileTime, &systemTime);

	printf("%04hu-%02hu-%02hu %02hu:%02hu:%02hu\n",
		systemTime.wYear, systemTime.wMonth, systemTime.wDay,
		systemTime.wHour, systemTime.wMinute, systemTime.wSecond);
}

void PrintValuesEdge(CanonicalCookieEdge cookie, udmpparser::UserDumpParser& dump) {
	printf("    Name: ");
	ReadString(dump, cookie.name);
	printf("    Value: ");
	ReadString(dump, cookie.value);
	printf("    Domain: ");
	ReadString(dump, cookie.domain);
	printf("    Path: ");
	ReadString(dump, cookie.path);
	printf("    Creation time: ");
	PrintTimeStamp(cookie.creation_date);
	printf("    Expiration time: ");
	PrintTimeStamp(cookie.expiry_date);
	printf("    Last accessed: ");
	PrintTimeStamp(cookie.last_access_date);
	printf("    Last updated: ");
	PrintTimeStamp(cookie.last_update_date);
	printf("    Secure: %s\n", cookie.secure ? "True" : "False");
	printf("    HttpOnly: %s\n", cookie.httponly ? "True" : "False");

	printf("\n");
}

void PrintValuesChrome(CanonicalCookieChrome cookie, udmpparser::UserDumpParser& dump) {
	printf("    Name: ");
	ReadString(dump, cookie.name);
	printf("    Value: ");
	ReadString(dump, cookie.value);
	printf("    Domain: ");
	ReadString(dump, cookie.domain);
	printf("    Path: ");
	ReadString(dump, cookie.path);
	printf("    Creation time: ");
	PrintTimeStamp(cookie.creation_date);
	printf("    Expiration time: ");
	PrintTimeStamp(cookie.expiry_date);
	printf("    Last accessed: ");
	PrintTimeStamp(cookie.last_access_date);
	printf("    Last updated: ");
	PrintTimeStamp(cookie.last_update_date);
	printf("    Secure: %s\n", cookie.secure ? "True" : "False");
	printf("    HttpOnly: %s\n", cookie.httponly ? "True" : "False");

	printf("\n");
}

void ProcessNodeValue(udmpparser::UserDumpParser& dump, uintptr_t Valueaddr, bool isChrome) {

	if (isChrome) {
		CanonicalCookieChrome cookie = { 0 };
		if (!ReadDumpMemory(dump, Valueaddr, &cookie, sizeof(CanonicalCookieChrome))) {
			PrintErrorWithMessage(TEXT("Failed to read cookie struct"));
			return;
		}
		PrintValuesChrome(cookie, dump);

	}
	else {
		CanonicalCookieEdge cookie = { 0 };
		if (!ReadDumpMemory(dump, Valueaddr, &cookie, sizeof(CanonicalCookieEdge))) {
			PrintErrorWithMessage(TEXT("Failed to read cookie struct"));
			return;
		}
		PrintValuesEdge(cookie, dump);
	}
}

void ProcessNode(udmpparser::UserDumpParser& dump, const Node& node, bool isChrome) {
	// Process the current node
	printf("Cookie Key: ");
	ReadString(dump, node.key);

	ProcessNodeValue(dump, node.valueAddress, isChrome);

	// Process the left child if it exists
	if (node.left != 0) {
		Node leftNode;
		if (ReadDumpMemory(dump, node.left, &leftNode, sizeof(Node)))
			ProcessNode(dump, leftNode, isChrome);
		else
			printf("Error reading left node");
	}

	// Process the right child if it exists
	if (node.right != 0) {
		Node rightNode;
		if (ReadDumpMemory(dump, node.right, &rightNode, sizeof(Node)))
			ProcessNode(dump, rightNode, isChrome);
		else
			printf("Error reading right node");
	}
}

void WalkCookieMap(udmpparser::UserDumpParser& dump, uintptr_t cookieMapAddress, bool isChrome) {

	RootNode cookieMap;

	if (!ReadDumpMemory(dump, cookieMapAddress, &cookieMap, sizeof(RootNode))) {
		printf("[-] Failed to read the root node from address: 0x%p\n", (void*)cookieMapAddress);
		return;
	}

	if (cookieMap.size == 0) {
		printf("[*] This cookie map was empty\n\n");
		return;

	}
	printf("[*] Number of available cookies: %zu\n\n", cookieMap.size);

	// Process the first node in the binary search tree
	Node firstNode;
	if (ReadDumpMemory(dump, cookieMap.firstNode, &firstNode, sizeof(Node)))
		ProcessNode(dump, firstNode, isChrome);
	else
		printf("[-] Failed to read the first node from address: 0x%p\n", (void*)cookieMap.firstNode);

	printf("\n");
}
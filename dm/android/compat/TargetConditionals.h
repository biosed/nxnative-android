// Android: neither macOS nor iOS (see README.md). Code that is the same for every non-iOS target keeps testing !TARGET_OS_IPHONE; Mac-only code tests TARGET_OS_OSX
#pragma once
#define TARGET_OS_MAC 0
#define TARGET_OS_OSX 0
#define TARGET_OS_IPHONE 0
#define TARGET_OS_IOS 0
#define TARGET_OS_SIMULATOR 0

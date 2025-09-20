#include "FileDialog.h"

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#endif

std::string openFileDialog(const char* title, const char* directory, const std::vector<const char*>& exts) {
#ifdef __APPLE__
	@autoreleasepool {
		NSOpenPanel* panel = [NSOpenPanel openPanel];
		[panel setCanChooseFiles:YES];
		[panel setCanChooseDirectories:NO];
		[panel setAllowsMultipleSelection:NO];
		if (title) [panel setTitle:[NSString stringWithUTF8String:title]];
		if (directory) [panel setDirectoryURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:directory]]];
		if (!exts.empty()) {
			NSMutableArray* types = [NSMutableArray arrayWithCapacity:exts.size()];
			for (size_t i = 0; i < exts.size(); ++i) {
				if (exts[i] && std::strlen(exts[i]) > 0) {
					[types addObject:[NSString stringWithUTF8String:exts[i]]];
				}
			}
			if ([types count] > 0) {
				[panel setAllowedFileTypes:types];
			}
		}
		NSInteger result = [panel runModal];
		if (result == NSModalResponseOK) {
			NSURL* url = [[panel URLs] objectAtIndex:0];
			std::string path([[url path] UTF8String]);
			return path;
		}
	}
	return std::string();
#else
	(void)title; (void)directory; (void)exts;
	return std::string();
#endif
}



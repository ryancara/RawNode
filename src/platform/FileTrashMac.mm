#include "platform/FileTrash.h"
#import <Foundation/Foundation.h>

bool moveFileToTrash(const std::string &path, std::string &error) {
  @autoreleasepool {
    NSString *name = [[NSString alloc] initWithBytes:path.data() length:path.size() encoding:NSUTF8StringEncoding];
    if (!name) { error = "The file path is not valid UTF-8"; return false; }
    NSURL *url = [NSURL fileURLWithPath:name];
    [name release];
    NSError *failure = nil;
    const bool moved = [[NSFileManager defaultManager] trashItemAtURL:url resultingItemURL:nullptr error:&failure];
    error = moved ? "" : failure ? [[failure localizedDescription] UTF8String] : "Native Trash is unavailable";
    return moved;
  }
}

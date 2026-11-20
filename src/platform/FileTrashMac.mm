#include "platform/FileTrash.h"
#import <Foundation/Foundation.h>

TrashOutcome moveFileToTrash(const std::string &path, std::string &error) {
  @autoreleasepool {
    NSString *name = [[NSString alloc] initWithBytes:path.data() length:path.size() encoding:NSUTF8StringEncoding];
    if (!name) { error = "The file path is not valid UTF-8"; return TrashOutcome::Failed; }
    NSURL *url = [NSURL fileURLWithPath:name];
    [name release];
    NSError *failure = nil;
    const bool moved = [[NSFileManager defaultManager] trashItemAtURL:url resultingItemURL:nullptr error:&failure];
    const char *description = failure ? [[failure localizedDescription] UTF8String] : nullptr;
    error = moved ? "" : description ? description : "Native Trash is unavailable";
    return moved ? TrashOutcome::Trashed : TrashOutcome::Failed;
  }
}

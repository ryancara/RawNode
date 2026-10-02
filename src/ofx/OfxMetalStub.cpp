#include "ofx/OfxMetal.h"

#if defined(__APPLE__)
#error "OfxMetalStub.cpp must not be built on Apple platforms"
#endif

bool ofxMetalInit() { return false; }
bool ofxMetalAvailable() { return false; }

struct OfxMetalBuffer {};

OfxMetalBuffer *ofxMetalBufferCreate(size_t) { return nullptr; }
void ofxMetalBufferRelease(OfxMetalBuffer *) {}
void *ofxMetalBufferContents(OfxMetalBuffer *) { return nullptr; }
void *ofxMetalBufferHandle(OfxMetalBuffer *) { return nullptr; }
void *ofxMetalCommandQueue() { return nullptr; }
void ofxMetalSync() {}

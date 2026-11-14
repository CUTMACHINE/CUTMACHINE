// SN7 -- docs/roadmap/SON-2026-10.md. cutmachine-sound-host: the sound runtime
// of tools/cutmachine-sound in a JavaScriptCore context, without Node.
//
// A process of its own, which the engine launches (SoundRender.h), for three
// reasons measured or documented in SN6. It alone carries
// com.apple.security.cs.allow-jit: without that entitlement a JSContext runs
// interpreted, 16 times slower, and the entitlement holds for a whole process
// -- the editor's window has no use for it. It can be stopped: a JSContext has
// no public time limit, a process can be killed. And a ditty's code runs away
// from the editor.
//
//   cutmachine-sound-host <request.json>
//   request: { "job": <job.mjs's job>, "out": "<file.wav>", "stems_dir":
//   "<folder>" | null }
//
// Prints the report `cm-sound render` prints, built by the same JavaScript
// (job.mjs), so that both answer alike; errors as {"ok":false,"error":...} on
// standard error, exit status 1. The hash of the samples is CommonCrypto's
// SHA-256, node:crypto's on the Node side.

#import <Foundation/Foundation.h>
#import <JavaScriptCore/JavaScriptCore.h>

#include <CommonCrypto/CommonDigest.h>

#include <cstdio>
#include <string>

extern const char kSoundRuntimeBundle[];

namespace {

// As the Node CLI answers a refusal: {"ok":false,"error":"..."}, in that order.
[[noreturn]] void Fail(NSString* message) {
    NSString* said = message != nil ? message : @"unknown error";
    NSData* quoted = [NSJSONSerialization dataWithJSONObject:@[ said ]
                                                     options:0
                                                       error:nil];
    NSString* text = quoted != nil
                         ? [[NSString alloc] initWithData:quoted
                                                 encoding:NSUTF8StringEncoding]
                         : nil;
    const char* json =
        text != nil && text.length >= 2
            ? [text substringWithRange:NSMakeRange(1, text.length - 2)]
                  .UTF8String
            : "\"unknown error\"";
    fprintf(stderr, "{\"ok\":false,\"error\":%s}\n", json);
    exit(1);
}

// The bytes a Uint8Array holds, as JavaScriptCore keeps them, without a copy.
NSData* TypedArrayBytes(JSValue* value) {
    JSGlobalContextRef context = value.context.JSGlobalContextRef;
    JSValueRef exception = nullptr;
    JSObjectRef object = JSValueToObject(context, value.JSValueRef, &exception);
    if (object == nullptr || exception != nullptr) {
        return nil;
    }
    const auto* base = static_cast<const uint8_t*>(
        JSObjectGetTypedArrayBytesPtr(context, object, &exception));
    const size_t length =
        JSObjectGetTypedArrayByteLength(context, object, &exception);
    const size_t offset =
        JSObjectGetTypedArrayByteOffset(context, object, &exception);
    if (base == nullptr || exception != nullptr) {
        return nil;
    }
    return [NSData dataWithBytesNoCopy:const_cast<uint8_t*>(base + offset)
                                length:length
                          freeWhenDone:NO];
}

NSString* Sha256(NSData* bytes, NSUInteger from) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(static_cast<const uint8_t*>(bytes.bytes) + from,
              static_cast<CC_LONG>(bytes.length - from), digest);
    NSMutableString* hex = [NSMutableString stringWithCapacity:64];
    for (unsigned char byte : digest) {
        [hex appendFormat:@"%02x", byte];
    }
    return hex;
}

void Write(NSData* bytes, NSString* path) {
    NSError* error = nil;
    [[NSFileManager defaultManager]
              createDirectoryAtPath:[path stringByDeletingLastPathComponent]
        withIntermediateDirectories:YES
                         attributes:nil
                              error:nil];
    if (![bytes writeToFile:path options:NSDataWritingAtomic error:&error]) {
        Fail([NSString stringWithFormat:@"cannot write %@: %@", path,
                                        error.localizedDescription]);
    }
}

}  // namespace

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc != 2) {
            Fail(@"usage: cutmachine-sound-host <request.json>");
        }
        NSData* requestData = [NSData
            dataWithContentsOfFile:@(argv[1] != nullptr ? argv[1] : "")];
        if (requestData == nil) {
            Fail(@"cannot read the request");
        }
        NSDictionary* request =
            [NSJSONSerialization JSONObjectWithData:requestData
                                            options:0
                                              error:nil];
        if (![request isKindOfClass:[NSDictionary class]] ||
            ![request[@"job"] isKindOfClass:[NSDictionary class]] ||
            ![request[@"out"] isKindOfClass:[NSString class]]) {
            Fail(@"the request needs a job and an out path");
        }
        NSString* out = request[@"out"];
        NSString* stemsDir =
            [request[@"stems_dir"] isKindOfClass:[NSString class]]
                ? request[@"stems_dir"]
                : nil;
        NSData* jobData =
            [NSJSONSerialization dataWithJSONObject:request[@"job"]
                                            options:0
                                              error:nil];
        if (jobData == nil) {
            Fail(@"the job does not write back as JSON");
        }
        NSString* jobJson =
            [[NSString alloc] initWithData:jobData
                                  encoding:NSUTF8StringEncoding];

        JSContext* context = [[JSContext alloc] init];
        __block NSString* thrown = nil;
        context.exceptionHandler = ^(JSContext*, JSValue* exception) {
          // An Error's message, as Node's err.message; anything else thrown,
          // as it reads.
          JSValue* message = exception.isObject ? exception[@"message"] : nil;
          thrown = message != nil && message.isString ? [message toString]
                                                      : [exception toString];
        };
        [context
            evaluateScript:[NSString stringWithUTF8String:kSoundRuntimeBundle]];
        if (thrown != nil) {
            Fail([@"the sound runtime did not load: "
                stringByAppendingString:thrown]);
        }

        JSValue* result =
            [context[@"cmSoundRun"] callWithArguments:@[ jobJson ]];
        if (thrown != nil) {
            Fail(thrown);
        }

        NSData* main = TypedArrayBytes(result[@"bytes"]);
        if (main == nil) {
            Fail(@"the render returned no WAV bytes");
        }
        const NSUInteger dataOffset = [result[@"dataOffset"] toUInt32];
        Write(main, out);
        NSString* sha256 = Sha256(main, dataOffset);

        if (stemsDir != nil) {
            JSValue* stems = result[@"stems"];
            const uint32_t count = [stems[@"length"] toUInt32];
            for (uint32_t index = 0; index < count; ++index) {
                JSValue* stem = stems[index];
                NSData* bytes = TypedArrayBytes(stem[@"bytes"]);
                if (bytes == nil) {
                    Fail(@"the render returned a stem without WAV bytes");
                }
                Write(
                    bytes,
                    [stemsDir stringByAppendingPathComponent:[stem[@"fileName"]
                                                                 toString]]);
            }
        }
        JSValue* report = [context[@"cmSoundReport"] callWithArguments:@[
            out, sha256, stemsDir != nil ? stemsDir : [NSNull null]
        ]];
        if (thrown != nil) {
            Fail(thrown);
        }
        printf("%s\n", [[report toString] UTF8String]);
    }
    return 0;
}

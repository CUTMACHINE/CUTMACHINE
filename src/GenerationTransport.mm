// GENERATIF-2026-10 GN1 -- docs/roadmap/GENERATIF-2026-10.md. The macOS
// half of paid generation (Generation.h): an HTTPS request through
// NSURLSession, and the fal.ai key read from the Keychain.
//
// Written in a Linux session; compiled on a Mac on 21 October 2026, never
// called against fal.ai. An ephemeral session, no redirect followed (a
// redirect would carry the Authorization header elsewhere), no NSError copied
// into a message (it may hold a signed URL).

#include "Generation.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

// Redirects are refused: one would carry `Authorization: Key ...` to
// whatever host it names.
@interface CMGenerationSessionDelegate : NSObject <NSURLSessionTaskDelegate>
@end

@implementation CMGenerationSessionDelegate
- (void)URLSession:(NSURLSession*)session
                          task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)request
             completionHandler:(void (^)(NSURLRequest*))completionHandler {
    (void)session;
    (void)task;
    (void)response;
    (void)request;
    completionHandler(nil);
}
@end

namespace {

bool NativeGenerationHttp(const generation::HttpRequest& request,
                          generation::HttpResponse& response,
                          std::string& error) {
    @autoreleasepool {
        NSURL* url = [NSURL URLWithString:@(request.url.c_str())];
        if (url == nil) {
            error = "adresse invalide";
            return false;
        }
        const double timeout =
            request.timeout_seconds > 0 ? request.timeout_seconds : 120.0;
        NSMutableURLRequest* native = [NSMutableURLRequest requestWithURL:url];
        native.HTTPMethod = @(request.method.c_str());
        native.timeoutInterval = timeout;
        for (const auto& header : request.headers) {
            [native setValue:@(header.second.c_str())
                forHTTPHeaderField:@(header.first.c_str())];
        }
        if (!request.body.empty()) {
            native.HTTPBody = [NSData dataWithBytes:request.body.data()
                                             length:request.body.size()];
        }
        NSURLSessionConfiguration* configuration =
            NSURLSessionConfiguration.ephemeralSessionConfiguration;
        configuration.timeoutIntervalForResource = timeout;
        NSURLSession* session = [NSURLSession
            sessionWithConfiguration:configuration
                            delegate:[CMGenerationSessionDelegate new]
                       delegateQueue:nil];
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block NSData* received = nil;
        __block NSHTTPURLResponse* http = nil;
        __block BOOL failed = NO;
        NSURLSessionDataTask* task =
            [session dataTaskWithRequest:native
                       completionHandler:^(NSData* data, NSURLResponse* result,
                                           NSError* taskError) {
                         received = data;
                         if ([result isKindOfClass:NSHTTPURLResponse.class]) {
                             http = (NSHTTPURLResponse*)result;
                         }
                         failed = taskError != nil;
                         dispatch_semaphore_signal(done);
                       }];
        [task resume];
        const long timedOut = dispatch_semaphore_wait(
            done,
            dispatch_time(DISPATCH_TIME_NOW,
                          static_cast<int64_t>((timeout + 5) * NSEC_PER_SEC)));
        if (timedOut != 0 || failed || http == nil) {
            [task cancel];
            [session invalidateAndCancel];
            error = "connexion impossible ou délai dépassé";
            return false;
        }
        response.status = static_cast<int>(http.statusCode);
        NSString* type = [http valueForHTTPHeaderField:@"Content-Type"];
        response.content_type = type != nil ? type.UTF8String : "";
        response.body.clear();
        if (received.length > 0) {
            response.body.assign(static_cast<const char*>(received.bytes),
                                 received.length);
        }
        [session finishTasksAndInvalidate];
        return true;
    }
}

bool KeychainFalKey(std::string& key, std::string& message) {
    @autoreleasepool {
        NSDictionary* query = @{
            (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
            (__bridge id)kSecAttrService : @(generation::kFalKeychainService),
            (__bridge id)kSecAttrAccount : @(generation::kFalKeychainAccount),
            (__bridge id)kSecReturnData : @YES,
            (__bridge id)
            kSecUseAuthenticationUI : (__bridge id)kSecUseAuthenticationUIFail
        };
        CFTypeRef result = nullptr;
        const OSStatus status =
            SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
        if (status != errSecSuccess) {
            message = status == errSecItemNotFound
                          ? "aucune clé fal.ai dans le trousseau"
                          : "le trousseau n'a pas rendu la clé fal.ai";
            return false;
        }
        NSData* data = CFBridgingRelease(result);
        key.assign(static_cast<const char*>(data.bytes), data.length);
        return true;
    }
}

}  // namespace

namespace generation {

void InstallGenerationTransport() {
    SetGenerationTransport(&NativeGenerationHttp);
    SetGenerationKeyReader(&KeychainFalKey);
}

}  // namespace generation

#import "ChatPanelView.h"

#include "AcpClient.h"
#include "AcpProcess.h"
#include "AfterBridge.h"
#include "AiEngineForm.h"
#include "ChatLlmClient.h"
#import "ChatMessageView.h"
#include "ChatPresentation.h"
#include "ChatSession.h"
#include "Cli.h"
#include "Document.h"
#include "Export.h"
#include "ExportVerification.h"
#include "LookLutStorage.h"
#include "MainThreadBackend.h"
#include "McpTools.h"
#include "ResolveXmlExport.h"
#include "UiTheme.h"

#import "FormPanel.h"
#import "UiComponents.h"
#import "UiThemeAppKit.h"

#import <Security/Security.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

NSString* const kAiModelDefaultsKey = @"AiEngine.Model.v1";
NSString* const kAiBaseUrlDefaultsKey = @"AiEngine.BaseUrl.v1";
NSString* const kAiProviderDefaultsKey = @"AiEngine.Provider.v1";
// ACP3 -- docs/roadmap/ACP-2026-09.md. The provider value that selects an
// external coding agent, and the command that starts it (an empty one is
// ai_engine::kDefaultAcpCommand, pinned since PRD22).
NSString* const kAiProviderAcp = @"acp";
NSString* const kAiAcpCommandDefaultsKey = @"AiEngine.AcpCommand.v1";
NSString* const kAiKeychainService = @"com.cutmachine.editor.ai";
NSString* const kAnthropicKeychainAccount = @"anthropic-messages-api-key";
NSString* const kOpenAiKeychainAccount = @"openai-compatible-api-key";

NSDictionary* AiKeychainIdentity(chat::ChatLlmProvider provider) {
    NSString* account = provider == chat::ChatLlmProvider::AnthropicMessages
                            ? kAnthropicKeychainAccount
                            : kOpenAiKeychainAccount;
    return @{
        (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService : kAiKeychainService,
        (__bridge id)kSecAttrAccount : account,
    };
}

std::string SecurityError(OSStatus status) {
    CFStringRef text = SecCopyErrorMessageString(status, nullptr);
    NSString* message = CFBridgingRelease(text);
    return message.UTF8String ?: "unknown Keychain error";
}

bool ReadAiApiKey(chat::ChatLlmProvider provider, std::string& key,
                  std::string& error) {
    key.clear();
    error.clear();
    NSMutableDictionary* query = [AiKeychainIdentity(provider) mutableCopy];
    query[(__bridge id)kSecReturnData] = @YES;
    query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    CFTypeRef result = nullptr;
    const OSStatus status =
        SecItemCopyMatching((__bridge CFDictionaryRef)query, &result);
    if (status == errSecItemNotFound) return true;
    if (status != errSecSuccess) {
        error = "unable to read the API key from Keychain: " +
                SecurityError(status);
        return false;
    }
    NSData* data = CFBridgingRelease(result);
    NSString* value = [[NSString alloc] initWithData:data
                                            encoding:NSUTF8StringEncoding];
    if (!value) {
        error = "the API key stored in Keychain is not valid UTF-8";
        return false;
    }
    key = value.UTF8String ?: "";
    return true;
}

// PRD22 -- whether a key is stored for `provider`, for what the dialog's Key
// field suggests. The query asks for no data, so it never reads the secret.
bool AiKeyStored(chat::ChatLlmProvider provider) {
    NSMutableDictionary* query = [AiKeychainIdentity(provider) mutableCopy];
    query[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    return SecItemCopyMatching((__bridge CFDictionaryRef)query, nullptr) ==
           errSecSuccess;
}

bool WriteAiApiKey(chat::ChatLlmProvider provider, const std::string& key,
                   std::string& error) {
    error.clear();
    NSDictionary* identity = AiKeychainIdentity(provider);
    if (key.empty()) {
        const OSStatus status =
            SecItemDelete((__bridge CFDictionaryRef)identity);
        if (status == errSecSuccess || status == errSecItemNotFound)
            return true;
        error = "unable to delete the API key from Keychain: " +
                SecurityError(status);
        return false;
    }
    NSData* data = [NSData dataWithBytes:key.data() length:key.size()];
    const NSDictionary* values = @{(__bridge id)kSecValueData : data};
    OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)identity,
                                    (__bridge CFDictionaryRef)values);
    if (status == errSecItemNotFound) {
        NSMutableDictionary* item = [identity mutableCopy];
        item[(__bridge id)kSecValueData] = data;
        status = SecItemAdd((__bridge CFDictionaryRef)item, nullptr);
    }
    if (status == errSecSuccess) return true;
    error = "unable to save the API key in Keychain: " + SecurityError(status);
    return false;
}

chat::ChatLlmConfig LoadAiConfiguration(std::string& configurationError) {
    chat::ChatLlmConfig config =
        chat::ChatLlmConfig::FromEnvironment(&configurationError);
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    NSString* provider = [defaults stringForKey:kAiProviderDefaultsKey];
    // PRD22 -- one reading of the preference, the dialog's too.
    if (ai_engine::ProtocolOfStoredProvider(provider.UTF8String ?: "") ==
        ai_engine::EngineProtocol::Ollama)
        config.provider = chat::ChatLlmProvider::Ollama;
    NSString* model = [defaults stringForKey:kAiModelDefaultsKey];
    NSString* baseUrl = [defaults stringForKey:kAiBaseUrlDefaultsKey];
    if (config.provider == chat::ChatLlmProvider::Ollama) {
        config.api_key.clear();
        if (model.length == 0) config.model = chat::kDefaultOllamaModel;
        if (baseUrl.length == 0) {
            config.base_url = chat::kDefaultOllamaBaseUrl;
        } else if ([baseUrl hasSuffix:@"/v1"]) {
            baseUrl = [baseUrl substringToIndex:baseUrl.length - 3];
        }
    }
    if (model.length > 0) config.model = model.UTF8String ?: config.model;
    if (baseUrl.length > 0)
        config.base_url = baseUrl.UTF8String ?: config.base_url;

    std::string storedKey;
    std::string keychainError;
    if (!ReadAiApiKey(config.provider, storedKey, keychainError)) {
        configurationError = keychainError;
    } else if (!storedKey.empty()) {
        config.api_key = std::move(storedKey);
    }
    if (config.provider == chat::ChatLlmProvider::AnthropicMessages &&
        config.api_key.empty()) {
        if (keychainError.empty())
            configurationError = "Moteur IA non configuré.";
    } else if (keychainError.empty()) {
        configurationError.clear();
    }
    return config;
}

// The one real (non-test) ChatHttpTransport: a synchronous NSURLSession
// POST. Always called from the background queue -sendPressed: dispatches
// to, never the main thread -- blocking on the semaphore below is exactly
// the point, it is what makes ChatLlmClient::SendMessages a plain blocking
// call from ChatSession's point of view, matching sidecar/planner.py's own
// synchronous urllib-based _post_json.
bool UrlSessionTransport(const chat::ChatHttpRequest& request,
                         chat::ChatHttpResponse& response, std::string& error) {
    NSURL* url = [NSURL
        URLWithString:[NSString stringWithUTF8String:request.url.c_str()]];
    if (!url) {
        error = "invalid URL: " + request.url;
        return false;
    }
    NSMutableURLRequest* urlRequest = [NSMutableURLRequest requestWithURL:url];
    urlRequest.HTTPMethod = @"POST";
    for (const auto& header : request.headers) {
        [urlRequest setValue:[NSString
                                 stringWithUTF8String:header.second.c_str()]
            forHTTPHeaderField:[NSString
                                   stringWithUTF8String:header.first.c_str()]];
    }
    urlRequest.HTTPBody = [NSData dataWithBytes:request.body.data()
                                         length:request.body.size()];
    // PRD22 -- the client's timeout (ChatLlmConfig::timeout_seconds): the
    // time NSURLSession waits without a byte, which for a response that is
    // not streamed is the whole time the model writes.
    urlRequest.timeoutInterval =
        request.timeout_seconds > 0.0 ? request.timeout_seconds : 120.0;

    __block bool receivedResponse = false;
    __block int statusCode = 0;
    __block std::string bodyText;
    __block std::string errorText;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    NSURLSessionDataTask* task = [[NSURLSession sharedSession]
        dataTaskWithRequest:urlRequest
          completionHandler:^(NSData* data, NSURLResponse* urlResponse,
                              NSError* nsError) {
            if (nsError != nil) {
                errorText = nsError.localizedDescription.UTF8String
                                ?: "unknown network error";
            } else {
                receivedResponse = true;
                statusCode = static_cast<int>(
                    ((NSHTTPURLResponse*)urlResponse).statusCode);
                if (data.length > 0)
                    bodyText.assign(static_cast<const char*>(data.bytes),
                                    data.length);
            }
            dispatch_semaphore_signal(semaphore);
          }];
    [task resume];
    dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);

    if (!receivedResponse) {
        error = errorText;
        return false;
    }
    response.status_code = statusCode;
    response.body = bodyText;
    return true;
}

CGFloat CGF(double value) { return static_cast<CGFloat>(value); }

// ACP3 -- where the agent's own output goes, next to the chat logs:
// ~/Library/Logs/CUTMACHINE/agent/<stamp>.{stderr.log,acp.jsonl}. Local to
// this Mac, never in the project. Empty if the folder cannot be made.
std::string AgentLogStem() {
    NSURL* library =
        [NSFileManager.defaultManager URLsForDirectory:NSLibraryDirectory
                                             inDomains:NSUserDomainMask]
            .firstObject;
    if (!library) return {};
    NSURL* directory =
        [library URLByAppendingPathComponent:@"Logs/CUTMACHINE/agent"
                                 isDirectory:YES];
    if (!directory) return {};
    if (![NSFileManager.defaultManager createDirectoryAtURL:directory
                                withIntermediateDirectories:YES
                                                 attributes:nil
                                                      error:nil])
        return {};
    NSDateFormatter* stamp = [NSDateFormatter new];
    stamp.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    stamp.dateFormat = @"yyyy-MM-dd_HH-mm-ss";
    return [directory
               URLByAppendingPathComponent:[stamp stringFromDate:NSDate.date]]
                   .path.UTF8String
               ?: "";
}

// Every protocol line of one agent process, for reading a run back (the
// bench, BENCH-2026-09) -- written from the turn's thread and, for a stop,
// from the main thread.
struct AcpTraceFile {
    std::mutex mutex;
    std::ofstream stream;
};

// What the agent is told before its first instruction. Its own instructions
// (the Claude Code or Codex system prompt) stay; this says where it is.
std::string AcpPreamble(const std::string& projectPath) {
    return "Tu travailles dans le panneau Agent de CUTMACHINE, l'éditeur "
           "vidéo, sur le projet ouvert dans sa fenêtre : " +
           projectPath +
           ". Le serveur MCP « cutmachine » est branché sur ce projet : "
           "chaque modification du montage passe par ses outils, dans le même "
           "journal que la souris, et chacun de tes tours s'annule d'un seul "
           "Annuler. Ne modifie jamais les fichiers du projet toi-même, et "
           "n'appelle pas open_project ni create_project : le projet est "
           "déjà ouvert. Réponds en français.\n" +
           chat::kAgentReplyGuidance;
}

}  // namespace

BOOL CMAgentAnthropicKeySaved(void) {
    return AiKeyStored(chat::ChatLlmProvider::AnthropicMessages) ? YES : NO;
}

// A trivial flipped NSView, so the transcript stacks top-down (message 1 at
// the top) the way a scrollable log naturally reads, matching NSScrollView's
// own top-down scroller convention. Declared up front since
// -[CMChatPanelView initWithFrame:] below instantiates one.
@interface CMFlippedView : NSView
@end

@implementation CMFlippedView
- (BOOL)isFlipped {
    return YES;
}
@end

// ---------------------------------------------------------------------
// CMAiEngineFormView -- PRD22, see ChatPanelView.h. AiEngineForm.h decides;
// this lays the controls out (frames, as before) and applies the decision.
// ---------------------------------------------------------------------

namespace {

NSString* CMString(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

std::string CMUtf8(NSString* text) { return text.UTF8String ?: ""; }

}  // namespace

@implementation CMAiEngineFormView {
    NSPopUpButton* _protocolMenu;
    NSTextField* _acpCommand;
    NSTextField* _model;
    NSTextField* _baseUrl;
    NSSecureTextField* _apiKey;
    NSButton* _removeKey;
    // Each text field's label, dimmed with it: in the dark appearance a
    // disabled field looks like an empty one, its label alone shows it off.
    NSTextField* _acpCommandLabel;
    NSTextField* _modelLabel;
    NSTextField* _baseUrlLabel;
    NSTextField* _apiKeyLabel;
    ai_engine::FormDrafts _drafts;
    ai_engine::FormContext _context;
    ai_engine::EngineProtocol _shown;
}

- (instancetype)initWithProtocol:(ai_engine::EngineProtocol)protocol
                          drafts:(const ai_engine::FormDrafts&)drafts
                         context:(const ai_engine::FormContext&)context {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 480, 230)])) {
        _drafts = drafts;
        _context = context;

        const auto addLabel = [self](NSString* title, CGFloat y,
                                     NSString* identifier) {
            NSTextField* label = [NSTextField labelWithString:title];
            label.frame = NSMakeRect(0, y + 3, 128, 20);
            label.font = CMFont(ui::theme::kFontSizeBody, NSFontWeightRegular);
            label.identifier = identifier;
            [self addSubview:label];
            return label;
        };
        const auto addField = [self](NSTextField* field, CGFloat y,
                                     NSString* identifier) {
            field.frame = NSMakeRect(136, y, 344, 26);
            field.identifier = identifier;
            [self addSubview:field];
            return field;
        };

        addLabel(@"Protocole", 196, nil);
        _protocolMenu =
            [[NSPopUpButton alloc] initWithFrame:NSMakeRect(136, 192, 344, 28)];
        for (const ai_engine::EngineProtocol item : ai_engine::kEngineProtocols)
            [_protocolMenu addItemWithTitle:@(ai_engine::MenuTitle(item))];
        [_protocolMenu selectItemAtIndex:ai_engine::MenuIndexOf(protocol)];
        _protocolMenu.identifier = @"ai_engine.provider";
        _protocolMenu.target = self;
        _protocolMenu.action = @selector(protocolChanged:);
        [self addSubview:_protocolMenu];

        _acpCommandLabel =
            addLabel(@"Commande ACP", 156, @"ai_engine.acp_command.label");
        _acpCommand = addField([[NSTextField alloc] initWithFrame:NSZeroRect],
                               152, @"ai_engine.acp_command");
        _modelLabel = addLabel(@"Modèle", 116, @"ai_engine.model.label");
        _model = addField([[NSTextField alloc] initWithFrame:NSZeroRect], 112,
                          @"ai_engine.model");
        _baseUrlLabel =
            addLabel(@"URL de base", 76, @"ai_engine.base_url.label");
        _baseUrl = addField([[NSTextField alloc] initWithFrame:NSZeroRect], 72,
                            @"ai_engine.base_url");
        _apiKeyLabel = addLabel(@"Clé API", 36, @"ai_engine.api_key.label");
        _apiKey = (NSSecureTextField*)addField(
            [[NSSecureTextField alloc] initWithFrame:NSZeroRect], 32,
            @"ai_engine.api_key");

        _removeKey = [NSButton checkboxWithTitle:@"Supprimer la clé enregistrée"
                                          target:nil
                                          action:nil];
        _removeKey.frame = NSMakeRect(136, 0, 344, 22);
        _removeKey.identifier = @"ai_engine.remove_key";
        [self addSubview:_removeKey];

        [self cmShow:protocol texts:_drafts.Shown(protocol)];
    }
    return self;
}

// Every field takes the protocol's rule and its draft: a field it does not
// use is greyed and empty, its placeholder saying why.
- (void)cmShow:(ai_engine::EngineProtocol)protocol
         texts:(const ai_engine::FormTexts&)texts {
    const ai_engine::FormPolicy policy =
        ai_engine::PolicyFor(protocol, _context);
    const auto apply = [](NSTextField* field, NSTextField* label,
                          const ai_engine::FieldPolicy& rule,
                          const std::string& text) {
        field.enabled = rule.enabled;
        field.placeholderString = CMString(rule.placeholder);
        field.stringValue = CMString(text);
        // The system's own colours, as the rest of the alert's: AppKit dims
        // a disabled field but never its label.
        label.textColor = rule.enabled ? NSColor.labelColor
                                       : NSColor.disabledControlTextColor;
    };
    apply(_acpCommand, _acpCommandLabel, policy.acp_command, texts.acp_command);
    apply(_model, _modelLabel, policy.model, texts.model);
    apply(_baseUrl, _baseUrlLabel, policy.base_url, texts.base_url);
    apply(_apiKey, _apiKeyLabel, policy.api_key, texts.api_key);
    // A deletion ticked for one protocol must not follow to the next one's
    // key.
    _removeKey.state = NSControlStateValueOff;
    _removeKey.enabled = policy.remove_key_enabled;
    _shown = protocol;
}

- (ai_engine::FormTexts)cmFieldTexts {
    ai_engine::FormTexts texts;
    texts.acp_command = CMUtf8(_acpCommand.stringValue);
    texts.model = CMUtf8(_model.stringValue);
    texts.base_url = CMUtf8(_baseUrl.stringValue);
    texts.api_key = CMUtf8(_apiKey.stringValue);
    return texts;
}

// The menu's action, as the person moves it: the fields keep what they held
// for the protocol left and show the next one's.
- (void)protocolChanged:(id)sender {
    (void)sender;
    // A field still being typed in gives its text to its cell first.
    if (self.window) [self.window makeFirstResponder:nil];
    const ai_engine::EngineProtocol next =
        ai_engine::ProtocolAtMenuIndex(_protocolMenu.indexOfSelectedItem);
    if (next == _shown) return;
    [self cmShow:next texts:_drafts.Move(_shown, [self cmFieldTexts], next)];
}

- (ai_engine::EngineProtocol)selectedProtocol {
    [self protocolChanged:nil];
    return _shown;
}

- (ai_engine::FormTexts)texts {
    [self protocolChanged:nil];
    return [self cmFieldTexts];
}

- (BOOL)removesStoredKey {
    return _removeKey.enabled && _removeKey.state == NSControlStateValueOn;
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. The window's form panel in
// place of an alert; the panel takes the form out of any earlier one.
- (CMFormPanel*)makePanel {
    CMFormPanel* panel = [[CMFormPanel alloc]
        initWithTitle:@"Moteur IA"
              message:
                  [NSString
                      stringWithFormat:
                          @"Choisissez Anthropic, un serveur local Ollama, ou "
                           "un agent de code qui parle ACP (Claude Code, "
                           "Codex…) : sa commande, par exemple %@. Seuls les "
                           "champs du protocole choisi sont actifs ; un "
                           "modèle, une URL ou une commande laissés vides "
                           "prennent la valeur proposée en gris. L'agent se "
                           "connecte avec son propre compte et ne touche le "
                           "montage que par les outils du moteur. Une "
                           "éventuelle clé reste dans le Trousseau de ce Mac "
                           "et ne fait jamais partie du projet.",
                          @(ai_engine::kCodexAcpCommandExample)]
                 form:self];
    [panel addButtonWithTitle:@"Enregistrer" role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    return panel;
}

@end

// ---------------------------------------------------------------------
// CMChatPanelView
// ---------------------------------------------------------------------

@interface CMChatPanelView () <NSTextFieldDelegate>
- (void)cmLayoutForSize:(NSSize)size;
- (void)cmRelayoutTranscript;
- (CMChatRowStyle)cmStyleForEntry:(const chat::ChatTranscriptEntry&)entry;
- (NSString*)cmDisplayTextForEntry:(const chat::ChatTranscriptEntry&)entry;
- (void)appendTranscriptEntry:(const chat::ChatTranscriptEntry&)entry;
- (void)cmRemoveConversationRows;
- (void)cmScrollToBottom;
- (void)cmReloadConfiguration;
- (void)cmShowConfigurationStatus;
- (void)cmConfigureAcpAgent;
- (std::string)cmAskPermission:(const acp::AcpPermissionRequest&)request;
- (void)configurePressed:(id)sender;
- (void)presetChanged:(id)sender;
- (void)sendPressed:(id)sender;
@end

@implementation CMChatPanelView {
    NSScrollView* _scrollView;
    NSView* _transcriptContainer;  // flipped; rows stack top to bottom
    NSTextField* _inputField;
    NSButton* _sendButton;
    NSButton* _configureButton;
    NSPopUpButton* _presetPopup;
    NSTextField* _statusLabel;

    McpLiveBackend* _liveBackend;  // not owned; see -configureWithBackend:
    std::unique_ptr<MainThreadBackend> _mainThreadBackend;
    std::unique_ptr<McpToolRegistry> _registry;
    std::unique_ptr<chat::ChatLlmClient> _llm;
    std::unique_ptr<chat::ChatSession> _session;
    // ACP3 -- set instead of _session when the engine is an external agent.
    std::unique_ptr<acp::AcpChatSession> _acpSession;
    CMChatRowView* _planRow;  // this turn's plan line, rewritten in place
    // PRD25 -- the relayed agent's journal, by entry ID (AgentJournal.h).
    std::map<uint64_t, CMChatRowView*> _journalRows;
    std::string _missingKeyError;
    BOOL _turnInFlight;
    void (^_stopHandler)(void);
    std::string _chatLogPath;
}

- (void)setStopHandler:(void (^)(void))handler {
    _stopHandler = [handler copy];
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layer.backgroundColor = CMSurfacePanelColor().CGColor;

        _transcriptContainer = [[CMFlippedView alloc] initWithFrame:NSZeroRect];
        _scrollView = [[NSScrollView alloc] initWithFrame:NSZeroRect];
        _scrollView.documentView = _transcriptContainer;
        _scrollView.hasVerticalScroller = YES;
        _scrollView.drawsBackground = NO;
        _scrollView.borderType = NSNoBorder;
        [self addSubview:_scrollView];

        _statusLabel = [NSTextField wrappingLabelWithString:@""];
        _statusLabel.font =
            CMFont(ui::theme::kFontSizeCaption, NSFontWeightRegular);
        _statusLabel.textColor = CMThemeColor(ui::theme::kError);
        _statusLabel.backgroundColor = NSColor.clearColor;
        _statusLabel.hidden = YES;
        [self addSubview:_statusLabel];

        _inputField = [[NSTextField alloc] initWithFrame:NSZeroRect];
        _inputField.placeholderString = @"Décrire une modification…";
        _inputField.font =
            CMFont(ui::theme::kFontSizeBody, NSFontWeightRegular);
        _inputField.delegate = self;
        _inputField.target = self;
        _inputField.action = @selector(sendPressed:);
        [self addSubview:_inputField];

        _sendButton =
            CMMakeStyledButton(@"Envoyer", self, @selector(sendPressed:));
        [self addSubview:_sendButton];

        _presetPopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect];
        [_presetPopup addItemWithTitle:@"Prompt libre"];
        [_presetPopup addItemWithTitle:@"Short interview dynamique — 60 s"];
        _presetPopup.target = self;
        _presetPopup.action = @selector(presetChanged:);
        _presetPopup.identifier = @"chat.prompt_preset";
        [self addSubview:_presetPopup];

        _configureButton = CMMakeStyledButton(@"Configurer le moteur IA…", self,
                                              @selector(configurePressed:));
        _configureButton.identifier = @"chat.configure_engine";
        [self addSubview:_configureButton];

        [self cmLayoutForSize:frame.size];
    }
    return self;
}

- (void)configureWithBackend:(McpLiveBackend&)backend {
    _liveBackend = &backend;
    _mainThreadBackend = std::make_unique<MainThreadBackend>(backend);
    _registry = std::make_unique<McpToolRegistry>();

    [self cmReloadConfiguration];
}

- (void)cmReloadConfiguration {
    if (!_mainThreadBackend || !_registry) return;
    _missingKeyError.clear();
    // ACP3 -- the previous agent goes first (its process is given a moment).
    _acpSession.reset();
    _planRow = nil;
    NSString* provider = [NSUserDefaults.standardUserDefaults
        stringForKey:kAiProviderDefaultsKey];
    if (ai_engine::ProtocolOfStoredProvider(provider.UTF8String ?: "") ==
        ai_engine::EngineProtocol::Acp) {
        [self cmConfigureAcpAgent];
        [self cmShowConfigurationStatus];
        return;
    }
    chat::ChatLlmConfig config = LoadAiConfiguration(_missingKeyError);
    _session.reset();
    _llm = std::make_unique<chat::ChatLlmClient>(std::move(config),
                                                 UrlSessionTransport);

    __weak CMChatPanelView* weakSelf = self;
    _session = std::make_unique<chat::ChatSession>(
        *_mainThreadBackend, *_registry, *_llm, chat::kDefaultSystemPrompt,
        [weakSelf](const chat::ChatTranscriptEntry& entry) {
            // ChatSession::SubmitUserMessage runs on the background queue
            // -sendPressed: spawns; hop back to the main thread for every
            // UI update, one transcript entry at a time, so the panel fills
            // in live instead of jumping once at the end of the turn.
            const chat::ChatTranscriptEntry entryCopy = entry;
            dispatch_async(dispatch_get_main_queue(), ^{
              CMChatPanelView* strongSelf = weakSelf;
              if (strongSelf) [strongSelf appendTranscriptEntry:entryCopy];
            });
        });
    [self cmShowConfigurationStatus];
}

- (void)cmShowConfigurationStatus {
    if (_missingKeyError.empty()) {
        _statusLabel.hidden = YES;
        _statusLabel.stringValue = @"";
    } else {
        _statusLabel.stringValue =
            [NSString stringWithUTF8String:_missingKeyError.c_str()] ?: @"";
        _statusLabel.hidden = NO;
    }
    [self cmLayoutForSize:self.bounds.size];
}

// ACP3 -- docs/roadmap/ACP-2026-09.md. The agent is only described here; its
// process starts with the first instruction, so an open panel costs nothing.
- (void)cmConfigureAcpAgent {
    _session.reset();
    _llm.reset();
    NSString* stored = [NSUserDefaults.standardUserDefaults
        stringForKey:kAiAcpCommandDefaultsKey];
    const std::string command =
        stored.length > 0 ? std::string(stored.UTF8String ?: "")
                          : std::string(ai_engine::kDefaultAcpCommand);
    const std::string projectPath =
        _liveBackend ? _liveBackend->ProjectPath() : std::string();
    NSString* executable = NSBundle.mainBundle.executablePath;
    if (projectPath.empty() || executable.length == 0) {
        _missingKeyError = "L'agent de code travaille sur un projet "
                           "enregistré : enregistrez-le d'abord.";
        return;
    }
    acp::AcpSessionConfig config;
    // The agent works beside the project, where the rushes are, not inside
    // its package: the manifest and the journals are the engine's alone.
    std::filesystem::path workingDirectory =
        std::filesystem::path(projectPath).parent_path();
    if (workingDirectory.extension() == ".cutmachine-project")
        workingDirectory = workingDirectory.parent_path();
    config.cwd = workingDirectory.string();
    // The engine's server is this binary: started on the project the window
    // holds, it relays every call to the window's endpoint (AX1).
    config.mcp_servers.push_back({"cutmachine",
                                  executable.UTF8String ?: "",
                                  {"--mcp-serve", projectPath, "--stdio"},
                                  {}});
    config.preamble = AcpPreamble(projectPath);

    auto trace = std::make_shared<AcpTraceFile>();
    const std::string cwd = config.cwd;
    acp::AcpTransportFactory factory =
        [command, cwd,
         trace](std::string& error) -> std::unique_ptr<acp::AcpTransport> {
        std::string stem;
        @autoreleasepool {
            stem = AgentLogStem();
        }
        {
            const std::lock_guard<std::mutex> lock(trace->mutex);
            trace->stream.close();
            if (!stem.empty())
                trace->stream.open(stem + ".acp.jsonl",
                                   std::ios::app | std::ios::binary);
        }
        acp::AcpProcessOptions options;
        options.command = command;
        options.cwd = cwd;
        options.environment = acp::AgentEnvironment();
        options.stderr_path =
            stem.empty() ? std::string() : stem + ".stderr.log";
        return acp::AcpProcessTransport::Start(options, error);
    };

    __weak CMChatPanelView* weakSelf = self;
    acp::AcpPermissionPrompt prompt =
        [weakSelf](const acp::AcpPermissionRequest& request) -> std::string {
        // Asked from the turn's thread; answered by the person, on the main
        // thread, before the agent goes on.
        __block std::string chosen;
        const acp::AcpPermissionRequest copy = request;
        dispatch_sync(dispatch_get_main_queue(), ^{
          CMChatPanelView* strongSelf = weakSelf;
          if (strongSelf) chosen = [strongSelf cmAskPermission:copy];
        });
        return chosen;
    };
    _acpSession = std::make_unique<acp::AcpChatSession>(
        std::move(factory), std::move(config),
        [weakSelf](const chat::ChatTranscriptEntry& entry) {
            const chat::ChatTranscriptEntry entryCopy = entry;
            dispatch_async(dispatch_get_main_queue(), ^{
              CMChatPanelView* strongSelf = weakSelf;
              if (strongSelf) [strongSelf appendTranscriptEntry:entryCopy];
            });
        },
        std::move(prompt));
    _acpSession->SetTrace([trace](bool outgoing, const std::string& line) {
        const std::lock_guard<std::mutex> lock(trace->mutex);
        if (!trace->stream.is_open()) return;
        const auto now =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        trace->stream << "{\"ms\":" << now << ",\"dir\":\""
                      << (outgoing ? "out" : "in") << "\",\"line\":\""
                      << mcp_json::EscapeJsonString(line) << "\"}\n";
        trace->stream.flush();
    });
}

// ACP3 -- a call the agent wants to make outside the engine: a shell, a
// file edit, a fetch. The refusal is the first button, so Return says no.
- (std::string)cmAskPermission:(const acp::AcpPermissionRequest&)request {
    std::vector<const acp::AcpPermissionOption*> order;
    for (const char* kind :
         {"reject_once", "reject_always", "allow_once", "allow_always"}) {
        for (const acp::AcpPermissionOption& option : request.options)
            if (option.kind == kind) order.push_back(&option);
    }
    for (const acp::AcpPermissionOption& option : request.options)
        if (std::find(order.begin(), order.end(), &option) == order.end())
            order.push_back(&option);
    if (order.empty()) return {};

    std::string detail = request.title;
    std::string input = request.raw_input_json;
    if (input.size() > 600) {
        size_t cut = 600;
        while (cut > 0 &&
               (static_cast<unsigned char>(input[cut]) & 0xc0) == 0x80)
            --cut;
        input = input.substr(0, cut) + "…";
    }
    if (!input.empty()) detail += "\n\n" + input;
    detail += "\n\nLes outils du moteur passent sans question ; celui-ci "
              "n'en fait pas partie.";
    NSAlert* alert = [NSAlert new];
    alert.messageText = @"L'agent demande une autorisation";
    alert.informativeText =
        [NSString stringWithUTF8String:detail.c_str()] ?: @"";
    for (const acp::AcpPermissionOption* option : order)
        [alert addButtonWithTitle:[NSString
                                      stringWithUTF8String:option->name.c_str()]
                                      ?: @"?"];
    const NSInteger index = [alert runModal] - NSAlertFirstButtonReturn;
    if (index < 0 || index >= static_cast<NSInteger>(order.size())) return {};
    return order[static_cast<size_t>(index)]->id;
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self cmLayoutForSize:newSize];
}

- (void)cmLayoutForSize:(NSSize)size {
    const CGFloat inset = CGF(ui::theme::kSpaceS);
    const CGFloat inputHeight = CGF(ui::theme::kControlRowHeight);
    const CGFloat sendWidth = 76.0;
    const CGFloat configureHeight = 28.0;
    const CGFloat presetHeight = 28.0;
    const CGFloat statusHeight = _statusLabel.hidden ? 0.0 : 28.0;

    _statusLabel.frame = NSMakeRect(
        inset, inputHeight + presetHeight + configureHeight + 3 * inset,
        size.width - 2 * inset, std::max<CGFloat>(0.0, statusHeight - inset));
    _inputField.frame =
        NSMakeRect(inset, inset,
                   std::max<CGFloat>(0.0, size.width - sendWidth - 3 * inset),
                   inputHeight);
    _sendButton.frame = NSMakeRect(size.width - sendWidth - inset, inset,
                                   sendWidth, inputHeight);
    _presetPopup.frame = NSMakeRect(inset, inputHeight + 2 * inset,
                                    size.width - 2 * inset, presetHeight);
    _configureButton.frame =
        NSMakeRect(inset, inputHeight + presetHeight + 3 * inset,
                   size.width - 2 * inset, configureHeight);

    const CGFloat scrollTop =
        inputHeight + presetHeight + configureHeight + statusHeight + 5 * inset;
    _scrollView.frame =
        NSMakeRect(0, scrollTop, size.width,
                   std::max<CGFloat>(0.0, size.height - scrollTop));
    [self cmRelayoutTranscript];
}

- (void)presetChanged:(id)sender {
    (void)sender;
    _inputField.placeholderString = _presetPopup.indexOfSelectedItem == 1
                                        ? @"Angle ou consigne facultative…"
                                        : @"Décrire une modification…";
}

// Re-stacks every row top to bottom at the scroll view's current content
// width. Simple rather than incremental -- chat transcripts in one session
// are short enough (a handful of turns, each a handful of tool calls) that
// re-measuring every row on each append or resize is not a real cost, and
// it keeps this file free of the bookkeeping an incremental layout would
// need to stay correct across resizes.
- (void)cmRelayoutTranscript {
    const CGFloat width = _scrollView.contentSize.width;
    CGFloat y = 0.0;
    for (NSView* row in _transcriptContainer.subviews) {
        if (![row isKindOfClass:[CMChatRowView class]]) continue;
        CMChatRowView* chatRow = (CMChatRowView*)row;
        const CGFloat height = [chatRow layoutForWidth:width];
        chatRow.frame = NSMakeRect(0, y, width, height);
        y += height;
    }
    _transcriptContainer.frame = NSMakeRect(0, 0, width, y);
}

- (CMChatRowStyle)cmStyleForEntry:(const chat::ChatTranscriptEntry&)entry {
    switch (entry.kind) {
        case chat::ChatEntryKind::UserMessage:
            return CMChatRowStyleUser;
        case chat::ChatEntryKind::AssistantText:
            return CMChatRowStyleAssistant;
        case chat::ChatEntryKind::ToolCall:
            return CMChatRowStyleToolCall;
        case chat::ChatEntryKind::ToolResult:
            return entry.tool_ok ? CMChatRowStyleToolResultOk
                                 : CMChatRowStyleToolResultError;
        case chat::ChatEntryKind::Error:
            return CMChatRowStyleError;
        case chat::ChatEntryKind::Plan:
            return CMChatRowStylePlan;
        case chat::ChatEntryKind::AgentJournal:
            return entry.tool_ok ? CMChatRowStyleAgentJournal
                                 : CMChatRowStyleAgentJournalRefused;
    }
    return CMChatRowStyleAssistant;
}

- (NSString*)cmDisplayTextForEntry:(const chat::ChatTranscriptEntry&)entry {
    std::string text;
    switch (entry.kind) {
        case chat::ChatEntryKind::UserMessage:
            text = "Vous : " + entry.text;
            break;
        case chat::ChatEntryKind::AssistantText:
            text = "Agent : " + entry.text;
            break;
        case chat::ChatEntryKind::ToolCall:
            text = "→ " + entry.tool_name + "(" + entry.tool_args_json + ")";
            break;
        case chat::ChatEntryKind::ToolResult:
            text = (entry.tool_ok ? std::string("✓ ") : std::string("✗ ")) +
                   entry.tool_name + " : " + entry.text;
            break;
        case chat::ChatEntryKind::Error:
            text = "Erreur : " + entry.text;
            break;
        case chat::ChatEntryKind::Plan:
            text = "Plan :\n" + entry.text;
            break;
        case chat::ChatEntryKind::AgentJournal:
            // The journal writes the whole line, its time first.
            text = entry.text;
            break;
    }
    return [NSString stringWithUTF8String:text.c_str()];
}

// Visual QA of 19 September 2026 -- the conversation was kept nowhere: once
// the app quit, what the agent was asked, said and tried was gone, and a
// bench run could not be read back. Each entry is appended as one JSON line
// to a file per session, under ~/Library/Logs/CUTMACHINE/chat/, local to
// this Mac like the app's other logs. A picture is noted, not written.
- (void)cmLogTranscriptEntry:(const chat::ChatTranscriptEntry&)entry {
    if (_chatLogPath.empty()) {
        NSURL* library =
            [NSFileManager.defaultManager URLsForDirectory:NSLibraryDirectory
                                                 inDomains:NSUserDomainMask]
                .firstObject;
        if (!library) return;
        NSURL* directory =
            [library URLByAppendingPathComponent:@"Logs/CUTMACHINE/chat"
                                     isDirectory:YES];
        if (![NSFileManager.defaultManager createDirectoryAtURL:directory
                                    withIntermediateDirectories:YES
                                                     attributes:nil
                                                          error:nil])
            return;
        NSDateFormatter* stamp = [NSDateFormatter new];
        stamp.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
        stamp.dateFormat = @"yyyy-MM-dd_HH-mm-ss";
        NSString* name = [NSString
            stringWithFormat:@"%@.jsonl", [stamp stringFromDate:NSDate.date]];
        _chatLogPath =
            [directory URLByAppendingPathComponent:name].path.UTF8String ?: "";
        if (_chatLogPath.empty()) return;
    }
    const char* kind = "user";
    switch (entry.kind) {
        case chat::ChatEntryKind::UserMessage:
            kind = "user";
            break;
        case chat::ChatEntryKind::AssistantText:
            kind = "assistant";
            break;
        case chat::ChatEntryKind::ToolCall:
            kind = "tool_call";
            break;
        case chat::ChatEntryKind::ToolResult:
            kind = "tool_result";
            break;
        case chat::ChatEntryKind::Error:
            kind = "error";
            break;
        case chat::ChatEntryKind::Plan:
            kind = "plan";
            break;
        case chat::ChatEntryKind::AgentJournal:
            // Never logged (-showAgentJournal:): named for completeness.
            kind = "agent_journal";
            break;
    }
    NSISO8601DateFormatter* iso = [NSISO8601DateFormatter new];
    mcp_json::Value line = mcp_json::Value::MakeObject();
    line.Set("time", mcp_json::Value::MakeString(
                         [iso stringFromDate:NSDate.date].UTF8String ?: ""));
    if (_liveBackend)
        line.Set("project",
                 mcp_json::Value::MakeString(_liveBackend->ProjectPath()));
    line.Set("kind", mcp_json::Value::MakeString(kind));
    if (!entry.tool_name.empty())
        line.Set("tool", mcp_json::Value::MakeString(entry.tool_name));
    if (!entry.tool_args_json.empty())
        line.Set("arguments",
                 mcp_json::Value::MakeString(entry.tool_args_json));
    if (entry.kind == chat::ChatEntryKind::ToolResult)
        line.Set("ok", mcp_json::Value::MakeBool(entry.tool_ok));
    line.Set("text", mcp_json::Value::MakeString(entry.text));
    if (!entry.image_base64.empty())
        line.Set("image", mcp_json::Value::MakeString(entry.image_mime));
    std::ofstream log(_chatLogPath, std::ios::app | std::ios::binary);
    log << line.Dump() << '\n';
}

- (void)appendTranscriptEntry:(const chat::ChatTranscriptEntry&)entry {
    [self cmLogTranscriptEntry:entry];
    if (entry.kind == chat::ChatEntryKind::UserMessage) _planRow = nil;
    if (entry.kind == chat::ChatEntryKind::Plan && _planRow) {
        [_planRow cmSetText:[self cmDisplayTextForEntry:entry]];
        [self cmRelayoutTranscript];
        return;
    }
    CMChatRowView* row =
        [[CMChatRowView alloc] initWithText:[self cmDisplayTextForEntry:entry]
                                      style:[self cmStyleForEntry:entry]];
    __weak CMChatPanelView* weakSelf = self;
    [row setResizeHandler:^{
      [weakSelf cmRelayoutTranscript];
    }];
    if (entry.kind == chat::ChatEntryKind::Plan) _planRow = row;
    if (!entry.image_base64.empty()) {
        NSData* data = [[NSData alloc]
            initWithBase64EncodedString:@(entry.image_base64.c_str())
                                options:
                                    NSDataBase64DecodingIgnoreUnknownCharacters];
        NSImage* image = data ? [[NSImage alloc] initWithData:data] : nil;
        if (image) [row cmSetImage:image];
    }
    [_transcriptContainer addSubview:row];
    [self cmRelayoutTranscript];
    [self cmScrollToBottom];
    if (self.activityHandler) self.activityHandler();
}

- (void)cmScrollToBottom {
    NSPoint bottom = NSMakePoint(
        0, std::max<CGFloat>(0.0, _transcriptContainer.frame.size.height -
                                      _scrollView.contentSize.height));
    [_transcriptContainer scrollPoint:bottom];
}

// PRD25 -- see ChatPanelView.h. The rows are keyed on the journal's own IDs,
// which it never reuses: an ID it no longer has is a row to take away, one
// the panel has not seen is a row to add.
- (void)showAgentJournal:
    (const std::vector<chat::ChatTranscriptEntry>&)entries {
    std::set<uint64_t> kept;
    for (const chat::ChatTranscriptEntry& entry : entries)
        kept.insert(entry.journal_id);
    for (auto row = _journalRows.begin(); row != _journalRows.end();) {
        if (kept.count(row->first) != 0) {
            ++row;
            continue;
        }
        [row->second removeFromSuperview];
        row = _journalRows.erase(row);
    }
    bool added = false;
    for (const chat::ChatTranscriptEntry& entry : entries) {
        if (_journalRows.count(entry.journal_id) != 0) continue;
        CMChatRowView* row = [[CMChatRowView alloc]
            initWithText:[self cmDisplayTextForEntry:entry]
                   style:[self cmStyleForEntry:entry]];
        row.identifier = @"chat.agent_journal";
        [_transcriptContainer addSubview:row];
        _journalRows[entry.journal_id] = row;
        added = true;
    }
    [self cmRelayoutTranscript];
    if (added) [self cmScrollToBottom];
    if (added && self.activityHandler) self.activityHandler();
}

// The panel's own conversation goes when its engine changes; the relayed
// agent's journal stays, since it is not this engine's.
- (void)cmRemoveConversationRows {
    for (NSView* row in [_transcriptContainer.subviews copy])
        if (![row.identifier isEqualToString:@"chat.agent_journal"])
            [row removeFromSuperview];
}

- (void)configurePressed:(id)sender {
    (void)sender;
    [self showConfigurationWindow];
}

// PRD22 -- the form opens on the protocol chosen, each field holding what
// is stored for it (ai_engine::DraftsFromStored): a field with nothing
// stored is empty, its placeholder showing what the engine takes instead --
// the environment's Anthropic model and URL, or the built-in defaults. It
// used to open with the model in use written in, which saving then stored.
- (CMAiEngineFormView*)cmMakeEngineForm {
    NSUserDefaults* stored = NSUserDefaults.standardUserDefaults;
    const ai_engine::EngineProtocol protocol =
        ai_engine::ProtocolOfStoredProvider(
            CMUtf8([stored stringForKey:kAiProviderDefaultsKey]));
    const ai_engine::FormDrafts drafts = ai_engine::DraftsFromStored(
        protocol, CMUtf8([stored stringForKey:kAiModelDefaultsKey]),
        CMUtf8([stored stringForKey:kAiBaseUrlDefaultsKey]),
        CMUtf8([stored stringForKey:kAiAcpCommandDefaultsKey]));
    const chat::ChatLlmConfig environment =
        chat::ChatLlmConfig::FromEnvironment();
    ai_engine::FormContext context;
    context.anthropic_model = environment.model;
    context.anthropic_base_url = environment.base_url;
    context.anthropic_key_stored =
        !environment.api_key.empty() ||
        AiKeyStored(chat::ChatLlmProvider::AnthropicMessages);
    context.ollama_key_stored = AiKeyStored(chat::ChatLlmProvider::Ollama);
    return [[CMAiEngineFormView alloc] initWithProtocol:protocol
                                                 drafts:drafts
                                                context:context];
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. On the window's form panel, a
// sheet. The URL's form and the key's way into the Keychain are checked
// before it closes: refused, the panel stays open on what was typed and
// says why, where a second alert reopened the form.
- (void)showConfigurationWindow {
    if (_turnInFlight) {
        NSBeep();
        return;
    }
    CMAiEngineFormView* form = [self cmMakeEngineForm];
    CMFormPanel* panel = [form makePanel];
    NSUserDefaults* stored = NSUserDefaults.standardUserDefaults;
    NSCharacterSet* blanks = NSCharacterSet.whitespaceAndNewlineCharacterSet;
    // An empty URL is the protocol's default, the one its placeholder
    // showed (AiEngineForm.h): nothing is stored for it.
    NSString* (^typedBaseUrl)(void) = ^NSString* {
      NSString* url = [CMString(form.texts.base_url)
          stringByTrimmingCharactersInSet:blanks];
      while ([url hasSuffix:@"/"]) url = [url substringToIndex:url.length - 1];
      return url;
    };
    const auto providerOf = [](ai_engine::EngineProtocol protocol) {
        return protocol == ai_engine::EngineProtocol::Ollama
                   ? chat::ChatLlmProvider::Ollama
                   : chat::ChatLlmProvider::AnthropicMessages;
    };
    panel.check = ^NSString*(NSModalResponse response) {
      (void)response;
      const ai_engine::EngineProtocol protocol = form.selectedProtocol;
      if (protocol == ai_engine::EngineProtocol::Acp) return nil;
      const ai_engine::FormTexts texts = form.texts;
      NSString* selectedBaseUrl = typedBaseUrl();
      if (selectedBaseUrl.length > 0) {
          NSURLComponents* components =
              [NSURLComponents componentsWithString:selectedBaseUrl];
          const BOOL validScheme =
              [components.scheme.lowercaseString isEqualToString:@"https"] ||
              [components.scheme.lowercaseString isEqualToString:@"http"];
          if (!validScheme || components.host.length == 0)
              return @"Renseignez une URL HTTP(S) complète, sans le chemin "
                      "final /messages ou /chat/completions, ou laissez le "
                      "champ vide pour l'URL proposée.";
      }
      const chat::ChatLlmProvider selectedProvider = providerOf(protocol);
      std::string keychainError;
      if (form.removesStoredKey) {
          if (!WriteAiApiKey(selectedProvider, {}, keychainError))
              return [@"Trousseau inaccessible : "
                  stringByAppendingString:CMString(keychainError)];
      } else if (!texts.api_key.empty()) {
          if (!WriteAiApiKey(selectedProvider, texts.api_key, keychainError))
              return [@"Trousseau inaccessible : "
                  stringByAppendingString:CMString(keychainError)];
      }
      return nil;
    };
    [panel
        presentInWindow:self.formWindow ?: self.window
             completion:^(NSModalResponse response) {
               if (response != NSAlertFirstButtonReturn) return;
               const ai_engine::EngineProtocol protocol = form.selectedProtocol;
               const ai_engine::FormTexts texts = form.texts;
               if (protocol == ai_engine::EngineProtocol::Acp) {
                   // An empty command is the Claude adapter's.
                   NSString* command = [CMString(texts.acp_command)
                       stringByTrimmingCharactersInSet:blanks];
                   [stored setObject:kAiProviderAcp
                              forKey:kAiProviderDefaultsKey];
                   [stored setObject:command forKey:kAiAcpCommandDefaultsKey];
               } else {
                   // An empty model is the protocol's default too.
                   NSString* selectedModel = [CMString(texts.model)
                       stringByTrimmingCharactersInSet:blanks];
                   NSString* selectedBaseUrl = typedBaseUrl();
                   [stored setObject:providerOf(protocol) ==
                                             chat::ChatLlmProvider::Ollama
                                         ? @"ollama"
                                         : @"anthropic-messages"
                              forKey:kAiProviderDefaultsKey];
                   if (selectedModel.length > 0)
                       [stored setObject:selectedModel
                                  forKey:kAiModelDefaultsKey];
                   else
                       [stored removeObjectForKey:kAiModelDefaultsKey];
                   if (selectedBaseUrl.length > 0)
                       [stored setObject:selectedBaseUrl
                                  forKey:kAiBaseUrlDefaultsKey];
                   else
                       [stored removeObjectForKey:kAiBaseUrlDefaultsKey];
               }
               // PRD25 -- a new engine clears the conversation, never the
               // agent journal's lines.
               [self cmRemoveConversationRows];
               [self cmReloadConfiguration];
               [self cmRelayoutTranscript];
               [self.window makeFirstResponder:self->_inputField];
             }];
}

- (void)focusInput {
    [self.window makeFirstResponder:_inputField];
}

- (void)sendPressed:(id)sender {
    // Visual QA of 19 September 2026 -- while a turn runs the button stops
    // it: the session ends the turn before its next call, and the stop
    // handler cancels the media tasks the turn is waiting on.
    if (_turnInFlight && sender == _sendButton) {
        if (_session) _session->RequestStop();
        if (_acpSession) _acpSession->RequestStop();
        if (_stopHandler) _stopHandler();
        _sendButton.title = @"Arrêt…";
        _sendButton.enabled = NO;
        return;
    }
    if (_turnInFlight || (!_session && !_acpSession)) return;
    NSString* text = [_inputField.stringValue
        stringByTrimmingCharactersInSet:NSCharacterSet
                                            .whitespaceAndNewlineCharacterSet];
    const BOOL shortPreset = _presetPopup.indexOfSelectedItem == 1;
    if (text.length == 0 && !shortPreset) return;
    if (!_missingKeyError.empty()) {
        _statusLabel.hidden = NO;
        [self cmLayoutForSize:self.bounds.size];
        return;
    }

    _inputField.stringValue = @"";
    _inputField.enabled = NO;
    _sendButton.title = @"Arrêter";
    _sendButton.enabled = YES;
    _turnInFlight = YES;

    std::string instruction = text.UTF8String ? text.UTF8String : "";
    if (shortPreset) {
        instruction =
            "Exécute le preset « Short interview dynamique — 60 s ». "
            "Appelle d'abord get_timeline_transcript. Choisis uniquement des "
            "spans présents, pour une durée totale cible de 50 à 65 secondes. "
            "Réordonne-les pour ouvrir sur l'accroche la plus forte, garder "
            "un développement concis et finir sur une chute claire. Élimine "
            "les répétitions et les digressions. Copie source_id, source_in "
            "et duration exactement, sans calcul ni modification, puis "
            "appelle create_interview_short une seule fois. Ne modifie pas "
            "la timeline d'origine.";
        if (text.length > 0)
            instruction += " Consigne éditoriale supplémentaire : " +
                           std::string(text.UTF8String ?: "");
    }
    // A Stop pressed as the previous turn finished must not stop this one;
    // one pressed from now on does, even before the worker starts.
    if (_session) _session->ClearStopRequest();
    if (_acpSession) _acpSession->ClearStopRequest();
    chat::ChatSession* session = _session.get();
    acp::AcpChatSession* agent = _acpSession.get();
    MainThreadBackend* backend = _mainThreadBackend.get();
    __weak CMChatPanelView* weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      std::string error;
      if (agent) {
          // PL1 -- one Undo per turn, as the built-in loop does: the agent's
          // edits reach the same live backend through the window's endpoint.
          const chat::TurnIntervention turn(*backend,
                                            chat::TurnLabel(instruction));
          agent->SubmitUserMessage(instruction, error);
      } else {
          session->SubmitUserMessage(instruction, error);
      }
      // Failures are already in the transcript as an Error entry.
      dispatch_async(dispatch_get_main_queue(), ^{
        CMChatPanelView* strongSelf = weakSelf;
        if (!strongSelf) return;
        strongSelf->_inputField.enabled = YES;
        strongSelf->_sendButton.title = @"Envoyer";
        strongSelf->_sendButton.enabled = YES;
        strongSelf->_turnInFlight = NO;
        [strongSelf.window makeFirstResponder:strongSelf->_inputField];
      });
    });
}

@end

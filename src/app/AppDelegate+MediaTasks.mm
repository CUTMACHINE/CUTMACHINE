// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The media
// library's work in the background (MediaTaskManager.h): imports and their
// probes, relinks, proxies, thumbnails, waveforms and transcriptions,
// asked from the window, then committed to the document when they finish.
// Moved out of main.mm unchanged.

#include "AppDelegatePrivate.h"

@implementation AppDelegate (MediaTasks)

- (void)relinkSelectedMedia:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    if (!identifier) return;
    const Ulid mediaId(identifier.UTF8String ?: "");
    const LibraryMedia* existing =
        self.state->document.FindLibraryMedia(mediaId);
    if (!existing) return;
    for (const auto& pending : self.state->pendingRelinks) {
        if (pending.second.media_id == mediaId) {
            self.binSummaryLabel.stringValue =
                @"Une reconnexion est déjà en cours pour ce média.";
            return;
        }
    }
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title =
        [NSString stringWithFormat:@"Reconnecter %@",
                                   CMStringFromUtf8(existing->filename)];
    panel.prompt = @"Reconnecter";
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.resolvesAliases = YES;
    panel.allowedContentTypes = @[ UTTypeMovie ];
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return;

    std::error_code pathError;
    const std::filesystem::path absolute = std::filesystem::weakly_canonical(
        std::filesystem::path(panel.URL.fileSystemRepresentation ?: ""),
        pathError);
    if (pathError) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Fichier inaccessible : %@",
                                       CMStringFromUtf8(pathError.message())];
        return;
    }
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    // LIEN1 -- docs/roadmap/IMPORT-2026-10.md. The file as it was picked,
    // links kept: a rush picked through a link beside the package keeps it.
    const std::string stored = StoredMediaPath(
        std::filesystem::path(panel.URL.fileSystemRepresentation ?: ""), base);
    auto result = std::make_shared<RelinkProbeResult>();
    result->media.id = mediaId;
    result->media.path = stored;
    result->media.filename = absolute.filename().string();
    const Document snapshot = self.state->document;
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Relink, "Relink " + existing->filename,
        [absolute, mediaId, snapshot, result](MediaTaskContext& context,
                                              std::string& taskError) {
            if (context.Cancelled()) {
                taskError = "relink cancelled";
                return false;
            }
            context.SetProgress(0.1, "Analyse du remplacement");
            if (!ProbeMediaMetadata(absolute.string(), result->media,
                                    taskError)) {
                result->error = taskError;
                return false;
            }
            // COLOR-T -- the file moves; how its picture is read does not.
            if (const LibraryMedia* original =
                    snapshot.FindLibraryMedia(mediaId))
                KeepMediaReading(*original, result->media);
            if (context.Cancelled()) {
                taskError = "relink cancelled";
                return false;
            }
            context.SetProgress(0.8, "Vérification de compatibilité");
            if (!ValidateRelinkCandidate(snapshot, mediaId, result->media,
                                         taskError)) {
                result->error = taskError;
                return false;
            }
            context.SetProgress(1.0, "Remplacement compatible");
            return true;
        });
    self.state->pendingRelinks.emplace(
        taskId, PendingRelink{mediaId, absolute, stored, result});
    self.binSummaryLabel.stringValue =
        [NSString stringWithFormat:@"Vérification de %@…",
                                   CMStringFromUtf8(absolute.filename())];
    [self refreshMediaTaskStatus];
}

- (void)batchRelinkOfflineMedia:(id)sender {
    (void)sender;
    if (!self.state || self.state->offlineSourceIds.empty() ||
        !self.state->pendingBatchRelinks.empty())
        return;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Dossier contenant les médias originaux";
    panel.prompt = @"Rechercher et reconnecter";
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.resolvesAliases = YES;
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return;

    std::error_code pathError;
    // LIEN1 -- docs/roadmap/IMPORT-2026-10.md. The folder as it was picked,
    // links kept: what the relinked rushes store (StoredMediaPath). A rush
    // found under a link beside the package keeps the link in its path.
    const std::filesystem::path folder =
        std::filesystem::absolute(
            std::filesystem::path(panel.URL.fileSystemRepresentation ?: ""),
            pathError)
            .lexically_normal();
    if (pathError) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Dossier inaccessible : %@",
                                       CMStringFromUtf8(pathError.message())];
        return;
    }
    const Document snapshot = self.state->document;
    const std::vector<Ulid> offlineIds(self.state->offlineSourceIds.begin(),
                                       self.state->offlineSourceIds.end());
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    auto result = std::make_shared<BatchRelinkResult>();
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Relink, "Relink dossier " + folder.filename().string(),
        [folder, base, snapshot, offlineIds, result](MediaTaskContext& context,
                                                     std::string& taskError) {
            const auto keyFor = [](std::string value) {
                std::transform(
                    value.begin(), value.end(), value.begin(),
                    [](unsigned char character) {
                        return static_cast<char>(std::tolower(character));
                    });
                return value;
            };
            std::map<std::string, std::vector<std::filesystem::path>> files;
            std::error_code scanError;
            std::filesystem::recursive_directory_iterator iterator(
                folder,
                std::filesystem::directory_options::skip_permission_denied,
                scanError),
                end;
            while (!scanError && iterator != end) {
                if (context.Cancelled()) {
                    taskError = "batch relink cancelled";
                    return false;
                }
                std::error_code fileError;
                if (IsVolumeSystemDirectory(iterator->path()))
                    iterator.disable_recursion_pending();
                else if (iterator->is_regular_file(fileError) && !fileError &&
                         !IsVolumeLitter(iterator->path()))
                    files[keyFor(iterator->path().filename().string())]
                        .push_back(iterator->path());
                iterator.increment(scanError);
            }
            if (scanError) {
                taskError =
                    "unable to scan relink folder: " + scanError.message();
                return false;
            }
            for (size_t index = 0; index < offlineIds.size(); ++index) {
                if (context.Cancelled()) {
                    taskError = "batch relink cancelled";
                    return false;
                }
                const LibraryMedia* original =
                    snapshot.FindLibraryMedia(offlineIds[index]);
                if (!original) continue;
                const auto found = files.find(keyFor(original->filename));
                if (found == files.end()) {
                    ++result->unmatched;
                } else if (found->second.size() != 1) {
                    ++result->ambiguous;
                } else {
                    std::error_code canonicalError;
                    const std::filesystem::path absolute =
                        std::filesystem::weakly_canonical(found->second.front(),
                                                          canonicalError);
                    if (canonicalError) {
                        ++result->incompatible;
                        result->last_error = canonicalError.message();
                    } else {
                        const std::string stored =
                            StoredMediaPath(found->second.front(), base);
                        LibraryMedia replacement;
                        replacement.id = original->id;
                        replacement.path = stored;
                        replacement.filename = absolute.filename().string();
                        std::string probeError;
                        const bool probed = ProbeMediaMetadata(
                            absolute.string(), replacement, probeError);
                        // COLOR-T -- as the single relink does.
                        if (probed) KeepMediaReading(*original, replacement);
                        if (!probed ||
                            !ValidateRelinkCandidate(snapshot, original->id,
                                                     replacement, probeError)) {
                            ++result->incompatible;
                            result->last_error = probeError;
                        } else {
                            result->replacements.push_back(
                                {original->id, std::move(replacement), stored});
                        }
                    }
                }
                context.SetProgress(
                    static_cast<double>(index + 1) / offlineIds.size(),
                    original->filename);
            }
            return true;
        });
    self.state->pendingBatchRelinks.emplace(taskId, PendingBatchRelink{result});
    self.binSummaryLabel.stringValue =
        [NSString stringWithFormat:@"Recherche de %lu média%@ offline…",
                                   (unsigned long)offlineIds.size(),
                                   offlineIds.size() == 1 ? @"" : @"s"];
    [self refreshMediaTaskStatus];
}

- (void)importMediaPressed:(id)sender {
    (void)sender;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Importer des rushes";
    panel.prompt = @"Importer";
    panel.allowsMultipleSelection = YES;
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = YES;
    panel.resolvesAliases = YES;
    NSString* target =
        self.state->document.FindBin(self.selectedBinId.UTF8String ?: "")
            ? self.selectedBinId
            : @"";
    if ([panel runModal] == NSModalResponseOK)
        [self importMediaURLs:panel.URLs intoBin:target];
}

- (void)importSubtitlesPressed:(id)sender {
    (void)sender;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.title = @"Importer des sous-titres SubRip";
    panel.prompt = @"Importer";
    panel.allowsMultipleSelection = NO;
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowedContentTypes = @[ [UTType typeWithFilenameExtension:@"srt"] ];
    if ([panel runModal] != NSModalResponseOK || !panel.URL) return;

    std::vector<SubtitleCue> cues;
    std::string message;
    if (!LoadSrt(panel.URL.fileSystemRepresentation ?: "", cues, message)) {
        NSAlert* alert = [NSAlert new];
        alert.alertStyle = NSAlertStyleCritical;
        alert.messageText = @"Import SRT impossible";
        alert.informativeText =
            [NSString stringWithUTF8String:message.c_str()] ?: @"SRT invalide";
        [alert runModal];
        return;
    }
    int32_t trackIndex = 0;
    for (const DocumentTrack& track : self.state->document.sequence.tracks)
        trackIndex = std::max(trackIndex, track.index + 1);
    EditError error = EditError::None;
    AddTrackOperation operation = BuildSubtitleTrackEdit(cues, trackIndex);
    if (![self applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                          error:error
                                        message:message]) {
        NSAlert* alert = [NSAlert new];
        alert.alertStyle = NSAlertStyleCritical;
        alert.messageText = @"Import SRT impossible";
        alert.informativeText = [NSString stringWithUTF8String:message.c_str()]
                                    ?: @"Erreur inconnue";
        [alert runModal];
        return;
    }
    [self refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
    self.state->overlayDirty = true;
    self.infoLabel.stringValue =
        [NSString stringWithFormat:@"%lu sous-titre%@ importé%@",
                                   (unsigned long)cues.size(),
                                   cues.size() == 1 ? @"" : @"s",
                                   cues.size() == 1 ? @"" : @"s"];
}

- (BOOL)importMediaURLs:(NSArray<NSURL*>*)urls intoBin:(NSString*)binId {
    const std::string targetBin(binId.UTF8String ?: "");
    if (!targetBin.empty() && !self.state->document.FindBin(targetBin))
        return NO;

    std::vector<std::filesystem::path> candidates;
    for (NSURL* url in urls) {
        if (!url.isFileURL) continue;
        std::filesystem::path path(url.path.UTF8String ?: "");
        std::error_code error;
        if (std::filesystem::is_directory(path, error) && !error) {
            std::filesystem::recursive_directory_iterator iterator(
                path,
                std::filesystem::directory_options::skip_permission_denied,
                error),
                end;
            for (; !error && iterator != end; iterator.increment(error)) {
                if (IsVolumeSystemDirectory(iterator->path())) {
                    iterator.disable_recursion_pending();
                    continue;
                }
                if (iterator->is_regular_file(error) && !error &&
                    !IsVolumeLitter(iterator->path()))
                    candidates.push_back(iterator->path());
            }
        } else if (!error && std::filesystem::is_regular_file(path, error)) {
            candidates.push_back(path);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());
    if (candidates.empty()) {
        self.binSummaryLabel.stringValue = @"Aucun fichier à importer.";
        return NO;
    }

    auto batch = std::make_shared<ProbedImportBatch>();
    batch->document_path = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    batch->target_bin = targetBin;
    // PRD7 -- docs/roadmap/PRODUIT-2026-10.md. The engine's ingest reads the
    // files (ProbeIngestFile: headers, then the audio level, which decodes
    // the sound), here on the task's thread. The library is read now, on the
    // main thread, for the files it already holds complete: nothing is
    // decoded for those. Cancelling is heard between two files, not inside
    // one measurement. F2.4 -- the same reading as an agent's ingest_media
    // (ProbeIngestFiles, McpLiveBackend.h).
    const std::set<std::string> complete = CompleteLibraryPaths(
        self.state->document, batch->document_path.parent_path());
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Probe, IngestTaskLabel(candidates.size()),
        [batch, candidates, complete](MediaTaskContext& context,
                                      std::string& error) {
            if (ProbeIngestFiles(candidates, complete, batch->files,
                                 &context)) {
                return true;
            }
            error = "import cancelled";
            return false;
        });
    self.state->pendingImports[taskId] = std::move(batch);
    self.binSummaryLabel.stringValue =
        [NSString stringWithFormat:@"Analyse de %lu fichier%@ en arrière-plan…",
                                   (unsigned long)candidates.size(),
                                   candidates.size() == 1 ? @"" : @"s"];
    [self refreshMediaTaskStatus];
    return YES;
}

- (void)generateSelectedMediaProxy:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    [self enqueueProxyForMediaIdentifier:identifier];
}

- (void)transcribeSelectedMedia:(id)sender {
    (void)sender;
    [self transcribeSelectedMediaFrenchVerbatim:NO];
}

- (void)transcribeSelectedMediaHesitations:(id)sender {
    (void)sender;
    [self transcribeSelectedMediaFrenchVerbatim:YES];
}

// AL4 -- docs/roadmap/ALIGNEMENT-2026-11.md. « Transcrire avec Whisper… »
// names no profile and is neither French nor verbatim: `default`. « Transcrire
// les hésitations (français)… » asks for the euh: it names fr-hesitations-v1
// on the hesitations weights, and refuses with an alert without them, never
// a silent `default` (whisper_model_setting::HesitationsWeightsProblem).
- (void)transcribeSelectedMediaFrenchVerbatim:(BOOL)frenchVerbatim {
    NSString* identifier = [self selectedMediaId];
    if (!identifier || !self.state) return;
    if (frenchVerbatim && ![self hesitationsWeightsAllowed]) return;
    NSString* modelPath = [self transcriptionModelPath];
    if (!modelPath) return;
    WhisperSettings settings;
    settings.whisper_model_path = modelPath.UTF8String ?: "";
    if (frenchVerbatim) {
        settings.language = "fr";
        settings.verbatim = true;
    }
    [self enqueueTranscriptionForMediaIdentifier:identifier
                                        settings:settings
                                         batchId:Ulid()
                                      reuseCache:NO
                                     hesitations:frenchVerbatim];
}

- (void)transcribeTimeline:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self transcribeTimelineFrenchVerbatim:NO];
}

- (void)transcribeTimelineHesitations:(id)sender {
    if ([self charterRefusesMontageCommand:sender]) return;
    [self transcribeTimelineFrenchVerbatim:YES];
}

- (void)transcribeTimelineFrenchVerbatim:(BOOL)frenchVerbatim {
    if (!self.state) return;
    if (frenchVerbatim && ![self hesitationsWeightsAllowed]) return;
    NSString* modelPath = [self transcriptionModelPath];
    if (!modelPath) return;

    const bool hasSolo =
        std::any_of(self.state->document.sequence.tracks.begin(),
                    self.state->document.sequence.tracks.end(),
                    [](const DocumentTrack& track) {
                        return track.kind == "audio" && track.solo;
                    });
    std::set<Ulid> mediaIds;
    for (const DocumentTrack& track : self.state->document.sequence.tracks) {
        if (track.kind != "audio" || track.muted || (hasSolo && !track.solo))
            continue;
        for (const DocumentClip& clip : track.clips) {
            // A disabled clip is not heard (ClipEnabled.h).
            if (!ClipIsHeard(clip)) continue;
            const LibraryMedia* media =
                self.state->document.FindLibraryMedia(clip.source_id);
            if (media && media->has_audio) mediaIds.insert(clip.source_id);
        }
    }
    if (mediaIds.empty()) {
        self.binSummaryLabel.stringValue =
            @"La timeline ne contient aucune piste audio active.";
        return;
    }
    for (const auto& pending : self.state->pendingTranscriptions) {
        if (mediaIds.count(pending.second.media_id)) {
            self.binSummaryLabel.stringValue =
                @"Une source de cette timeline est déjà en transcription.";
            return;
        }
    }

    const Ulid batchId = GenerateUlid();
    PendingTimelineTranscription batch;
    batch.timeline_id = self.state->activeTimelineId;
    batch.hesitations = frenchVerbatim;
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path transcriptDirectory =
        projectPath.parent_path() / ".cutmachine" / "transcripts";
    WhisperSettings requestedSettings;
    requestedSettings.whisper_model_path = modelPath.UTF8String ?: "";
    if (frenchVerbatim) {
        requestedSettings.language = "fr";
        requestedSettings.verbatim = true;
    }
    // AL4 -- the profile is part of the cache identity. When only reading
    // the weights tells it, the main thread does not: each task does,
    // then reuses the cache that matches.
    const WhisperDecodingRequest request =
        frenchVerbatim ? WhisperDecodingRequest(
                             WhisperDecodingProfile::FrenchHesitationsV1)
                       : WhisperDecodingRequest();
    const std::optional<WhisperDecodingChoice> known =
        KnownWhisperDecodingChoice(request, requestedSettings.language,
                                   requestedSettings.verbatim,
                                   requestedSettings.whisper_model_path,
                                   ConfiguredWhisperHesitationsModel());
    WhisperSettings knownSettings = requestedSettings;
    if (known) {
        knownSettings.decoding_profile = known->profile;
        knownSettings.whisper_model_path = known->model_path;
    }
    for (const Ulid& mediaId : mediaIds) {
        const std::filesystem::path cachedPath =
            transcriptDirectory / (mediaId + ".json");
        Transcript cached;
        std::string cacheError;
        if (known &&
            LoadAudioTranscript(cachedPath.string(), cached, cacheError) &&
            TranscriptCacheMatches(cached, mediaId, knownSettings)) {
            batch.transcript_paths.emplace(mediaId, cachedPath);
            continue;
        }
        NSString* identifier = [NSString stringWithUTF8String:mediaId.c_str()];
        const Ulid taskId =
            [self enqueueTranscriptionForMediaIdentifier:identifier
                                                settings:requestedSettings
                                                 batchId:batchId
                                              reuseCache:!known
                                             hesitations:frenchVerbatim];
        if (taskId.empty()) {
            batch.error = "unable to enqueue every timeline source";
            break;
        }
        batch.task_ids.insert(taskId);
        batch.transcript_paths.emplace(
            mediaId,
            self.state->pendingTranscriptions.at(taskId).absolute_path);
    }
    if (batch.transcript_paths.empty()) {
        self.binSummaryLabel.stringValue =
            @"Impossible de démarrer la transcription de la timeline.";
        return;
    }
    const bool cachedOnly = batch.task_ids.empty();
    self.state->pendingTimelineTranscriptions.emplace(batchId,
                                                      std::move(batch));
    self.binSummaryLabel.stringValue =
        cachedOnly
            ? @"Transcripts en cache · création de S1…"
            : [NSString stringWithFormat:@"Transcription de la timeline · %lu "
                                          "source%@…",
                                         (unsigned long)mediaIds.size(),
                                         mediaIds.size() == 1 ? @"" : @"s"];
}

- (Ulid)enqueueTranscriptionForMediaIdentifier:(NSString*)identifier
                                      settings:(const WhisperSettings&)settings
                                       batchId:(const Ulid&)batchId
                                    reuseCache:(BOOL)reuseCache
                                   hesitations:(BOOL)hesitations {
    if (!identifier || settings.whisper_model_path.empty() || !self.state)
        return {};
    const Ulid mediaId(identifier.UTF8String ?: "");
    const LibraryMedia* media = self.state->document.FindLibraryMedia(mediaId);
    const DocumentSource* source = self.state->document.FindSource(mediaId);
    if (!media || !source || !media->has_audio) return {};
    for (const auto& pending : self.state->pendingTranscriptions) {
        if (pending.second.media_id == mediaId) {
            self.binSummaryLabel.stringValue =
                @"Une transcription est déjà en cours pour ce média.";
            return {};
        }
    }

    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    std::filesystem::path input(media->path);
    if (input.is_relative()) input = base / input;
    input = input.lexically_normal();
    const std::filesystem::path output =
        base / ".cutmachine" / "transcripts" / (mediaId + ".json");
    const bool reuse = reuseCache;
    const bool euh = hesitations;
    const whisper_model_setting::HesitationsFacts facts =
        whisper_model_setting::CurrentHesitationsFacts();
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Transcription, "Whisper " + media->filename,
        [input, output, mediaId, rate = source->rate, settings, reuse, euh,
         facts](MediaTaskContext& context, std::string& taskError) {
            // AL4 -- off the main thread: recognizing the weights reads them.
            // The hesitations action runs on its weights or not at all.
            if (euh && !whisper_model_setting::HesitationsWeightsReady(
                           facts, context, taskError))
                return false;
            WhisperSettings resolved = settings;
            WhisperDecodingChoice choice;
            const WhisperDecodingRequest request =
                euh ? WhisperDecodingRequest(
                          WhisperDecodingProfile::FrenchHesitationsV1)
                    : WhisperDecodingRequest();
            if (!ResolveWhisperSettings(request, facts.path, context, resolved,
                                        choice, taskError))
                return false;
            Transcript cached;
            std::string cacheError;
            if (reuse &&
                LoadAudioTranscript(output.string(), cached, cacheError) &&
                TranscriptCacheMatches(cached, mediaId, resolved))
                return true;
            return GenerateAudioTranscript(input.string(), output.string(),
                                           mediaId, rate, resolved, context,
                                           taskError);
        });
    self.state->pendingTranscriptions.emplace(
        taskId, PendingTranscription{mediaId, self.state->activeTimelineId,
                                     batchId, output, hesitations == YES});
    self.binSummaryLabel.stringValue =
        [NSString stringWithFormat:@"Transcription de %@ en arrière-plan…",
                                   CMStringFromUtf8(media->filename)];
    [self refreshMediaTaskStatus];
    return taskId;
}

- (void)regenerateSelectedMediaThumbnail:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    if (!identifier) return;
    [self.mediaThumbnails removeObjectForKey:identifier];
    [self enqueueThumbnailForMediaIdentifier:identifier];
}

- (void)enqueueProxyForMediaIdentifier:(NSString*)identifier {
    if (!identifier) return;
    const Ulid mediaId(identifier.UTF8String ?: "");
    const LibraryMedia* media = self.state->document.FindLibraryMedia(mediaId);
    const DocumentSource* source = self.state->document.FindSource(mediaId);
    if (!media || !source) return;
    if (!media->has_video) {
        self.binSummaryLabel.stringValue =
            @"Ce média est audio seul · aucun proxy vidéo à générer.";
        return;
    }
    for (const auto& pending : self.state->pendingProxies) {
        if (pending.second.media_id == mediaId) {
            self.binSummaryLabel.stringValue = @"Un proxy est déjà en cours.";
            return;
        }
    }
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    std::filesystem::path input(media->path);
    if (input.is_relative()) input = base / input;
    input = input.lexically_normal();
    const std::filesystem::path output =
        base / ".cutmachine" / "proxies" / (mediaId + ".mov");
    std::error_code error;
    std::string stored = std::filesystem::relative(output, base, error)
                             .lexically_normal()
                             .string();
    if (error) stored = output.string();
    ProxySettings settings;
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Proxy, "Proxy " + media->filename,
        [input, output, duration = source->duration, settings](
            MediaTaskContext& context, std::string& taskError) {
            return GenerateMediaProxy(input.string(), output.string(), duration,
                                      settings, context, taskError);
        });
    self.state->pendingProxies.emplace(
        taskId, PendingProxy{mediaId, output, std::move(stored)});
    self.binSummaryLabel.stringValue =
        [NSString stringWithFormat:@"Génération du proxy de %@…",
                                   CMStringFromUtf8(media->filename)];
    [self refreshMediaTaskStatus];
}

- (void)removeSelectedMediaProxy:(id)sender {
    (void)sender;
    NSString* identifier = [self selectedMediaId];
    const Ulid mediaId(identifier.UTF8String ?: "");
    const LibraryMedia* media = self.state->document.FindLibraryMedia(mediaId);
    if (!media || media->proxy_path.empty()) return;
    for (const auto& pending : self.state->pendingProxies)
        if (pending.second.media_id == mediaId)
            self.state->mediaTasks->Cancel(pending.first);
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    const std::string previousProxyPath = media->proxy_path;
    std::filesystem::path stored(previousProxyPath);
    if (stored.is_relative()) stored = base / stored;
    const std::filesystem::path expected =
        base / ".cutmachine" / "proxies" / (mediaId + ".mov");
    // Detaching is an edit (SetMediaProxyOperation), undone like any other;
    // `media` is not read past this point, the document being replaced.
    std::string message;
    EditError editError = EditError::None;
    if (![self applyAndPersistTimelineOperation:Operation {
            SetMediaProxyOperation { mediaId, std::string() }
        }
                                          error:editError
                                        message:message]) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Suppression du proxy impossible : %@",
                                       CMStringFromUtf8(message)];
        return;
    }
    if (stored.lexically_normal() == expected.lexically_normal()) {
        std::error_code ignored;
        std::filesystem::remove(expected, ignored);
    }
    [self reloadDecodeWorkers];
    [self rebuildMediaList];
    self.binSummaryLabel.stringValue = @"Proxy supprimé · original actif";
}

- (void)refreshMediaTaskStatus {
    if (!self.state || !self.state->mediaTasks || !self.mediaTaskLabel) return;
    const std::vector<MediaTaskSnapshot> tasks =
        self.state->mediaTasks->Snapshot();
    // PRD37 -- ROADMAP.md. Which task, and what is said of it, in French,
    // by MediaPanelModel.h: the engine's English word for the state never
    // shows, and a task over leaves no bar behind.
    const MediaTaskSnapshot* shown = ui::media_panel::ShownMediaTask(tasks);
    const ui::media_panel::MediaTaskLine line =
        ui::media_panel::DescribeMediaTask(shown);
    self.displayedMediaTaskId =
        shown != nullptr ? [NSString stringWithUTF8String:shown->id.c_str()]
                         : nil;
    self.mediaTaskLabel.stringValue = CMStringFromUtf8(line.text);
    self.mediaTaskProgress.hidden = !line.shows_progress;
    self.mediaTaskProgress.doubleValue = line.progress * 100.0;
    self.mediaTaskCancelButton.enabled = line.cancellable;
}

- (void)cancelDisplayedMediaTask:(id)sender {
    (void)sender;
    if (!self.displayedMediaTaskId || !self.state->mediaTasks) return;
    self.state->mediaTasks->Cancel(self.displayedMediaTaskId.UTF8String ?: "");
    [self refreshMediaTaskStatus];
}

- (void)processCompletedMediaImports {
    if (!self.state || self.state->pendingImports.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    for (auto item = self.state->pendingImports.begin();
         item != self.state->pendingImports.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        if (task->second.state == MediaTaskState::Succeeded) {
            [self commitMediaImportBatch:item->second.get()];
        } else if (task->second.state == MediaTaskState::Cancelled) {
            self.binSummaryLabel.stringValue = @"Import média annulé.";
        } else {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Import média échoué : %@",
                                 CMStringFromUtf8(task->second.error)];
        }
        item = self.state->pendingImports.erase(item);
    }
}

- (void)processCompletedMediaProxies {
    if (!self.state || self.state->pendingProxies.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    bool reload = false;
    for (auto item = self.state->pendingProxies.begin();
         item != self.state->pendingProxies.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        if (task->second.state == MediaTaskState::Succeeded) {
            const LibraryMedia* media =
                self.state->document.FindLibraryMedia(item->second.media_id);
            if (media) {
                // Attaching is an edit (SetMediaProxyOperation): the
                // filename is read first, the document being replaced.
                const std::string filename = media->filename;
                std::string message;
                EditError editError = EditError::None;
                if ([self applyAndPersistTimelineOperation:Operation {
                        SetMediaProxyOperation {
                            item->second.media_id, item->second.stored_path
                        }
                    }
                                                     error:editError
                                                   message:message]) {
                    self.binSummaryLabel.stringValue =
                        [NSString stringWithFormat:@"Proxy prêt · %@",
                                                   CMStringFromUtf8(filename)];
                    reload = true;
                } else {
                    std::error_code ignored;
                    std::filesystem::remove(item->second.absolute_path,
                                            ignored);
                    self.binSummaryLabel.stringValue = [NSString
                        stringWithFormat:@"Proxy créé mais non attaché : %@",
                                         CMStringFromUtf8(message)];
                }
            }
        } else if (task->second.state == MediaTaskState::Cancelled) {
            self.binSummaryLabel.stringValue = @"Génération proxy annulée.";
        } else {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Échec du proxy : %@",
                                 CMStringFromUtf8(task->second.error)];
        }
        item = self.state->pendingProxies.erase(item);
    }
    if (reload) {
        [self reloadDecodeWorkers];
        [self rebuildMediaList];
    }
}

- (void)loadOrEnqueueWaveformForMediaIdentifier:(NSString*)identifier {
    if (!identifier || !self.state) return;
    const Ulid mediaId(identifier.UTF8String ?: "");
    if (self.state->waveforms.count(mediaId) != 0) return;
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path output = projectPath.parent_path() /
                                         ".cutmachine" / "waveforms" /
                                         (mediaId + ".waveform");
    AudioWaveform waveform;
    std::string error;
    if (LoadAudioWaveform(output.string(), waveform, error)) {
        self.state->waveforms.emplace(mediaId, std::move(waveform));
        self.state->overlayDirty = true;
        return;
    }
    [self enqueueWaveformForMediaIdentifier:identifier];
}

- (void)enqueueWaveformForMediaIdentifier:(NSString*)identifier {
    if (!identifier || !self.state) return;
    const Ulid mediaId(identifier.UTF8String ?: "");
    const DocumentSource* source = self.state->document.FindSource(mediaId);
    if (!source) return;
    const LibraryMedia* media = self.state->document.FindLibraryMedia(mediaId);
    for (const auto& pending : self.state->pendingWaveforms)
        if (pending.second.media_id == mediaId) return;
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    std::filesystem::path input(source->path);
    if (input.is_relative()) input = base / input;
    const std::filesystem::path output =
        base / ".cutmachine" / "waveforms" / (mediaId + ".waveform");
    const std::string label =
        "Waveform " + (media ? media->filename : input.filename().string());
    WaveformSettings settings;
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Waveform, label,
        [input = input.lexically_normal(), output, duration = source->duration,
         settings](MediaTaskContext& context, std::string& taskError) {
            return GenerateAudioWaveform(input.string(), output.string(),
                                         duration, settings, context,
                                         taskError);
        });
    self.state->pendingWaveforms.emplace(taskId,
                                         PendingWaveform{mediaId, output});
    [self refreshMediaTaskStatus];
}

- (void)processCompletedMediaWaveforms {
    if (!self.state || self.state->pendingWaveforms.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    for (auto item = self.state->pendingWaveforms.begin();
         item != self.state->pendingWaveforms.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        if (task->second.state == MediaTaskState::Succeeded) {
            AudioWaveform waveform;
            std::string error;
            if (LoadAudioWaveform(item->second.absolute_path.string(), waveform,
                                  error)) {
                self.state->waveforms[item->second.media_id] =
                    std::move(waveform);
                self.state->overlayDirty = true;
            } else {
                self.binSummaryLabel.stringValue =
                    [NSString stringWithFormat:@"Waveform illisible : %@",
                                               CMStringFromUtf8(error)];
            }
        } else if (task->second.state == MediaTaskState::Failed) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Échec waveform : %@",
                                 CMStringFromUtf8(task->second.error)];
        }
        item = self.state->pendingWaveforms.erase(item);
    }
}

- (void)processCompletedMediaTranscriptions {
    if (!self.state || (self.state->pendingTranscriptions.empty() &&
                        self.state->pendingTimelineTranscriptions.empty()))
        return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    for (auto item = self.state->pendingTranscriptions.begin();
         item != self.state->pendingTranscriptions.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        const PendingTranscription pending = item->second;
        item = self.state->pendingTranscriptions.erase(item);
        if (!pending.batch_id.empty()) {
            auto batch = self.state->pendingTimelineTranscriptions.find(
                pending.batch_id);
            if (batch != self.state->pendingTimelineTranscriptions.end()) {
                batch->second.task_ids.erase(task->first);
                if (task->second.state == MediaTaskState::Cancelled)
                    batch->second.error = "timeline transcription cancelled";
                else if (task->second.state != MediaTaskState::Succeeded)
                    batch->second.error = task->second.error;
            }
            continue;
        }
        if (task->second.state == MediaTaskState::Cancelled) {
            self.binSummaryLabel.stringValue = @"Transcription annulée.";
            continue;
        }
        if (task->second.state != MediaTaskState::Succeeded) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Échec de la transcription : %@",
                                 CMStringFromUtf8(task->second.error)];
            // AL4 -- the weights the task found unusable: why, and what to
            // set.
            if (pending.hesitations)
                [self showHesitationsRefusal:task->second.error];
            continue;
        }

        Transcript transcript;
        std::string message;
        // ITW3 -- captions built here cut on the same words as every tool.
        if (!LoadSourceWords(pending.absolute_path.parent_path().parent_path(),
                             pending.media_id, transcript, message)) {
            self.binSummaryLabel.stringValue =
                [NSString stringWithFormat:@"Transcript illisible : %@",
                                           CMStringFromUtf8(message)];
            continue;
        }
        if (pending.timeline_id != self.state->activeTimelineId) {
            self.binSummaryLabel.stringValue =
                @"Transcript prêt · revenez sur la timeline d’origine puis "
                 "relancez l’action pour créer S1.";
            continue;
        }

        std::vector<SubtitleCue> cues;
        for (const DocumentTrack& track :
             self.state->document.sequence.tracks) {
            if (track.kind != "video") continue;
            for (const DocumentClip& clip : track.clips) {
                if (clip.source_id != pending.media_id) continue;
                std::vector<SubtitleCue> clipCues;
                std::string ignored;
                if (SubtitleCuesForClip(transcript, clip, clipCues, ignored))
                    cues.insert(cues.end(), clipCues.begin(), clipCues.end());
            }
        }
        std::stable_sort(cues.begin(), cues.end(),
                         [](const SubtitleCue& left, const SubtitleCue& right) {
                             return left.timeline_in < right.timeline_in;
                         });
        if (cues.empty()) {
            self.binSummaryLabel.stringValue =
                @"Transcript prêt · ce rush n’est pas présent sur la "
                 "timeline active.";
            continue;
        }
        bool overlaps = false;
        for (size_t index = 1; index < cues.size(); ++index) {
            if (cues[index].timeline_in <
                cues[index - 1].timeline_in.add(cues[index - 1].duration)) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) {
            self.binSummaryLabel.stringValue =
                @"Transcript prêt · les occurrences du rush se chevauchent; "
                 "impossible de créer une seule piste S1.";
            continue;
        }

        int32_t trackIndex = 0;
        for (const DocumentTrack& track : self.state->document.sequence.tracks)
            trackIndex = std::max(trackIndex, track.index + 1);
        EditError editError = EditError::None;
        AddTrackOperation operation = BuildSubtitleTrackEdit(cues, trackIndex);
        if (![self
                applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                           error:editError
                                         message:message]) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Sous-titres non créés (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(editError)),
                                 CMStringFromUtf8(message)];
            continue;
        }
        [self
            refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
        self.state->overlayDirty = true;
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Transcript prêt · %lu sous-titre%@ ajouté%@ "
                              "sur S1",
                             (unsigned long)cues.size(),
                             cues.size() == 1 ? @"" : @"s",
                             cues.size() == 1 ? @"" : @"s"];
    }

    for (auto item = self.state->pendingTimelineTranscriptions.begin();
         item != self.state->pendingTimelineTranscriptions.end();) {
        if (!item->second.task_ids.empty()) {
            ++item;
            continue;
        }
        PendingTimelineTranscription batch = std::move(item->second);
        item = self.state->pendingTimelineTranscriptions.erase(item);
        if (!batch.error.empty()) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Transcription de la timeline échouée : %@",
                                 CMStringFromUtf8(batch.error)];
            if (batch.hesitations &&
                batch.error != "timeline transcription cancelled")
                [self showHesitationsRefusal:batch.error];
            continue;
        }
        if (batch.timeline_id != self.state->activeTimelineId) {
            self.binSummaryLabel.stringValue =
                @"Transcripts prêts · revenez sur la timeline d’origine puis "
                 "relancez l’action pour créer S1.";
            continue;
        }

        std::map<Ulid, Transcript> transcripts;
        std::string message;
        bool loaded = true;
        for (const auto& transcriptPath : batch.transcript_paths) {
            Transcript transcript;
            if (!LoadSourceWords(
                    transcriptPath.second.parent_path().parent_path(),
                    transcriptPath.first, transcript, message)) {
                loaded = false;
                break;
            }
            transcripts.emplace(transcriptPath.first, std::move(transcript));
        }
        if (!loaded) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Transcript de timeline illisible : %@",
                                 CMStringFromUtf8(message)];
            continue;
        }

        const bool hasSolo =
            std::any_of(self.state->document.sequence.tracks.begin(),
                        self.state->document.sequence.tracks.end(),
                        [](const DocumentTrack& track) {
                            return track.kind == "audio" && track.solo;
                        });
        std::vector<SubtitleCue> cues;
        for (const DocumentTrack& track :
             self.state->document.sequence.tracks) {
            if (track.kind != "audio" || track.muted ||
                (hasSolo && !track.solo))
                continue;
            for (const DocumentClip& clip : track.clips) {
                if (!ClipIsHeard(clip)) continue;
                const auto transcript = transcripts.find(clip.source_id);
                if (transcript == transcripts.end()) continue;
                std::vector<SubtitleCue> clipCues;
                std::string ignored;
                if (SubtitleCuesForClip(transcript->second, clip, clipCues,
                                        ignored))
                    cues.insert(cues.end(), clipCues.begin(), clipCues.end());
            }
        }
        std::stable_sort(cues.begin(), cues.end(),
                         [](const SubtitleCue& left, const SubtitleCue& right) {
                             return left.timeline_in < right.timeline_in;
                         });
        if (cues.empty()) {
            self.binSummaryLabel.stringValue =
                @"Transcription terminée, mais aucun mot ne correspond aux "
                 "coupes de la timeline.";
            continue;
        }
        bool overlaps = false;
        for (size_t index = 1; index < cues.size(); ++index) {
            if (cues[index].timeline_in <
                cues[index - 1].timeline_in.add(cues[index - 1].duration)) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) {
            self.binSummaryLabel.stringValue =
                @"Transcripts prêts · plusieurs pistes audio parlées se "
                 "chevauchent; impossible de créer une seule piste S1.";
            continue;
        }

        int32_t trackIndex = 0;
        for (const DocumentTrack& track : self.state->document.sequence.tracks)
            trackIndex = std::max(trackIndex, track.index + 1);
        EditError editError = EditError::None;
        AddTrackOperation operation = BuildSubtitleTrackEdit(cues, trackIndex);
        if (![self
                applyAndPersistTimelineOperation:Operation{std::move(operation)}
                                           error:editError
                                         message:message]) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Sous-titres non créés (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(editError)),
                                 CMStringFromUtf8(message)];
            continue;
        }
        [self
            refreshTimelineAfterEditFromPosition:self.state->requestedPosition];
        self.state->overlayDirty = true;
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"Timeline transcrite · %lu sous-titre%@ sur S1",
                             (unsigned long)cues.size(),
                             cues.size() == 1 ? @"" : @"s"];
    }
}

- (void)loadOrEnqueueThumbnailForMediaIdentifier:(NSString*)identifier {
    if ([self stageThumbnailForMediaIdentifier:identifier])
        [self.mediaCollection reloadData];
}

// PERF-2026-09. Returns whether the collection view now shows something it
// did not before, so a caller loading a whole library can reload it once
// instead of once per rush. -reloadData is O(items), so the startup loop was
// quadratic in the number of rushes on top of reading each thumbnail off the
// drive on the main thread.
- (BOOL)stageThumbnailForMediaIdentifier:(NSString*)identifier {
    if (!identifier || !self.state || self.mediaThumbnails[identifier])
        return NO;
    const Ulid mediaId(identifier.UTF8String ?: "");
    const LibraryMedia* media = self.state->document.FindLibraryMedia(mediaId);
    if (media && !media->has_video) return NO;
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path output = projectPath.parent_path() /
                                         ".cutmachine" / "thumbnails" /
                                         (mediaId + ".png");
    NSImage* image = [[NSImage alloc]
        initWithContentsOfFile:[NSString stringWithUTF8String:output.c_str()]];
    if (image) {
        self.mediaThumbnails[identifier] = image;
        return YES;
    }
    [self enqueueThumbnailForMediaIdentifier:identifier];
    return NO;
}

- (void)enqueueThumbnailForMediaIdentifier:(NSString*)identifier {
    if (!identifier || !self.state) return;
    const Ulid mediaId(identifier.UTF8String ?: "");
    const DocumentSource* source = self.state->document.FindSource(mediaId);
    if (!source) return;
    const LibraryMedia* media = self.state->document.FindLibraryMedia(mediaId);
    if (media && !media->has_video) {
        self.binSummaryLabel.stringValue =
            @"Ce média est audio seul · aucune vignette à générer.";
        return;
    }
    for (const auto& pending : self.state->pendingThumbnails)
        if (pending.second.media_id == mediaId) return;
    const std::filesystem::path projectPath = std::filesystem::absolute(
        std::filesystem::path(self.documentPath.UTF8String ?: ""));
    const std::filesystem::path base = projectPath.parent_path();
    std::filesystem::path input(source->path);
    if (input.is_relative()) input = base / input;
    const std::filesystem::path output =
        base / ".cutmachine" / "thumbnails" / (mediaId + ".png");
    const std::string label =
        "Thumbnail " + (media ? media->filename : input.filename().string());
    ThumbnailSettings settings;
    const Ulid taskId = self.state->mediaTasks->Enqueue(
        MediaTaskKind::Thumbnail, label,
        [input = input.lexically_normal(), output, duration = source->duration,
         settings](MediaTaskContext& context, std::string& taskError) {
            return GenerateMediaThumbnail(input.string(), output.string(),
                                          duration, settings, context,
                                          taskError);
        });
    self.state->pendingThumbnails.emplace(taskId,
                                          PendingThumbnail{mediaId, output});
    [self refreshMediaTaskStatus];
}

- (void)processCompletedMediaThumbnails {
    if (!self.state || self.state->pendingThumbnails.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    bool reload = false;
    for (auto item = self.state->pendingThumbnails.begin();
         item != self.state->pendingThumbnails.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        if (task->second.state == MediaTaskState::Succeeded) {
            NSString* identifier =
                [NSString stringWithUTF8String:item->second.media_id.c_str()];
            NSImage* image = [[NSImage alloc]
                initWithContentsOfFile:
                    [NSString stringWithUTF8String:item->second.absolute_path
                                                       .c_str()]];
            if (image) {
                self.mediaThumbnails[identifier] = image;
                reload = true;
            } else {
                self.binSummaryLabel.stringValue =
                    @"La vignette générée est illisible.";
            }
        } else if (task->second.state == MediaTaskState::Failed) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Échec vignette : %@",
                                 CMStringFromUtf8(task->second.error)];
        }
        item = self.state->pendingThumbnails.erase(item);
    }
    if (reload) [self.mediaCollection reloadData];
}

- (void)processCompletedMediaRelinks {
    if (!self.state || self.state->pendingRelinks.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    for (auto item = self.state->pendingRelinks.begin();
         item != self.state->pendingRelinks.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        PendingRelink pending = item->second;
        item = self.state->pendingRelinks.erase(item);
        if (task->second.state != MediaTaskState::Succeeded) {
            if (task->second.state == MediaTaskState::Failed)
                self.binSummaryLabel.stringValue = [NSString
                    stringWithFormat:@"Reconnexion refusée : %@",
                                     CMStringFromUtf8(task->second.error)];
            continue;
        }

        const LibraryMedia* previousMedia =
            self.state->document.FindLibraryMedia(pending.media_id);
        if (!previousMedia) continue;
        const LibraryMedia previous = *previousMedia;
        Project stagedProject = self.state->project;
        ProjectEditLog stagedProjectLog = self.state->projectEditLog;
        std::string message;
        EditError editError = EditError::None;
        ProjectOperation relink = RelinkProjectMediaOperation{
            {{pending.media_id, pending.result->media, pending.stored_path}},
            std::nullopt};
        if (!stagedProjectLog.Apply(stagedProject, std::move(relink), editError,
                                    message) ||
            ![self commitProjectCandidate:stagedProject
                                  editLog:self.state->editLog
                               projectLog:stagedProjectLog
                                  message:message]) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Reconnexion impossible (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(editError)),
                                 CMStringFromUtf8(message)];
            continue;
        }
        self.state->project = std::move(stagedProject);
        self.state->projectEditLog = std::move(stagedProjectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        self.state->document =
            self.state->project.MakeDocument(self.state->activeTimelineId);

        for (auto pendingTask = self.state->pendingProxies.begin();
             pendingTask != self.state->pendingProxies.end();) {
            if (pendingTask->second.media_id == pending.media_id) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingProxies.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }
        for (auto pendingTask = self.state->pendingWaveforms.begin();
             pendingTask != self.state->pendingWaveforms.end();) {
            if (pendingTask->second.media_id == pending.media_id) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingWaveforms.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }
        for (auto pendingTask = self.state->pendingThumbnails.begin();
             pendingTask != self.state->pendingThumbnails.end();) {
            if (pendingTask->second.media_id == pending.media_id) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingThumbnails.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }
        for (auto pendingTask = self.state->pendingTranscriptions.begin();
             pendingTask != self.state->pendingTranscriptions.end();) {
            if (pendingTask->second.media_id == pending.media_id) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                const Ulid batchId = pendingTask->second.batch_id;
                if (!batchId.empty()) {
                    auto batch =
                        self.state->pendingTimelineTranscriptions.find(batchId);
                    if (batch !=
                        self.state->pendingTimelineTranscriptions.end()) {
                        batch->second.task_ids.erase(pendingTask->first);
                        batch->second.error =
                            "source relinked during timeline transcription";
                    }
                }
                pendingTask =
                    self.state->pendingTranscriptions.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }

        const std::filesystem::path projectPath = std::filesystem::absolute(
            std::filesystem::path(self.documentPath.UTF8String ?: ""));
        const std::filesystem::path base = projectPath.parent_path();
        const std::filesystem::path expectedProxy =
            base / ".cutmachine" / "proxies" / (pending.media_id + ".mov");
        std::filesystem::path oldProxy(previous.proxy_path);
        if (oldProxy.is_relative()) oldProxy = base / oldProxy;
        std::error_code ignored;
        if (!previous.proxy_path.empty() &&
            oldProxy.lexically_normal() == expectedProxy.lexically_normal())
            std::filesystem::remove(expectedProxy, ignored);
        std::filesystem::remove(base / ".cutmachine" / "waveforms" /
                                    (pending.media_id + ".waveform"),
                                ignored);
        std::filesystem::remove(
            base / ".cutmachine" / "thumbnails" / (pending.media_id + ".png"),
            ignored);
        std::filesystem::remove(
            base / ".cutmachine" / "transcripts" / (pending.media_id + ".json"),
            ignored);
        self.state->waveforms.erase(pending.media_id);
        NSString* identifier =
            [NSString stringWithUTF8String:pending.media_id.c_str()];
        [self.mediaThumbnails removeObjectForKey:identifier];
        if (const LibraryMedia* media =
                self.state->document.FindLibraryMedia(pending.media_id))
            self.state->mediaMetadata[pending.media_id] = *media;

        [self reloadDecodeWorkers];
        auto audio = std::make_unique<AudioPlayback>();
        std::string audioError;
        const bool audioReady =
            audio->Open(self.state->document, base.string(), audioError);
        self.state->audioPlayback = std::move(audio);
        [self rebuildMediaList];
        if (self.state->sourceMonitorId == pending.media_id) {
            const LibraryMedia* media =
                self.state->document.FindLibraryMedia(pending.media_id);
            if (media)
                self.sourceMonitorTitleLabel.stringValue = [NSString
                    stringWithFormat:@"SOURCE — %@",
                                     CMStringFromUtf8(media->filename)];
            self.sourceOfflineMediaLabel.hidden =
                self.state->offlineSourceIds.count(pending.media_id) == 0;
        }
        [self loadOrEnqueueThumbnailForMediaIdentifier:identifier];
        const LibraryMedia* replacement =
            self.state->document.FindLibraryMedia(pending.media_id);
        if (replacement && replacement->has_audio)
            [self loadOrEnqueueWaveformForMediaIdentifier:identifier];
        if (replacement) {
            std::string codec = replacement->codec;
            std::transform(
                codec.begin(), codec.end(), codec.begin(),
                [](unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            if (self.state->automaticProxiesEnabled &&
                (replacement->width > 1920 || codec == "hevc" ||
                 codec == "h265" || codec == "av1"))
                [self enqueueProxyForMediaIdentifier:identifier];
        }
        self.binSummaryLabel.stringValue =
            audioReady
                ? @"Média reconnecté · caches en cours de régénération"
                : [NSString stringWithFormat:
                                @"Média reconnecté · audio indisponible : %@",
                                CMStringFromUtf8(audioError)];
    }
}

- (void)processCompletedBatchRelinks {
    if (!self.state || self.state->pendingBatchRelinks.empty()) return;
    const std::vector<MediaTaskSnapshot> snapshots =
        self.state->mediaTasks->Snapshot();
    std::map<Ulid, MediaTaskSnapshot> byId;
    for (const MediaTaskSnapshot& task : snapshots) byId[task.id] = task;
    for (auto item = self.state->pendingBatchRelinks.begin();
         item != self.state->pendingBatchRelinks.end();) {
        const auto task = byId.find(item->first);
        if (task == byId.end() ||
            task->second.state == MediaTaskState::Queued ||
            task->second.state == MediaTaskState::Running) {
            ++item;
            continue;
        }
        std::shared_ptr<BatchRelinkResult> result = item->second.result;
        item = self.state->pendingBatchRelinks.erase(item);
        if (task->second.state != MediaTaskState::Succeeded) {
            if (task->second.state == MediaTaskState::Failed)
                self.binSummaryLabel.stringValue = [NSString
                    stringWithFormat:@"Relink en lot échoué : %@",
                                     CMStringFromUtf8(task->second.error)];
            continue;
        }
        if (result->replacements.empty()) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Aucun média reconnecté · %lu introuvable%@ "
                                 @"· %lu ambigu%@ · %lu incompatible%@",
                                 (unsigned long)result->unmatched,
                                 result->unmatched == 1 ? @"" : @"s",
                                 (unsigned long)result->ambiguous,
                                 result->ambiguous == 1 ? @"" : @"s",
                                 (unsigned long)result->incompatible,
                                 result->incompatible == 1 ? @"" : @"s"];
            continue;
        }

        std::map<Ulid, LibraryMedia> previousMedia;
        for (const RelinkReplacement& replacement : result->replacements) {
            const LibraryMedia* previous =
                self.state->document.FindLibraryMedia(replacement.media_id);
            if (previous) previousMedia[replacement.media_id] = *previous;
        }
        Project stagedProject = self.state->project;
        ProjectEditLog stagedProjectLog = self.state->projectEditLog;
        std::string message;
        std::vector<ProjectRelinkItem> projectReplacements;
        projectReplacements.reserve(result->replacements.size());
        for (const RelinkReplacement& replacement : result->replacements)
            projectReplacements.push_back({replacement.media_id,
                                           replacement.media,
                                           replacement.stored_path});
        EditError editError = EditError::None;
        ProjectOperation relink = RelinkProjectMediaOperation{
            std::move(projectReplacements), std::nullopt};
        if (!stagedProjectLog.Apply(stagedProject, std::move(relink), editError,
                                    message) ||
            ![self commitProjectCandidate:stagedProject
                                  editLog:self.state->editLog
                               projectLog:stagedProjectLog
                                  message:message]) {
            self.binSummaryLabel.stringValue = [NSString
                stringWithFormat:@"Relink en lot annulé (%@) : %@",
                                 CMStringFromUtf8(EditErrorName(editError)),
                                 CMStringFromUtf8(message)];
            continue;
        }
        self.state->project = std::move(stagedProject);
        self.state->projectEditLog = std::move(stagedProjectLog);
        self.state->lastHistoryDomain = HistoryDomain::Project;
        self.state->document =
            self.state->project.MakeDocument(self.state->activeTimelineId);
        std::set<Ulid> relinkedIds;
        for (const RelinkReplacement& replacement : result->replacements)
            relinkedIds.insert(replacement.media_id);

        for (auto pendingTask = self.state->pendingProxies.begin();
             pendingTask != self.state->pendingProxies.end();) {
            if (relinkedIds.count(pendingTask->second.media_id)) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingProxies.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }
        for (auto pendingTask = self.state->pendingWaveforms.begin();
             pendingTask != self.state->pendingWaveforms.end();) {
            if (relinkedIds.count(pendingTask->second.media_id)) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingWaveforms.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }
        for (auto pendingTask = self.state->pendingThumbnails.begin();
             pendingTask != self.state->pendingThumbnails.end();) {
            if (relinkedIds.count(pendingTask->second.media_id)) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                pendingTask = self.state->pendingThumbnails.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }
        for (auto pendingTask = self.state->pendingTranscriptions.begin();
             pendingTask != self.state->pendingTranscriptions.end();) {
            if (relinkedIds.count(pendingTask->second.media_id)) {
                self.state->mediaTasks->Cancel(pendingTask->first);
                const Ulid batchId = pendingTask->second.batch_id;
                if (!batchId.empty()) {
                    auto batch =
                        self.state->pendingTimelineTranscriptions.find(batchId);
                    if (batch !=
                        self.state->pendingTimelineTranscriptions.end()) {
                        batch->second.task_ids.erase(pendingTask->first);
                        batch->second.error =
                            "source relinked during timeline transcription";
                    }
                }
                pendingTask =
                    self.state->pendingTranscriptions.erase(pendingTask);
            } else {
                ++pendingTask;
            }
        }

        const std::filesystem::path projectPath = std::filesystem::absolute(
            std::filesystem::path(self.documentPath.UTF8String ?: ""));
        const std::filesystem::path base = projectPath.parent_path();
        for (const Ulid& mediaId : relinkedIds) {
            const std::filesystem::path expectedProxy =
                base / ".cutmachine" / "proxies" / (mediaId + ".mov");
            const auto previous = previousMedia.find(mediaId);
            if (previous != previousMedia.end() &&
                !previous->second.proxy_path.empty()) {
                std::filesystem::path oldProxy(previous->second.proxy_path);
                if (oldProxy.is_relative()) oldProxy = base / oldProxy;
                std::error_code ignored;
                if (oldProxy.lexically_normal() ==
                    expectedProxy.lexically_normal())
                    std::filesystem::remove(expectedProxy, ignored);
            }
            std::error_code ignored;
            std::filesystem::remove(
                base / ".cutmachine" / "waveforms" / (mediaId + ".waveform"),
                ignored);
            std::filesystem::remove(
                base / ".cutmachine" / "thumbnails" / (mediaId + ".png"),
                ignored);
            std::filesystem::remove(
                base / ".cutmachine" / "transcripts" / (mediaId + ".json"),
                ignored);
            self.state->waveforms.erase(mediaId);
            NSString* identifier =
                [NSString stringWithUTF8String:mediaId.c_str()];
            [self.mediaThumbnails removeObjectForKey:identifier];
            if (const LibraryMedia* media =
                    self.state->document.FindLibraryMedia(mediaId))
                self.state->mediaMetadata[mediaId] = *media;
        }

        [self reloadDecodeWorkers];
        auto audio = std::make_unique<AudioPlayback>();
        std::string audioError;
        const bool audioReady =
            audio->Open(self.state->document, base.string(), audioError);
        self.state->audioPlayback = std::move(audio);
        [self rebuildMediaList];
        for (const Ulid& mediaId : relinkedIds) {
            NSString* identifier =
                [NSString stringWithUTF8String:mediaId.c_str()];
            [self loadOrEnqueueThumbnailForMediaIdentifier:identifier];
            const LibraryMedia* media =
                self.state->document.FindLibraryMedia(mediaId);
            if (!media) continue;
            if (media->has_audio)
                [self loadOrEnqueueWaveformForMediaIdentifier:identifier];
            std::string codec = media->codec;
            std::transform(
                codec.begin(), codec.end(), codec.begin(),
                [](unsigned char character) {
                    return static_cast<char>(std::tolower(character));
                });
            if (self.state->automaticProxiesEnabled &&
                (media->width > 1920 || codec == "hevc" || codec == "h265" ||
                 codec == "av1"))
                [self enqueueProxyForMediaIdentifier:identifier];
            if (self.state->sourceMonitorId == mediaId) {
                self.sourceMonitorTitleLabel.stringValue = [NSString
                    stringWithFormat:@"SOURCE — %@",
                                     CMStringFromUtf8(media->filename)];
                self.sourceOfflineMediaLabel.hidden =
                    self.state->offlineSourceIds.count(mediaId) == 0;
            }
        }
        self.binSummaryLabel.stringValue = [NSString
            stringWithFormat:@"%lu média%@ reconnecté%@ · %lu introuvable%@ · "
                             @"%lu ambigu%@ · %lu incompatible%@%@",
                             (unsigned long)relinkedIds.size(),
                             relinkedIds.size() == 1 ? @"" : @"s",
                             relinkedIds.size() == 1 ? @"" : @"s",
                             (unsigned long)result->unmatched,
                             result->unmatched == 1 ? @"" : @"s",
                             (unsigned long)result->ambiguous,
                             result->ambiguous == 1 ? @"" : @"s",
                             (unsigned long)result->incompatible,
                             result->incompatible == 1 ? @"" : @"s",
                             audioReady ? @"" : @" · audio indisponible"];
    }
}

- (void)commitMediaImportBatch:(ProbedImportBatch*)batch {
    if (!batch) return;
    const std::string targetBin =
        batch->target_bin.empty() ||
                self.state->document.FindBin(batch->target_bin)
            ? batch->target_bin
            : std::string{};
    IngestOutcome outcome;
    std::string errorName;
    std::string message;
    [self commitProbedMedia:batch->files
               documentPath:batch->document_path
                  targetBin:targetBin
                    outcome:outcome
                  errorName:errorName
                    message:message];
}

// PRD7 -- docs/roadmap/PRODUIT-2026-10.md. The engine adds what the task
// read (AddProbedMedia, Ingest.h), as it does for --ingest: known files, new
// ones, the target bin, the audio level. What stays here is the window's:
// putting the document back if it does not validate or cannot be written,
// and what a new rush needs on screen.
//
// F2.4 -- ROADMAP.md. The File menu's import and an agent's ingest_media
// (McpLiveBackend::SetIngestCommitCallback) both end here, so that a rush an
// agent imports reaches the browser as one imported by hand. An unset
// `targetBin` leaves a known rush in its bin, as --ingest does; a set one,
// the root (empty) included, files it there.
//
// DEC32 (PRD7) -- docs/roadmap/PRODUIT-2026-10.md. The new rushes and the
// moves are the plan's IngestMediaOperation, applied through the timeline's
// journal on copies and published once written (S3): Cmd+Z takes an import
// back. A target bin the document no longer has is refused by name.
- (BOOL)commitProbedMedia:(const std::vector<ProbedIngestFile>&)files
             documentPath:(const std::filesystem::path&)documentPath
                targetBin:(const std::optional<Ulid>&)targetBin
                  outcome:(IngestOutcome&)outcome
                errorName:(std::string&)errorName
                  message:(std::string&)message {
    errorName.clear();
    message.clear();
    const IngestPlan plan = PlanProbedMedia(
        self.state->document, documentPath.parent_path(), files, targetBin);
    outcome = plan.outcome;
    Document stagedDocument = self.state->document;
    EditLog stagedLog = self.state->editLog;
    if (plan.HasJournalEntry()) {
        EditError editError = EditError::None;
        if (!stagedLog.Apply(stagedDocument, plan.operation, editError,
                             message))
            errorName = EditErrorName(editError);
    }
    const size_t added = outcome.added_ids.size();
    const size_t completed = outcome.completed_ids.size();
    const size_t moved = outcome.moved;
    const size_t rejected = outcome.errors.size();
    // The rushes whose metadata the document has just learnt: the new ones,
    // and those it knew without having read them.
    std::vector<Ulid> readMedia = outcome.added_ids;
    readMedia.insert(readMedia.end(), outcome.completed_ids.begin(),
                     outcome.completed_ids.end());

    if (errorName.empty() && outcome.changed) {
        if (!stagedDocument.Validate(message)) {
            errorName = "ValidationFailed";
        } else if (![self persistStagedDocument:stagedDocument
                                        editLog:stagedLog
                                        message:message]) {
            errorName = "IoError";
        }
    }
    if (!errorName.empty()) {
        self.binSummaryLabel.stringValue =
            [NSString stringWithFormat:@"Import impossible : %@",
                                       CMStringFromUtf8(message)];
        return NO;
    }
    if (outcome.changed) {
        self.state->document = std::move(stagedDocument);
        self.state->editLog = std::move(stagedLog);
        self.state->lastHistoryDomain = HistoryDomain::Timeline;
    }
    // PERF10 -- no decoder is opened for an imported rush: the monitor opens
    // one when it first shows it. The file was probed a moment ago, so it is
    // not offline.
    for (const Ulid& mediaId : readMedia) {
        const LibraryMedia* media =
            self.state->document.FindLibraryMedia(mediaId);
        if (media) self.state->mediaMetadata[mediaId] = *media;
    }
    if (!readMedia.empty()) {
        auto audio = std::make_unique<AudioPlayback>();
        std::string audioError;
        if (audio->Open(self.state->document,
                        documentPath.parent_path().string(), audioError))
            self.state->audioPlayback = std::move(audio);
    }
    for (const Ulid& mediaId : readMedia) {
        const LibraryMedia* media =
            self.state->document.FindLibraryMedia(mediaId);
        if (!media) continue;
        std::string codec = media->codec;
        std::transform(codec.begin(), codec.end(), codec.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        if (self.state->automaticProxiesEnabled &&
            (media->width > 1920 || codec == "hevc" || codec == "h265" ||
             codec == "av1"))
            [self enqueueProxyForMediaIdentifier:
                      [NSString stringWithUTF8String:mediaId.c_str()]];
        if (media->has_audio)
            [self loadOrEnqueueWaveformForMediaIdentifier:
                      [NSString stringWithUTF8String:mediaId.c_str()]];
        [self loadOrEnqueueThumbnailForMediaIdentifier:
                  [NSString stringWithUTF8String:mediaId.c_str()]];
    }
    // The bin the rushes went to is shown; without one, new rushes are at
    // the root, and an ingest that added none leaves the selection alone.
    NSString* selected = nil;
    if (targetBin)
        selected = targetBin->empty()
                       ? @"__root__"
                       : [NSString stringWithUTF8String:targetBin->c_str()];
    else if (added)
        selected = @"__root__";
    [self refreshBinControlsSelecting:selected];
    NSMutableArray<NSString*>* results = [NSMutableArray array];
    if (added)
        [results addObject:[NSString stringWithFormat:@"%lu rush%@ importé%@",
                                                      (unsigned long)added,
                                                      added == 1 ? @"" : @"es",
                                                      added == 1 ? @"" : @"s"]];
    if (completed)
        [results
            addObject:[NSString stringWithFormat:@"%lu rush%@ complété%@",
                                                 (unsigned long)completed,
                                                 completed == 1 ? @"" : @"es",
                                                 completed == 1 ? @"" : @"s"]];
    if (moved)
        [results addObject:[NSString stringWithFormat:@"%lu rush%@ déplacé%@",
                                                      (unsigned long)moved,
                                                      moved == 1 ? @"" : @"es",
                                                      moved == 1 ? @"" : @"s"]];
    if (rejected)
        [results
            addObject:[NSString stringWithFormat:@"%lu refusé%@",
                                                 (unsigned long)rejected,
                                                 rejected == 1 ? @"" : @"s"]];
    if (results.count == 0) [results addObject:@"Aucun changement"];
    self.binSummaryLabel.stringValue =
        [results componentsJoinedByString:@" · "];
    return YES;
}

@end

#pragma once

// S8 -- SAVING_ROADMAP.md. A project shaped like the interview packages the
// save path was measured on (PRJI: 405 rushes, 11 timelines), built in
// memory so that the history tests and the save benchmark need neither media
// nor the external volume the real packages live on.

#include "Project.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

inline Project MakeInterviewShapedProject(size_t rushCount = 405,
                                          size_t timelineCount = 11,
                                          size_t clipsPerTimeline = 24) {
    Project project("Interview-shaped fixture");
    for (int index = 0; index < 4; ++index) {
        DocumentBin bin;
        bin.name = "Bin " + std::to_string(index + 1);
        project.bins.push_back(std::move(bin));
    }
    for (size_t index = 0; index < rushCount; ++index) {
        LibraryMedia media;
        media.filename = "C" + std::to_string(8000 + index) + ".MP4";
        media.path = "1_RUSHES/" + media.filename;
        media.codec = "hevc";
        media.width = 3840;
        media.height = 2160;
        media.pixel_format = "yuv422p10le";
        media.color_range = "tv";
        media.color_space = "bt709";
        media.color_transfer = "bt709";
        media.color_primaries = "bt709";
        media.rate = {25, 1};
        media.duration = {750 + static_cast<int64_t>(index % 50) * 25, 25};
        media.orientation = "landscape";
        media.has_audio = true;
        media.audio_rate = 48000;
        media.audio_channels = 2;
        media.audio_level_measured = true;
        media.audio_level = 20000 + static_cast<int64_t>(index);
        media.bin_id = project.bins[index % project.bins.size()].id;
        project.sources.push_back(
            {media.id, media.path, media.rate, media.duration});
        project.rushes.push_back(std::move(media));
    }
    project.timelines.clear();
    for (size_t timelineIndex = 0; timelineIndex < timelineCount;
         ++timelineIndex) {
        DocumentSequence timeline;
        timeline.name = "Timeline " + std::to_string(timelineIndex + 1);
        timeline.width = 1080;
        timeline.height = 1920;
        DocumentTrack video{GenerateUlid(), "video", 0, {}};
        DocumentTrack audio{GenerateUlid(), "audio", 1, {}};
        RationalTime cursor{0, 25};
        for (size_t clipIndex = 0; clipIndex < clipsPerTimeline; ++clipIndex) {
            const DocumentSource& source =
                project.sources[(timelineIndex * clipsPerTimeline + clipIndex) %
                                project.sources.size()];
            DocumentClip picture;
            picture.source_id = source.id;
            picture.source_in = {25, 25};
            picture.duration = {50, 25};
            picture.timeline_in = cursor;
            picture.include_audio = false;
            picture.link_group_id = GenerateUlid();
            DocumentClip sound = picture;
            sound.id = GenerateUlid();
            video.clips.push_back(std::move(picture));
            audio.clips.push_back(std::move(sound));
            cursor = cursor.add({50, 25});
        }
        timeline.tracks = {std::move(video), std::move(audio)};
        project.timelines.push_back(std::move(timeline));
    }
    project.active_timeline_id = project.timelines.front().id;
    return project;
}

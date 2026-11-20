#include "MotionOutline.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>

std::string MotionDisplayId(const std::string& id) {
    return id.substr(0, id.find('#'));
}

std::string MotionShortName(std::string text, size_t most) {
    for (char& character : text) {
        if (character == '\n' || character == '\r') {
            character = ' ';
        }
    }
    if (text.size() > most) {
        size_t cut = most;
        while (cut > 0 &&
               (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
            --cut;  // never inside a UTF-8 sequence
        }
        text = text.substr(0, cut) + "…";
    }
    return text;
}

std::string MotionGroupName(const std::string& id,
                            const MotionEvaluatedNode* evaluated) {
    if (!id.empty()) {
        return MotionDisplayId(id);
    }
    std::string text;
    double biggest = 0.0;
    const std::function<void(const MotionEvaluatedNode&)> gather =
        [&](const MotionEvaluatedNode& at) {
            if (at.type == MotionNodeType::Text &&
                at.text_style.font_size > biggest) {
                biggest = at.text_style.font_size;
                text = at.text;
            }
            for (const MotionEvaluatedNode& child : at.children) {
                gather(child);
            }
        };
    if (evaluated != nullptr) {
        gather(*evaluated);
    }
    return text.empty() ? "groupe" : text;
}

std::string MotionSceneNumber(int64_t index) {
    char digits[16];
    std::snprintf(digits, sizeof(digits), "%02lld ",
                  static_cast<long long>(index));
    return digits;
}

namespace {

std::string Kind(MotionNodeType type) {
    switch (type) {
        case MotionNodeType::Fill:
            return "Plein cadre";
        case MotionNodeType::Box:
            return "Boîte";
        case MotionNodeType::Text:
            return "Texte";
        case MotionNodeType::Image:
            return "Image";
        case MotionNodeType::Video:
            return "Vidéo";
        case MotionNodeType::Audio:
            return "Son";
        case MotionNodeType::Composition:
            return "Composition";
        case MotionNodeType::Sequence:
            return "Scène";
        case MotionNodeType::Path:
            return "Tracé";
        case MotionNodeType::Repeat:
            return "Répétition";
        case MotionNodeType::Series:
            return "Série";
        case MotionNodeType::Transition:
            return "Transition";
    }
    return "Nœud";
}

class Outliner {
public:
    Outliner(const MotionComposition& composition, const mcp_json::Value& props,
             MotionMeasurer& measurer, const MotionEvaluationContext& context,
             std::vector<MotionOutlineRow>& rows, MotionError& error)
        : composition_(composition),
          props_(props),
          measurer_(measurer),
          context_(context),
          rows_(rows),
          error_(error) {}

    bool Run() {
        rows_.clear();
        for (size_t index = 0; index < composition_.root.children.size();
             ++index) {
            if (!Visit(composition_.root.children[index],
                       "root.children[" + std::to_string(index) + "]", 0, 0,
                       composition_.duration, false)) {
                return false;
            }
        }
        return true;
    }

private:
    const MotionEvaluatedNode* At(int64_t frame, const std::string& path) {
        frame = std::clamp<int64_t>(
            frame, 0, std::max<int64_t>(0, composition_.duration - 1));
        auto& slot = frames_[frame];
        if (!slot) {
            slot = std::make_unique<Indexed>();
            if (!EvaluateMotionFrame(composition_, props_, frame, measurer_,
                                     context_, slot->frame, error_)) {
                failed_ = true;
                return nullptr;
            }
            Index(slot->frame.root, *slot);
        }
        const auto found = slot->by_path.find(path);
        return found == slot->by_path.end() ? nullptr : found->second;
    }

    struct Indexed {
        MotionFrame frame;
        std::map<std::string, const MotionEvaluatedNode*> by_path;
    };

    static void Index(const MotionEvaluatedNode& node, Indexed& indexed) {
        indexed.by_path.emplace(node.path, &node);
        for (const MotionEvaluatedNode& child : node.children) {
            Index(child, indexed);
        }
    }

    static std::string FileName(const std::optional<MotionValue>& source) {
        if (!source.has_value() || source->kind != MotionBindingKind::Literal ||
            !source->literal.IsString()) {
            return {};
        }
        return std::filesystem::path(source->literal.AsString())
            .filename()
            .string();
    }

    std::string Name(const MotionNode& node, const std::string& path,
                     int64_t start, bool inScene) {
        const MotionEvaluatedNode* shown = nullptr;
        const auto evaluated = [&]() {
            if (shown == nullptr) {
                shown = At(start, path);
            }
            return shown;
        };
        switch (node.type) {
            case MotionNodeType::Sequence: {
                const std::string number =
                    inScene ? std::string() : MotionSceneNumber(++scenes_);
                return number + MotionGroupName(node.id, evaluated());
            }
            case MotionNodeType::Text: {
                if (node.content.has_value() &&
                    node.content->kind == MotionBindingKind::Timecode) {
                    return "timecode";
                }
                if (node.content.has_value() &&
                    node.content->kind == MotionBindingKind::Literal &&
                    node.content->literal.IsString()) {
                    return MotionShortName(node.content->literal.AsString());
                }
                const MotionEvaluatedNode* at = evaluated();
                return at != nullptr ? MotionShortName(at->text)
                                     : std::string("texte");
            }
            case MotionNodeType::Image:
            case MotionNodeType::Video:
            case MotionNodeType::Audio: {
                if (!node.id.empty()) {
                    return MotionDisplayId(node.id);
                }
                const std::string file = FileName(node.src);
                return file.empty() ? Kind(node.type) : file;
            }
            case MotionNodeType::Fill:
            case MotionNodeType::Box:
                if (node.children.empty()) {
                    return node.id.empty() ? std::string("fond")
                                           : MotionDisplayId(node.id);
                }
                return MotionGroupName(node.id, evaluated());
            default:
                return node.id.empty() ? Kind(node.type)
                                       : MotionDisplayId(node.id);
        }
    }

    bool Visit(const MotionNode& node, const std::string& path, int depth,
               int64_t start, int64_t length, bool inScene) {
        MotionOutlineRow row;
        row.uid = node.uid;
        row.path = path;
        row.depth = depth;
        row.kind = Kind(node.type);
        int64_t inside = start;
        int64_t innerLength = length;
        if (node.type == MotionNodeType::Sequence) {
            inside = start + node.from;
            innerLength = node.duration.value_or(length - node.from);
            row.scene = !inScene;
            row.start = inside;
            row.duration = innerLength;
        }
        row.name = Name(node, path, inside, inScene);
        if (failed_) {
            return false;
        }
        rows_.push_back(row);
        const bool childrenInScene =
            inScene || node.type == MotionNodeType::Sequence;
        for (size_t index = 0; index < node.children.size(); ++index) {
            if (!Visit(node.children[index],
                       path + ".children[" + std::to_string(index) + "]",
                       depth + 1, inside, innerLength, childrenInScene)) {
                return false;
            }
        }
        return true;
    }

    const MotionComposition& composition_;
    const mcp_json::Value& props_;
    MotionMeasurer& measurer_;
    const MotionEvaluationContext& context_;
    std::vector<MotionOutlineRow>& rows_;
    MotionError& error_;
    std::map<int64_t, std::unique_ptr<Indexed>> frames_;
    int64_t scenes_ = 0;
    bool failed_ = false;
};

}  // namespace

bool MotionOutlineRows(const MotionComposition& composition,
                       const mcp_json::Value& props, MotionMeasurer& measurer,
                       const MotionEvaluationContext& context,
                       std::vector<MotionOutlineRow>& rows,
                       MotionError& error) {
    Outliner outliner(composition, props, measurer, context, rows, error);
    return outliner.Run();
}

std::vector<size_t> MotionOutlineShown(
    const std::vector<MotionOutlineRow>& rows, const std::string& selected) {
    // The parent of each row: the nearest row before it one level up.
    std::vector<size_t> parents(rows.size(), rows.size());
    std::vector<size_t> open;
    for (size_t index = 0; index < rows.size(); ++index) {
        const auto depth = static_cast<size_t>(std::max(0, rows[index].depth));
        open.resize(std::min(open.size(), depth));
        if (depth > 0 && open.size() == depth) {
            parents[index] = open.back();
        }
        open.push_back(index);
    }
    std::vector<bool> expanded(rows.size(), false);
    for (size_t index = 0; index < rows.size(); ++index) {
        if (!selected.empty() && rows[index].uid == selected) {
            for (size_t at = index; at < rows.size(); at = parents[at]) {
                expanded[at] = true;
            }
        }
    }
    std::vector<size_t> shown;
    for (size_t index = 0; index < rows.size(); ++index) {
        if (rows[index].depth == 0 ||
            (parents[index] < rows.size() && expanded[parents[index]])) {
            shown.push_back(index);
        }
    }
    return shown;
}

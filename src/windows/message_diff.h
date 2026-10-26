#ifndef MURMUR_MESSAGE_DIFF_H
#define MURMUR_MESSAGE_DIFF_H
#include <cstddef>
#include <cstdint>
#include <vector>

/* What the page already has, versus what the app now holds. The bridge sends only the
   difference: messages dropped from the front, changed in place, appended at the end.
   Anything that does not fit that shape (new conversation, reordering) is a reset. */
struct SentMessage { uint32_t uid; uint32_t revision; };
struct MessageDiff {
    enum Kind { NONE, DELTA, RESET } kind = NONE;
    size_t drop = 0;                 /* messages removed from the front */
    size_t kept = 0;                 /* sent messages that remain (indices drop..drop+kept in `sent`) */
    std::vector<size_t> updated;     /* indices into `next` whose content changed */
};
inline MessageDiff plan_message_diff(const std::vector<SentMessage> &sent, const std::vector<SentMessage> &next) {
    MessageDiff plan;
    if (next.empty()) { if (!sent.empty()) plan.kind = MessageDiff::RESET; return plan; }
    size_t drop = 0;
    while (drop < sent.size() && sent[drop].uid != next[0].uid) ++drop;
    if (drop == sent.size()) { plan.kind = MessageDiff::RESET; return plan; }
    const size_t kept = sent.size() - drop;
    if (kept > next.size()) { plan.kind = MessageDiff::RESET; return plan; }
    for (size_t i = 0; i < kept; ++i) {
        if (sent[drop + i].uid != next[i].uid) { plan.kind = MessageDiff::RESET; plan.updated.clear(); return plan; }
        if (sent[drop + i].revision != next[i].revision) plan.updated.push_back(i);
    }
    plan.drop = drop; plan.kept = kept;
    plan.kind = (drop == 0 && plan.updated.empty() && kept == next.size()) ? MessageDiff::NONE : MessageDiff::DELTA;
    return plan;
}
#endif

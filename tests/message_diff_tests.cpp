#include "message_diff.h"
#include <cstdio>
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
static std::vector<SentMessage> list(uint32_t from, uint32_t to, uint32_t revision = 0) {
    std::vector<SentMessage> out;
    for (uint32_t i = from; i <= to; ++i) out.push_back({i, revision});
    return out;
}
int main() {
    /* Nothing yet: nothing to send. */
    CHECK(plan_message_diff({}, {}).kind == MessageDiff::NONE);
    /* First messages, or a new conversation: reset. */
    CHECK(plan_message_diff({}, list(1, 3)).kind == MessageDiff::RESET);
    CHECK(plan_message_diff(list(1, 3), list(7, 9)).kind == MessageDiff::RESET);
    /* Conversation cleared: reset to empty. */
    CHECK(plan_message_diff(list(1, 3), {}).kind == MessageDiff::RESET);
    /* Unchanged list. */
    CHECK(plan_message_diff(list(1, 5), list(1, 5)).kind == MessageDiff::NONE);
    /* Append. */
    { MessageDiff d = plan_message_diff(list(1, 5), list(1, 7));
      CHECK(d.kind == MessageDiff::DELTA && d.drop == 0 && d.kept == 5 && d.updated.empty()); }
    /* The oldest message leaves while a new one arrives (list capped at 128). */
    { MessageDiff d = plan_message_diff(list(1, 128), list(2, 129));
      CHECK(d.kind == MessageDiff::DELTA && d.drop == 1 && d.kept == 127); }
    /* Several dropped at once. */
    { MessageDiff d = plan_message_diff(list(1, 10), list(4, 12));
      CHECK(d.kind == MessageDiff::DELTA && d.drop == 3 && d.kept == 7); }
    /* A message changes in place (image upload finished). */
    { std::vector<SentMessage> next = list(1, 4); next[2].revision = 9;
      MessageDiff d = plan_message_diff(list(1, 4), next);
      CHECK(d.kind == MessageDiff::DELTA && d.updated.size() == 1 && d.updated[0] == 2 && d.kept == 4); }
    /* An update combined with a drop refers to indices in the new list. */
    { std::vector<SentMessage> sent = list(1, 4), next = list(2, 5); next[1].revision = 3;
      MessageDiff d = plan_message_diff(sent, next);
      CHECK(d.kind == MessageDiff::DELTA && d.drop == 1 && d.updated.size() == 1 && d.updated[0] == 1); }
    /* Reordering or a hole in the middle cannot be expressed as a delta. */
    { std::vector<SentMessage> next = list(1, 5); next.erase(next.begin() + 2);
      CHECK(plan_message_diff(list(1, 5), next).kind == MessageDiff::RESET); }
    /* Fewer messages than were kept (truncated tail) is a reset. */
    CHECK(plan_message_diff(list(1, 5), list(1, 3)).kind == MessageDiff::RESET);
    std::puts("Message diff planning passed.");
    return 0;
}

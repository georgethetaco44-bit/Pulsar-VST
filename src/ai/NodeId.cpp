// M11 node ids. See NodeId.h.

#include "ai/NodeId.h"

#include <cctype>

namespace vibesynth {

namespace {

std::string trimmed (const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace ((unsigned char) s[a]))
        ++a;
    while (b > a && std::isspace ((unsigned char) s[b - 1]))
        --b;
    return s.substr (a, b - a);
}

std::string lower (std::string s)
{
    for (auto& c : s)
        c = (char) std::tolower ((unsigned char) c);
    return s;
}

const char* ordinal (int i)
{
    static const char* const names[] = { "1st", "2nd", "3rd", "4th" };
    return i >= 0 && i < 4 ? names[i] : "4th or later";
}

} // namespace

bool parseNodeId (const std::string& idIn, NodeRef& out, std::string& error)
{
    std::string id = lower (trimmed (idIn));
    NodeRef n;
    for (const char* prefix : { "master/", "master:", "voice/", "voice:" })
    {
        const std::string p (prefix);
        if (id.rfind (p, 0) == 0)
        {
            n.master = p[0] == 'm';
            id = trimmed (id.substr (p.size()));
            break;
        }
    }
    // name, then an optional separator (':', '#', ' ', ' #') and a slot.
    size_t end = 0;
    while (end < id.size() && (std::isalnum ((unsigned char) id[end]) || id[end] == '_'))
        ++end;
    const std::string name = id.substr (0, end);
    std::string rest = trimmed (id.substr (end));
    if (! rest.empty() && (rest[0] == ':' || rest[0] == '#'))
        rest = trimmed (rest.substr (1));
    if (! rest.empty() && rest[0] == '#')
        rest = trimmed (rest.substr (1));

    if (name.empty() || ! moduleFromName (name, n.module))
    {
        error = "unknown node \"" + trimmed (idIn) + "\" (want e.g. flt_lp:0 or master/bus_sat#1)";
        return false;
    }
    if (! rest.empty())
    {
        if (rest.size() > 2 || ! std::isdigit ((unsigned char) rest[0])
            || (rest.size() == 2 && ! std::isdigit ((unsigned char) rest[1])))
        {
            error = "bad slot in node \"" + trimmed (idIn) + "\"";
            return false;
        }
        n.slot = std::stoi (rest);
    }
    if (n.slot < 0 || n.slot >= kMaxSlotsPerModule)
    {
        error = "slot " + std::to_string (n.slot) + " out of range in node \"" + trimmed (idIn) + "\"";
        return false;
    }
    out = n;
    return true;
}

std::string nodeIdString (const NodeRef& node)
{
    return std::string (node.master ? "master/" : "") + moduleShortName (node.module)
         + (isStatelessModule (node.module) ? "#" : ":") + std::to_string (node.slot);
}

std::string describeNodeForPrompt (const NodeRef& node)
{
    const std::string name = moduleShortName (node.module);
    const std::string section = node.master ? "master" : "voice";
    if (isStatelessModule (node.module))
        return nodeIdString (node) + ": the " + ordinal (node.slot) + " " + name + "(...) call in the " + section
             + " section, counting in the order the calls run each sample";
    return nodeIdString (node) + ": the " + name + "(" + std::to_string (node.slot) + ", ...) call in the "
         + section + " section";
}

bool nodeIsTouched (const TouchedNodes& touched, const NodeRef& node) noexcept
{
    const int m = static_cast<int> (node.module);
    if (m < 0 || m >= kNumModules || node.slot < 0 || node.slot >= kMaxSlotsPerModule)
        return false;
    const uint32_t bits = node.master ? touched.master[m] : touched.voice[m];
    return (bits & (1u << node.slot)) != 0u;
}

std::string lostNodes (const TouchedNodes& before, const TouchedNodes& after)
{
    std::string out;
    for (int section = 0; section < 2; ++section)
        for (int m = 0; m < kNumModules; ++m)
            for (int s = 0; s < kMaxSlotsPerModule; ++s)
            {
                NodeRef n { section == 1, static_cast<ModuleId> (m), s };
                if (nodeIsTouched (before, n) && ! nodeIsTouched (after, n))
                    out += (out.empty() ? "" : ", ") + nodeIdString (n);
            }
    return out;
}

} // namespace vibesynth

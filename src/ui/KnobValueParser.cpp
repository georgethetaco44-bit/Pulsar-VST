// M14b value parser. See KnobValueParser.h.

#include "ui/KnobValueParser.h"

#include <cmath>
#include <string>

#include "engine/Modulators.h"

namespace vibesynth::ui {

namespace {

// Parses a leading float, allowing optional leading '+' or '-'.
// Returns false if no valid number could be parsed or if trailing text remains
// that isn't matched by unit check.
bool parseLeadingFloat (const juce::String& s, double& outNum)
{
    const auto trimmed = s.trim();
    if (trimmed.isEmpty())
        return false;

    // Check if it's a valid floating point representation
    int i = 0;
    if (trimmed[i] == '+' || trimmed[i] == '-')
        ++i;
    bool hasDigit = false;
    bool hasDot = false;
    while (i < trimmed.length())
    {
        const juce::juce_wchar c = trimmed[i];
        if (c >= '0' && c <= '9')
        {
            hasDigit = true;
            ++i;
        }
        else if (c == '.' && ! hasDot)
        {
            hasDot = true;
            ++i;
        }
        else
        {
            break;
        }
    }
    if (! hasDigit)
        return false;

    if (i < trimmed.length())
        return false; // Trailing non-numeric chars

    outNum = trimmed.getDoubleValue();
    return true;
}

} // namespace

KnobParseResult parseKnobValue (const juce::String& rawText, KnobUnitKind kind,
                                double minVal, double maxVal)
{
    KnobParseResult result;
    juce::String s = rawText.trim().toLowerCase();
    if (s.isEmpty())
        return result;

    if (minVal > maxVal)
        std::swap (minVal, maxVal);

    enum class Unit
    {
        None,
        Hz,
        KHz,
        Db,
        Semitones,
        Ms,
        Sec,
        Percent,
        Off
    };

    Unit unit = Unit::None;
    juce::String numStr = s;
    if (s == "off")
    {
        unit = Unit::Off;
    }
    else if (s.endsWith ("khz"))
    {
        numStr = s.dropLastCharacters (3).trim();
        unit = Unit::KHz;
    }
    else if (s.endsWith ("hz"))
    {
        numStr = s.dropLastCharacters (2).trim();
        unit = Unit::Hz;
    }
    else if (s.endsWith ("k"))
    {
        numStr = s.dropLastCharacters (1).trim();
        unit = Unit::KHz;
    }
    else if (s.endsWith ("db"))
    {
        numStr = s.dropLastCharacters (2).trim();
        unit = Unit::Db;
    }
    else if (s.endsWith ("st"))
    {
        numStr = s.dropLastCharacters (2).trim();
        unit = Unit::Semitones;
    }
    else if (s.endsWith ("ms"))
    {
        numStr = s.dropLastCharacters (2).trim();
        unit = Unit::Ms;
    }
    else if (s.endsWith ("s") && s.length() > 1 && (std::isdigit (static_cast<unsigned char> (s[s.length() - 2])) || s[s.length() - 2] == ' '))
    {
        numStr = s.dropLastCharacters (1).trim();
        unit = Unit::Sec;
    }
    else if (s.endsWith ("%"))
    {
        numStr = s.dropLastCharacters (1).trim();
        unit = Unit::Percent;
    }

    // 1. Synced rate (LFO sync division)
    if (kind == KnobUnitKind::SyncedRate)
    {
        if (unit != Unit::None)
            return result;
        for (int i = 0; i < static_cast<int> (LfoSyncDiv::Count); ++i)
        {
            const auto div = static_cast<LfoSyncDiv> (i);
            const juce::String label = juce::String (lfoSyncDivLabel (div)).toLowerCase();
            const juce::String id = juce::String (lfoSyncDivId (div)).toLowerCase();
            if (s == label || s == id)
            {
                result.ok = true;
                result.value = static_cast<double> (i);
                return result;
            }
        }
        return result;
    }

    // 2. Glide
    if (kind == KnobUnitKind::Glide)
    {
        if (unit == Unit::Off)
        {
            result.ok = true;
            result.value = 0.0;
            return result;
        }
        if (unit != Unit::None && unit != Unit::Ms && unit != Unit::Sec)
            return result;
        double val = 0.0;
        if (! parseLeadingFloat (numStr, val))
            return result;
        double ms = 0.0;
        if (unit == Unit::Ms)
            ms = val;
        else if (unit == Unit::Sec)
            ms = val * 1000.0;
        else
            ms = val > 1.0 ? val : val * 1000.0;
        ms = juce::jmax (0.0, ms);
        double g = std::sqrt (ms / 1000.0);
        result.ok = true;
        result.value = juce::jlimit (minVal, maxVal, g);
        return result;
    }

    // 3. Time (Envelope A, D, R in seconds)
    if (kind == KnobUnitKind::Time)
    {
        if (unit != Unit::None && unit != Unit::Ms && unit != Unit::Sec)
            return result;
        double val = 0.0;
        if (! parseLeadingFloat (numStr, val))
            return result;
        double sec = 0.0;
        if (unit == Unit::Ms)
            sec = val / 1000.0;
        else if (unit == Unit::Sec)
            sec = val;
        else
            sec = val > 10.0 ? val / 1000.0 : val;
        result.ok = true;
        result.value = juce::jlimit (minVal, maxVal, sec);
        return result;
    }

    // 4. Semitones
    if (kind == KnobUnitKind::Semitones)
    {
        if (unit != Unit::None && unit != Unit::Semitones)
            return result;
        double val = 0.0;
        if (! parseLeadingFloat (numStr, val))
            return result;
        result.ok = true;
        result.value = juce::jlimit (minVal, maxVal, val);
        return result;
    }

    // 5. Hz / kHz
    if (kind == KnobUnitKind::Hz)
    {
        if (unit != Unit::None && unit != Unit::Hz && unit != Unit::KHz)
            return result;
        double val = 0.0;
        if (! parseLeadingFloat (numStr, val))
            return result;
        double hz = unit == Unit::KHz ? val * 1000.0 : val;
        result.ok = true;
        result.value = juce::jlimit (minVal, maxVal, hz);
        result.isHz = true;
        return result;
    }

    // 6. Decibels (dB)
    if (kind == KnobUnitKind::Decibels)
    {
        if (unit != Unit::None && unit != Unit::Db)
            return result;
        double val = 0.0;
        if (! parseLeadingFloat (numStr, val))
            return result;
        result.ok = true;
        result.value = juce::jlimit (minVal, maxVal, val);
        return result;
    }

    // 7. Macro
    if (kind == KnobUnitKind::Macro)
    {
        if (unit != Unit::None && unit != Unit::Percent)
            return result;
        double val = 0.0;
        if (! parseLeadingFloat (numStr, val))
            return result;
        if (unit == Unit::Percent)
            val /= 100.0;
        result.ok = true;
        result.value = juce::jlimit (minVal, maxVal, val);
        return result;
    }

    // 8. FilterCutoff (SPEC §9.6, M14e)
    if (kind == KnobUnitKind::FilterCutoff)
    {
        if (unit == Unit::Hz || unit == Unit::KHz)
        {
            double val = 0.0;
            if (! parseLeadingFloat (numStr, val))
                return result;
            double hz = unit == Unit::KHz ? val * 1000.0 : val;
            double x = 0.0;
            if (hz <= 20.0)
                x = 0.0;
            else if (hz >= 20000.0)
                x = 1.0;
            else
                x = std::log (hz / 20.0) / std::log (1000.0);
            result.ok = true;
            result.value = juce::jlimit (0.0, 1.0, x);
            result.isHz = true;
            return result;
        }
        if (unit == Unit::Percent)
        {
            double val = 0.0;
            if (! parseLeadingFloat (numStr, val))
                return result;
            result.ok = true;
            result.value = juce::jlimit (minVal, maxVal, val / 100.0);
            result.isHz = false;
            return result;
        }
        if (unit == Unit::None)
        {
            double val = 0.0;
            if (! parseLeadingFloat (numStr, val))
                return result;
            result.ok = true;
            result.value = juce::jlimit (minVal, maxVal, val);
            result.isHz = false;
            return result;
        }
        // Any other unit (e.g. dB, st, ms) on a cutoff is rejected
        return result;
    }

    // 9. Choice
    if (kind == KnobUnitKind::Choice)
    {
        if (unit != Unit::None)
            return result;
        double val = 0.0;
        if (! parseLeadingFloat (numStr, val))
            return result;
        val = std::floor (val + 0.5);
        result.ok = true;
        result.value = juce::jlimit (minVal, maxVal, val);
        return result;
    }

    // 10. Generic (0-1 amount, etc.)
    if (kind == KnobUnitKind::Generic)
    {
        if (unit == Unit::Percent)
        {
            double val = 0.0;
            if (! parseLeadingFloat (numStr, val))
                return result;
            result.ok = true;
            result.value = juce::jlimit (minVal, maxVal, val / 100.0);
            return result;
        }
        if (unit != Unit::None)
            return result; // Any unit on generic knob flashes red and changes nothing
        double val = 0.0;
        if (! parseLeadingFloat (numStr, val))
            return result;
        result.ok = true;
        result.value = juce::jlimit (minVal, maxVal, val);
        return result;
    }

    return result;
}

} // namespace vibesynth::ui

#pragma once

#include <functional>

namespace meridian::dsp
{

/** Exact floating-point equality, for the places that mean it: "has this value
    changed at all?", or "is this exactly the stored value?". Writing it out
    keeps -Wfloat-equal useful for catching accidental == on floats everywhere
    else (the comparison happens inside std::equal_to, where the warning does
    not apply). Equivalent to juce::exactlyEqual, for code that cannot use JUCE. */
template <typename T>
constexpr bool exactlyEqual (T a, T b) noexcept
{
    return std::equal_to<T>() (a, b);
}

} // namespace meridian::dsp

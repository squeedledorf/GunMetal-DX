/**
 * @file slfxini.cpp
 * @brief A ReShade preset (.ini) as a document that writes back what it read
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer Project.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include "slfxini.h"

#include <algorithm>
#include <cctype>

namespace
{
std::string trim(const std::string& s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t'))
    {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t'))
    {
        --e;
    }
    return s.substr(b, e - b);
}

bool iequals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
        {
            return false;
        }
    }
    return true;
}
} // namespace

void SLFxIniDocument::parse(const std::string& text_in)
{
    mSections.clear();
    mSections.emplace_back(); // global
    mNewline = "\n";
    mBOM = false;

    std::string text = text_in;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
    {
        mBOM = true;
        text.erase(0, 3);
    }

    bool newline_seen = false;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t nl = text.find('\n', pos);
        std::string raw;
        std::string eol;
        if (nl == std::string::npos)
        {
            raw = text.substr(pos);
            pos = text.size();
        }
        else
        {
            raw = text.substr(pos, nl - pos);
            eol = "\n";
            pos = nl + 1;
        }
        if (!raw.empty() && raw.back() == '\r')
        {
            raw.pop_back();
            eol = "\r" + eol;
        }
        if (!newline_seen && !eol.empty())
        {
            mNewline = eol;
            newline_seen = true;
        }

        const std::string t = trim(raw);
        if (t.size() >= 2 && t.front() == '[' && t.back() == ']')
        {
            Section section;
            section.mName = trim(t.substr(1, t.size() - 2));
            section.mHeaderRaw = raw;
            section.mHeaderEol = eol;
            mSections.push_back(std::move(section));
            continue;
        }

        Line line;
        line.mRaw = raw;
        line.mEol = eol;
        const size_t eq = t.find('=');
        if (!t.empty() && t[0] != ';' && t[0] != '#' && eq != std::string::npos && eq > 0)
        {
            line.mKind = Line::KEY;
            line.mKey = trim(t.substr(0, eq));
            line.mValue = trim(t.substr(eq + 1));
        }
        mSections.back().mLines.push_back(std::move(line));
    }
}

// static
std::string SLFxIniDocument::renderLine(const Line& line)
{
    if (line.mKind == Line::KEY && line.mDirty)
    {
        // ReShade writes no spaces around '='.
        return line.mKey + "=" + line.mValue;
    }
    return line.mRaw;
}

std::string SLFxIniDocument::write() const
{
    // Every piece is written with the ending it was read with. A last line
    // that had none gets one only if something now follows it.
    std::vector<std::pair<std::string, std::string>> out; // text, eol
    for (const Section& section : mSections)
    {
        if (&section != &mSections.front())
        {
            out.emplace_back(section.mHeaderRaw.empty() ? "[" + section.mName + "]" : section.mHeaderRaw,
                             section.mHeaderEol);
        }
        for (const Line& line : section.mLines)
        {
            out.emplace_back(renderLine(line), line.mEol);
        }
    }

    std::string text;
    if (mBOM)
    {
        text = "\xEF\xBB\xBF";
    }
    for (size_t i = 0; i < out.size(); ++i)
    {
        text += out[i].first;
        if (!out[i].second.empty())
        {
            text += out[i].second;
        }
        else if (i + 1 < out.size())
        {
            text += mNewline;
        }
    }
    return text;
}

SLFxIniDocument::Section* SLFxIniDocument::findSection(const std::string& name)
{
    return const_cast<Section*>(static_cast<const SLFxIniDocument*>(this)->findSection(name));
}

const SLFxIniDocument::Section* SLFxIniDocument::findSection(const std::string& name) const
{
    if (name.empty())
    {
        return mSections.empty() ? nullptr : &mSections.front();
    }
    for (size_t i = 1; i < mSections.size(); ++i)
    {
        if (iequals(mSections[i].mName, name))
        {
            return &mSections[i];
        }
    }
    return nullptr;
}

std::vector<std::string> SLFxIniDocument::sectionNames() const
{
    std::vector<std::string> names;
    for (size_t i = 0; i < mSections.size(); ++i)
    {
        if (i == 0)
        {
            const bool has_keys = std::any_of(mSections[0].mLines.begin(), mSections[0].mLines.end(),
                                              [](const Line& l) { return l.mKind == Line::KEY; });
            if (has_keys)
            {
                names.emplace_back();
            }
            continue;
        }
        names.push_back(mSections[i].mName);
    }
    return names;
}

bool SLFxIniDocument::hasSection(const std::string& section) const
{
    return section.empty() || findSection(section) != nullptr;
}

std::vector<std::pair<std::string, std::string>> SLFxIniDocument::entries(const std::string& section) const
{
    std::vector<std::pair<std::string, std::string>> result;
    if (const Section* s = findSection(section))
    {
        for (const Line& line : s->mLines)
        {
            if (line.mKind == Line::KEY)
            {
                result.emplace_back(line.mKey, line.mValue);
            }
        }
    }
    return result;
}

const std::string* SLFxIniDocument::get(const std::string& section, const std::string& key) const
{
    if (const Section* s = findSection(section))
    {
        // The last one wins, as in ReShade's reader, which overwrites.
        const std::string* found = nullptr;
        for (const Line& line : s->mLines)
        {
            if (line.mKind == Line::KEY && line.mKey == key)
            {
                found = &line.mValue;
            }
        }
        return found;
    }
    return nullptr;
}

// static
bool SLFxIniDocument::keysSorted(const Section& section)
{
    const std::string* prev = nullptr;
    for (const Line& line : section.mLines)
    {
        if (line.mKind != Line::KEY)
        {
            continue;
        }
        if (prev && line.mKey < *prev)
        {
            return false;
        }
        prev = &line.mKey;
    }
    return true;
}

SLFxIniDocument::Section& SLFxIniDocument::addSection(const std::string& name)
{
    if (mSections.empty())
    {
        mSections.emplace_back();
    }

    // Sorted among the existing sections if they are sorted (ReShade's own
    // output always is), else at the end.
    bool sorted = true;
    for (size_t i = 2; i < mSections.size(); ++i)
    {
        if (mSections[i].mName < mSections[i - 1].mName)
        {
            sorted = false;
            break;
        }
    }
    size_t at = mSections.size();
    if (sorted)
    {
        for (size_t i = 1; i < mSections.size(); ++i)
        {
            if (name < mSections[i].mName)
            {
                at = i;
                break;
            }
        }
    }

    Section section;
    section.mName = name;
    section.mHeaderRaw = "[" + name + "]";
    section.mHeaderEol = mNewline;

    // A blank line after it, as ReShade separates sections; and when it goes
    // last, a blank line before it too unless one is already there.
    Line blank;
    blank.mEol = mNewline;
    if (at == mSections.size())
    {
        Section& prev = mSections.back();
        const bool prev_has_content = !prev.mLines.empty() || &prev != &mSections.front();
        const bool prev_ends_blank = !prev.mLines.empty() && prev.mLines.back().mKind == Line::OTHER &&
                                     trim(prev.mLines.back().mRaw).empty();
        if (prev_has_content && !prev_ends_blank)
        {
            if (!prev.mLines.empty() && prev.mLines.back().mEol.empty())
            {
                prev.mLines.back().mEol = mNewline;
            }
            prev.mLines.push_back(blank);
        }
        else if (!prev.mLines.empty() && prev.mLines.back().mEol.empty())
        {
            prev.mLines.back().mEol = mNewline;
        }
    }
    else
    {
        section.mLines.push_back(blank);
    }
    mSections.insert(mSections.begin() + at, std::move(section));
    return mSections[at];
}

void SLFxIniDocument::set(const std::string& section_name, const std::string& key, const std::string& value)
{
    if (mSections.empty())
    {
        mSections.emplace_back();
    }
    Section* section = findSection(section_name);
    if (!section)
    {
        section = &addSection(section_name);
    }

    // Existing key: change the last occurrence (the one a reader keeps).
    for (auto it = section->mLines.rbegin(); it != section->mLines.rend(); ++it)
    {
        if (it->mKind == Line::KEY && it->mKey == key)
        {
            if (it->mValue != value)
            {
                it->mValue = value;
                it->mDirty = true;
            }
            return;
        }
    }

    Line line;
    line.mKind = Line::KEY;
    line.mKey = key;
    line.mValue = value;
    line.mDirty = true;
    line.mEol = mNewline;

    // Where: before the first greater key if the keys are in order, else
    // after the last key; before any trailing blank lines either way.
    std::vector<Line>& lines = section->mLines;
    size_t insert_at = 0;
    const bool sorted = keysSorted(*section);
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (lines[i].mKind != Line::KEY)
        {
            continue;
        }
        if (sorted && key < lines[i].mKey)
        {
            insert_at = i;
            break;
        }
        insert_at = i + 1;
    }
    if (insert_at > 0 && lines[insert_at - 1].mEol.empty())
    {
        // The line before was the file's last and had no ending.
        lines[insert_at - 1].mEol = mNewline;
        if (insert_at == lines.size())
        {
            line.mEol.clear();
        }
    }
    lines.insert(lines.begin() + insert_at, std::move(line));
}

bool SLFxIniDocument::remove(const std::string& section_name, const std::string& key)
{
    Section* section = findSection(section_name);
    if (!section)
    {
        return false;
    }
    bool removed = false;
    for (auto it = section->mLines.begin(); it != section->mLines.end();)
    {
        if (it->mKind == Line::KEY && it->mKey == key)
        {
            it = section->mLines.erase(it);
            removed = true;
        }
        else
        {
            ++it;
        }
    }
    return removed;
}

// static
std::vector<std::string> SLFxIniDocument::splitList(const std::string& value)
{
    std::vector<std::string> items;
    if (value.empty())
    {
        return items;
    }
    size_t pos = 0;
    while (true)
    {
        const size_t comma = value.find(',', pos);
        if (comma == std::string::npos)
        {
            items.push_back(value.substr(pos));
            break;
        }
        items.push_back(value.substr(pos, comma - pos));
        pos = comma + 1;
    }
    return items;
}

// static
std::string SLFxIniDocument::joinList(const std::vector<std::string>& items)
{
    std::string out;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i)
        {
            out += ',';
        }
        out += items[i];
    }
    return out;
}

/**
 * @file slfxini.h
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

#ifndef SL_FXINI_H
#define SL_FXINI_H

#include <string>
#include <utility>
#include <vector>

/**
 * An .ini file held line by line, so that writing it back reproduces every
 * byte nobody changed: the order of sections and keys, blank lines, comments,
 * lines it does not understand, CRLF or LF, a UTF-8 byte order mark, and
 * whether the last line ends in a newline.
 *
 * That is the whole point of the class. ReShade presets are shared between
 * users and between ReShade and us; a preset that comes back from us with
 * every value re-printed, its keys re-sorted or its line endings changed is a
 * diff the user did not make. So a value is only re-rendered when set() is
 * given a different string, and a key or section that has to be added goes
 * where ReShade itself would have put it (keys in a section and sections in a
 * file are sorted when ReShade writes them; we insert in order when what is
 * already there is in order, and append otherwise).
 *
 * The format, as ReShade reads it: an optional run of key=value lines before
 * any [section] (the "global" section, name ""), then [section] blocks. Keys
 * and values are trimmed; a value is a comma-separated list when the reader
 * wants one. Lines starting with ';' or '#' are comments.
 *
 * Plain std only, so it can be tested outside the viewer.
 */
class SLFxIniDocument
{
public:
    /// Read @a text. Never fails: what is not a section header or key=value
    /// is kept as an opaque line.
    void parse(const std::string& text);
    /// The file, byte for byte what parse() read, plus whatever set()/remove()
    /// changed.
    std::string write() const;

    /// Section names in file order, the global section ("") first if it has
    /// any keys.
    std::vector<std::string> sectionNames() const;
    bool hasSection(const std::string& section) const;
    /// key -> value pairs of @a section, in file order. Section names match
    /// case-insensitively (an effect file is "Clarity.fx" on one machine and
    /// "clarity.fx" on another); keys match exactly, as ReShade's do.
    std::vector<std::pair<std::string, std::string>> entries(const std::string& section) const;
    /// The value, or null.
    const std::string* get(const std::string& section, const std::string& key) const;

    /// Set a value. A value equal to what is there leaves the line alone; a
    /// new key is inserted (see the class comment); a new section is created.
    void set(const std::string& section, const std::string& key, const std::string& value);
    /// Remove a key. True if it was there.
    bool remove(const std::string& section, const std::string& key);

    /// ReShade's list syntax: split on ',' with no trimming, "" -> empty list.
    static std::vector<std::string> splitList(const std::string& value);
    static std::string joinList(const std::vector<std::string>& items);

    /// The line ending the file uses ("\r\n" or "\n").
    const std::string& newline() const { return mNewline; }

private:
    struct Line
    {
        enum EKind
        {
            OTHER,  // blank, comment, or not understood: written as read
            KEY     // key=value
        };
        EKind       mKind = OTHER;
        std::string mRaw;     // as read, without its line ending
        std::string mEol;     // its line ending as read; "" on a last line without one
        std::string mKey;     // trimmed
        std::string mValue;   // trimmed
        bool        mDirty = false;
    };
    struct Section
    {
        std::string       mName;       // "" for the global section
        std::string       mHeaderRaw;  // the "[name]" line as read; empty for global
        std::string       mHeaderEol;
        std::vector<Line> mLines;
    };

    Section* findSection(const std::string& name);
    const Section* findSection(const std::string& name) const;
    Section& addSection(const std::string& name);
    static std::string renderLine(const Line& line);
    static bool keysSorted(const Section& section);

    std::vector<Section> mSections; // [0] is always the global section
    /// The ending new lines get: the file's first one, else "\n".
    std::string mNewline = "\n";
    bool mBOM = false;
};

#endif // SL_FXINI_H

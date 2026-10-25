/**
 * @file llmeshbulkupload.h
 * @brief Bulk mesh upload (#780): companion mesh detection by name.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#ifndef LL_LLMESHBULKUPLOAD_H
#define LL_LLMESHBULKUPLOAD_H

#include <string>
#include <utility>
#include <vector>

// Name rules for bulk model upload. A name (file stem, or an object name inside a
// file) is split into a base name and a role:
//   physics:  suffix _phys/_physics/_col/_collision (any case), the configured
//             MeshPhysicsSuffix, or prefix UCX_/UBX_/UCP_/USP_ (with an optional
//             trailing _NN, as in UCX_Chair_01)
//   LODs:     suffix _LOD0.._LOD3 (or the configured Mesh*LodSuffix values, which win),
//             _lowest/_low/_med/_medium/_high
//   visual:   anything else
// Matching is case-insensitive; the base keeps the original case. A trailing
// Blender-style ".NNN" is ignored.
namespace LLMeshBulk
{
    // Values 0..4 are the LLModel LOD indices (LOD_IMPOSTOR .. LOD_PHYSICS).
    enum ERole
    {
        ROLE_LOD0 = 0,      // lowest
        ROLE_LOD1 = 1,      // low
        ROLE_LOD2 = 2,      // medium
        ROLE_HIGH = 3,      // explicitly named high LOD
        ROLE_PHYSICS = 4,
        ROLE_VISUAL = 5,    // no companion marker: the model itself
    };

    struct NameInfo
    {
        std::string mBase;  // original case
        ERole mRole = ROLE_VISUAL;
    };

    // Suffix table: lower-case suffix (with its leading '_') -> role, longest first.
    typedef std::vector<std::pair<std::string, ERole> > suffix_table_t;

    // Built from the Mesh*LodSuffix / MeshPhysicsSuffix settings plus the built-in aliases.
    suffix_table_t buildSuffixTable();

    NameInfo classifyName(const std::string& name, const suffix_table_t& suffixes);
    NameInfo classifyName(const std::string& name);   // uses buildSuffixTable()

    std::string toLowerCopy(const std::string& s);

    // True for the model formats the uploader reads (.dae .gltf .glb .fbx).
    bool isModelFile(const std::string& path);

    // Non-recursive listing of the model files in a directory, sorted by name.
    std::vector<std::string> listModelFiles(const std::string& dir);

    struct Group
    {
        std::string mName;                  // base name (display / match key)
        std::string mVisual;                // high LOD (the model) file; empty = no visual found
        std::string mLODFile[4];            // separate LOD files by LOD index (3 unused)
        std::string mPhysicsFile;           // separate physics file
        std::vector<std::string> mNotes;    // detection remarks (duplicates, ignored files)
    };

    // Groups files by base name. Groups without a visual file are returned too
    // (mVisual empty) so the caller can show them as unmatched companions.
    // Order follows the first appearance of each base in 'files' after sorting.
    std::vector<Group> groupFiles(const std::vector<std::string>& files);
}

#endif // LL_LLMESHBULKUPLOAD_H

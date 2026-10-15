// Comparing a folder as the folder scanner lists it with the same folder in the
// tree read from its drive's master file table.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../../src/Core/FolderTree.h"

namespace test
{
    struct Difference
    {
        std::string what;  // "size on disk", "only listed", ...
        std::wstring path;
        std::uint64_t listed = 0;
        std::uint64_t table = 0;
    };

    // Compares the trees below `listed` and `table` file by file: kind, size
    // on disk, length, attributes and date.  Folders are compared by what they
    // hold (NTFS refreshes the dates a listing shows for folders only now and
    // then).  `path` is the folder's, for the report.
    std::vector<Difference> CompareTrees(sm::Folder const& listed, sm::Folder const& table, std::wstring const& path,
                                         std::uint64_t* filesCompared = nullptr);

    // The folder at `relative` (such as L"Users\\Public") below `root`; null if it is not there.
    sm::Folder const* FindFolder(sm::Folder const& root, std::wstring_view relative);
}

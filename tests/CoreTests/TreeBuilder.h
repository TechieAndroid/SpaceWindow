// Builds folder trees in memory for the layout tests.
#pragma once

#include "../../src/Core/FolderTree.h"

namespace test
{
    inline void AddFile(sm::Folder& folder, std::wstring_view name, std::uint64_t size)
    {
        sm::Entry entry;
        entry.kind = sm::EntryKind::File;
        entry.size = entry.logicalSize = size;
        entry.nameOffset = static_cast<std::uint32_t>(folder.names.size());
        entry.nameLength = static_cast<std::uint16_t>(name.size());
        folder.names.append(name);
        folder.entries.push_back(std::move(entry));
    }

    inline void AddSpecial(sm::Folder& folder, sm::EntryKind kind, std::uint64_t size)
    {
        sm::Entry entry;
        entry.kind = kind;
        entry.size = entry.logicalSize = size;
        folder.entries.push_back(std::move(entry));
    }

    inline sm::Folder& AddFolder(sm::Folder& parent, std::wstring_view name)
    {
        sm::Entry entry;
        entry.kind = sm::EntryKind::Folder;
        entry.nameOffset = static_cast<std::uint32_t>(parent.names.size());
        entry.nameLength = static_cast<std::uint16_t>(name.size());
        parent.names.append(name);
        entry.child = std::make_unique<sm::Folder>();
        entry.child->parent = &parent;
        sm::Folder& child = *entry.child;
        parent.entries.push_back(std::move(entry));
        return child;
    }

    // Totals and sorts a hand-built tree the way the scanner finalizes one.
    inline void Finish(sm::Folder& folder)
    {
        folder.sizeFiles = 0;
        folder.sizeSubfolders = 0;
        for (sm::Entry& entry : folder.entries)
        {
            if (entry.child)
            {
                Finish(*entry.child);
                entry.size = entry.child->SizeTotal();
                folder.sizeSubfolders += entry.size;
            }
            else if (entry.kind == sm::EntryKind::File)
            {
                folder.sizeFiles += entry.size;
            }
        }
        sm::SortEntries(folder);
    }
}

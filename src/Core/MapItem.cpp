// SpaceWindow - an entry as drawn by one of the map views.
#include "pch.h"
#include "MapItem.h"

#include "FolderTree.h"

namespace sm
{
    Entry const* MapItem::GetEntry() const noexcept
    {
        return HasEntry() && index < source->entries.size() ? &source->entries[index] : nullptr;
    }

    Folder* MapItem::ChildFolder() const noexcept
    {
        Entry const* const entry = GetEntry();
        return entry != nullptr ? entry->child.get() : nullptr;
    }
}

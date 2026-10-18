// SpaceWindow - bringing a scanned tree up to date from the NTFS change journal.
//
// New in SpaceWindow.  NTFS logs every change to a volume's files in its
// change journal (the "USN journal").  A tree read from the master file table
// remembers where the journal stood; a rescan then reads only the files the
// journal names since, re-reads their records and patches the tree in place -
// usually in a fraction of a second, where a full scan takes several.
//
// When the journal cannot tell what changed - it was deleted or recreated, or
// has dropped the entries since the tree's position - or so much changed that
// scanning again is quicker, a full scan is the fallback.
#pragma once

#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "FolderTree.h"

namespace sm
{
    namespace ntfs
    {
        class Volume;
    }

    // A file the journal says changed, as it is now.
    struct ChangedFile
    {
        std::uint32_t record = 0;
        bool exists = false;  // in use, and one of the volume's files (not the file system's own)
        bool directory = false;
        bool alias = false;  // a junction or symbolic link, which the tree leaves out
        std::uint32_t attributes = 0;
        std::uint64_t lastWriteTime = 0;
        std::uint64_t size = 0;                                     // bytes on disk
        std::uint64_t length = 0;                                   // bytes of data
        std::vector<std::pair<std::uint32_t, std::wstring>> names;  // folder record, name (one per hard link)
        std::vector<std::uint32_t> formerFolders;  // folders the journal saw it in, where it may still be listed
        bool recheck = false;                      // read again without the journal naming it, in case it grew unseen
    };

    struct JournalChanges
    {
        std::vector<ChangedFile> files;
        JournalPosition position;  // where the journal stands once they are applied
    };

    enum class JournalReadStatus
    {
        Ready,        // `changes` holds what changed
        Unavailable,  // the journal cannot tell, or too much changed: scan again
        Cancelled,
    };

    struct JournalRead
    {
        JournalReadStatus status = JournalReadStatus::Unavailable;
        JournalChanges changes;
    };

#ifdef _WIN32
    // Where the volume's change journal stands now; nothing if it keeps none.
    std::optional<JournalPosition> CurrentJournalPosition(ntfs::Volume const& volume);

    // Reads what changed on the volume since `from`.  With `lookbackBytes`,
    // files changed in that much of the journal before `from`, at `recentSince`
    // (a FILETIME) or later, count as changed too.  The files of `recheck`, and
    // those still open at `from`, are read again whatever the journal says.
    // Blocks; call it from a background thread.
    JournalRead ReadJournalChanges(JournalPosition const& from, std::stop_token const& stop,
                                   std::int64_t lookbackBytes = 0, std::uint64_t recentSince = 0,
                                   std::span<std::uint32_t const> recheck = {});
#endif

    // The journal names a changed file only once until it is closed, so a file
    // kept open - a download, a virtual machine's disk, a database, a log -
    // grows unseen.  Those seen changing are read again until they close
    // (JournalPosition::openFiles); files already open before the tree was
    // read cannot be told apart, so the large ones - what such files grow
    // into, and what shows on the map - are worth reading again on each rescan.
    inline constexpr std::uint64_t kLargeFile = std::uint64_t{ 16 } << 20;

    // The records of the tree's files of kLargeFile or more, sorted.  From
    // the tree's JournalIndex, which is built here if it has none: call it
    // while nothing changes the tree.
    std::vector<std::uint32_t> LargeFiles(FolderTree& tree);

    // Applies changes to a tree read from the same volume, and moves the
    // tree's journal position on.  Keeps the tree's JournalIndex up to date
    // (building it first if need be), so the next reload starts without
    // walking the tree.  Quick: call it on the thread that owns the tree.
    void ApplyJournalChanges(FolderTree& tree, JournalChanges const& changes);
}

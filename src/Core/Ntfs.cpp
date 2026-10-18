// SpaceWindow - reading NTFS volumes directly.
//
// Records are read field by field at the fixed offsets of NTFS's on-disk
// structures: the file record header, the attribute header, and the values of
// the attributes used here.  Every offset and length read from the disk is
// checked against the record before it is used.
#include "pch.h"
#include "Ntfs.h"

#include "ChangeJournal.h"
#include "Fields.h"
#include "Parallel.h"
#ifndef _WIN32
#include "Posix.h"
#endif

namespace sm::ntfs
{
    namespace
    {
        static_assert(std::endian::native == std::endian::little, "NTFS structures are little-endian");

        using fields::At;
        using fields::Slice;

        // Attribute types.
        constexpr std::uint32_t kStandardInformation = 0x10;
        constexpr std::uint32_t kAttributeList = 0x20;
        constexpr std::uint32_t kFileNameAttribute = 0x30;
        constexpr std::uint32_t kData = 0x80;
        constexpr std::uint32_t kReparsePoint = 0xC0;
        constexpr std::uint32_t kEndOfAttributes = 0xFFFFFFFF;

        // The file record header.
        constexpr std::uint32_t kFileSignature = 0x454C4946;  // "FILE"
        constexpr std::size_t kUpdateSequenceOffset = 0x04;
        constexpr std::size_t kUpdateSequenceCount = 0x06;
        constexpr std::size_t kFirstAttributeOffset = 0x14;
        constexpr std::size_t kRecordFlags = 0x16;
        constexpr std::size_t kBytesInUse = 0x18;
        constexpr std::size_t kBaseRecord = 0x20;
        constexpr std::uint16_t kRecordInUse = 0x0001;
        constexpr std::uint16_t kRecordIsDirectory = 0x0002;
        constexpr std::size_t kFixupStride = 512;  // the update sequence covers every 512 bytes

        // The attribute header.
        constexpr std::size_t kAttributeLength = 0x04;
        constexpr std::size_t kNonResident = 0x08;
        constexpr std::size_t kNameLength = 0x09;
        constexpr std::size_t kNameOffset = 0x0A;
        constexpr std::size_t kAttributeFlags = 0x0C;
        constexpr std::size_t kValueLength = 0x10;  // resident
        constexpr std::size_t kValueOffset = 0x14;
        constexpr std::size_t kLowestVcn = 0x10;  // non-resident
        constexpr std::size_t kMappingPairsOffset = 0x20;
        constexpr std::size_t kAllocatedLength = 0x28;
        constexpr std::size_t kFileSize = 0x30;
        constexpr std::size_t kTotalAllocated = 0x40;  // only when compressed or sparse
        constexpr std::uint16_t kCompressedMask = 0x00FF;
        constexpr std::uint16_t kSparse = 0x8000;

        // Attribute values.
        constexpr std::size_t kStandardLastWrite = 0x08;
        constexpr std::size_t kStandardAttributes = 0x20;
        constexpr std::size_t kNameParent = 0x00;
        constexpr std::size_t kNameAttributes = 0x38;
        constexpr std::size_t kNameReparseTag = 0x3C;
        constexpr std::size_t kNameCharacters = 0x40;
        constexpr std::size_t kNameSpace = 0x41;
        constexpr std::size_t kNameText = 0x42;
        constexpr std::uint8_t kDosNameSpace = 2;  // an 8.3 alias of a long name
        constexpr std::size_t kListEntryLength = 0x04;
        constexpr std::size_t kListEntryRecord = 0x10;
        constexpr std::size_t kListEntryMinimum = 0x1A;

        constexpr std::wstring_view kWofStream = L"WofCompressedData";

        struct Attribute
        {
            std::uint32_t type = 0;
            bool nonResident = false;
            std::uint16_t flags = 0;
            std::span<std::byte const> name;   // UTF-16, unaligned
            std::span<std::byte const> value;  // resident
            std::uint64_t lowestVcn = 0;       // non-resident
            std::uint64_t allocated = 0;       // non-resident, first segment: bytes on disk
            std::uint64_t size = 0;            // non-resident, first segment: length
            std::span<std::byte const> mappingPairs;
        };

        bool NameIs(std::span<std::byte const> name, std::wstring_view expected) noexcept
        {
            if (name.size() != expected.size() * sizeof(std::uint16_t))  // UTF-16 on the disk
            {
                return false;
            }
            for (std::size_t i = 0; i < expected.size(); ++i)
            {
                if (std::cmp_not_equal(At<std::uint16_t>(name, i * 2), static_cast<std::uint32_t>(expected[i])))
                {
                    return false;
                }
            }
            return true;
        }

        // Calls `visit` for each attribute of a record whose fixups are undone.
        // False if the attribute chain is malformed.
        template <typename Visit>
        bool ForEachAttribute(std::span<std::byte const> record, Visit const& visit)
        {
            std::size_t const end = std::min<std::size_t>(At<std::uint32_t>(record, kBytesInUse), record.size());
            std::size_t offset = At<std::uint16_t>(record, kFirstAttributeOffset);
            while (offset < end && end - offset >= 2 * sizeof(std::uint32_t))
            {
                auto const type = At<std::uint32_t>(record, offset);
                if (type == kEndOfAttributes)
                {
                    return true;
                }
                auto const length = At<std::uint32_t>(record, offset + kAttributeLength);
                if (length < 0x18 || length > end - offset)
                {
                    return false;
                }
                std::span<std::byte const> const header = record.subspan(offset, length);
                offset += length;

                Attribute attribute;
                attribute.type = type;
                attribute.nonResident = At<std::uint8_t>(header, kNonResident) != 0;
                attribute.flags = At<std::uint16_t>(header, kAttributeFlags);
                std::optional<std::span<std::byte const>> const name =
                    Slice(header, At<std::uint16_t>(header, kNameOffset),
                          At<std::uint8_t>(header, kNameLength) * std::size_t{ 2 });
                if (!name)
                {
                    return false;
                }
                attribute.name = *name;
                if (!attribute.nonResident)
                {
                    std::optional<std::span<std::byte const>> const value =
                        Slice(header, At<std::uint16_t>(header, kValueOffset), At<std::uint32_t>(header, kValueLength));
                    if (!value)
                    {
                        return false;
                    }
                    attribute.value = *value;
                }
                else
                {
                    if (length < kTotalAllocated)
                    {
                        return false;
                    }
                    attribute.lowestVcn = At<std::uint64_t>(header, kLowestVcn);
                    attribute.size = At<std::uint64_t>(header, kFileSize);
                    std::size_t const pairs = At<std::uint16_t>(header, kMappingPairsOffset);
                    // Compressed and sparse attributes say how much is really allocated.
                    bool const counted = (attribute.flags & (kCompressedMask | kSparse)) != 0 &&
                                         pairs >= kTotalAllocated + sizeof(std::uint64_t);
                    attribute.allocated = At<std::uint64_t>(header, counted ? kTotalAllocated : kAllocatedLength);
                    if (pairs > length)
                    {
                        return false;
                    }
                    attribute.mappingPairs = header.subspan(pairs);
                }
                visit(attribute);
            }
            return true;
        }

        // The records an attribute list points to, other than `self`.
        void ParseAttributeList(std::span<std::byte const> list, std::uint64_t self, std::vector<std::uint64_t>& out)
        {
            std::size_t offset = 0;
            while (list.size() - offset >= kListEntryMinimum)
            {
                std::size_t const length = At<std::uint16_t>(list, offset + kListEntryLength);
                if (length < kListEntryMinimum || length > list.size() - offset)
                {
                    return;
                }
                std::uint64_t const record = RecordOf(At<std::uint64_t>(list, offset + kListEntryRecord));
                if (record != self && std::ranges::find(out, record) == out.end())
                {
                    out.push_back(record);
                }
                offset += length;
            }
        }

#ifdef _WIN32
        // Page-aligned memory: direct reads from a volume need aligned buffers.
        class AlignedBuffer
        {
        public:
            explicit AlignedBuffer(std::size_t size)
                : memory_(
                      static_cast<std::byte*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))),
                  size_(size)
            {
                if (!memory_)
                {
                    throw std::bad_alloc();
                }
            }

            std::span<std::byte> Span() const noexcept { return { memory_.get(), size_ }; }

        private:
            struct Free
            {
                void operator()(std::byte* memory) const noexcept { VirtualFree(memory, 0, MEM_RELEASE); }
            };
            std::unique_ptr<std::byte, Free> memory_;
            std::size_t size_;
        };

        // The time now, as a FILETIME.
        std::uint64_t Now() noexcept
        {
            FILETIME now{};
            GetSystemTimeAsFileTime(&now);
            return (std::uint64_t{ now.dwHighDateTime } << 32) | now.dwLowDateTime;
        }
#endif
    }

    void FileRecord::Clear() noexcept
    {
        number = 0;
        base = 0;
        inUse = false;
        directory = false;
        hasAttributeList = false;
        hasStandardInformation = false;
        hasData = false;
        attributes = 0;
        reparseTag = 0;
        lastWriteTime = 0;
        allocated = 0;
        length = 0;
        wofAllocated = 0;
        names.clear();
        nameText.clear();
        extensions.clear();
        listLength = 0;
        listRuns.clear();
    }

    void FileRecord::Merge(FileRecord const& extension)
    {
        for (FileName const& name : extension.names)
        {
            FileName copy = name;
            copy.offset = static_cast<std::uint32_t>(nameText.size());
            nameText.append(extension.Name(name));
            names.push_back(copy);
        }
        if (!hasData && extension.hasData)
        {
            hasData = true;
            allocated = extension.allocated;
            length = extension.length;
        }
        if (wofAllocated == 0)
        {
            wofAllocated = extension.wofAllocated;
        }
        if (reparseTag == 0)
        {
            reparseTag = extension.reparseTag;
        }
        if (!hasStandardInformation && extension.hasStandardInformation)
        {
            hasStandardInformation = true;
            attributes = extension.attributes;
            lastWriteTime = extension.lastWriteTime;
        }
    }

    std::uint64_t FileRecord::SizeOnDisk() const noexcept
    {
        // A file compressed by Windows (compact.exe, CompactOS) keeps its data
        // in a stream of its own; the unnamed stream holds no clusters.
        return reparseTag == IO_REPARSE_TAG_WOF ? allocated + wofAllocated : allocated;
    }

    std::uint32_t FileRecord::ListedAttributes() const noexcept
    {
        // As a folder listing shows them: NTFS's mark for files with extended
        // attributes left out, and "normal" for a file with none.  Windows'
        // file compression hides that its files are sparse reparse points.
        constexpr std::uint32_t kHasExtendedAttributes = 0x00040000;
        std::uint32_t listed = attributes & ~kHasExtendedAttributes;
        if (reparseTag == IO_REPARSE_TAG_WOF)
        {
            listed &= ~static_cast<std::uint32_t>(FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_SPARSE_FILE);
        }
        if (directory)
        {
            listed |= FILE_ATTRIBUTE_DIRECTORY;
        }
        return listed != 0 ? listed : FILE_ATTRIBUTE_NORMAL;
    }

    bool FileRecord::IsAlias() const noexcept
    {
        return sm::IsAlias(attributes, reparseTag);
    }

    bool ApplyFixups(std::span<std::byte> record) noexcept
    {
        std::span<std::byte const> const view(record);
        if (At<std::uint32_t>(view, 0) != kFileSignature)
        {
            return false;
        }
        std::size_t const offset = At<std::uint16_t>(view, kUpdateSequenceOffset);
        std::size_t const count = At<std::uint16_t>(view, kUpdateSequenceCount);
        if (count < 2 || (count - 1) * kFixupStride > record.size() || offset + count * 2 > record.size())
        {
            return false;
        }
        auto const check = At<std::uint16_t>(view, offset);
        for (std::size_t i = 1; i < count; ++i)
        {
            std::size_t const end = i * kFixupStride - sizeof(std::uint16_t);
            auto const original = At<std::uint16_t>(view, offset + i * 2);
            auto const stored = At<std::uint16_t>(view, end);
            if (stored != check)
            {
                return false;  // torn: part of the record is from another write
            }
            std::memcpy(record.data() + end, &original, sizeof(original));
        }
        return true;
    }

    bool DecodeRuns(std::span<std::byte const> mappingPairs, std::uint64_t lowestVcn, std::vector<Extent>& out)
    {
        std::uint64_t vcn = lowestVcn;
        std::uint64_t lcn = 0;
        std::size_t offset = 0;
        auto const number = [&mappingPairs, &offset](std::size_t size) {
            std::uint64_t value = 0;
            for (std::size_t i = 0; i < size; ++i)
            {
                value |= std::uint64_t{ static_cast<std::uint8_t>(mappingPairs[offset + i]) } << (8 * i);
            }
            offset += size;
            return value;
        };
        while (offset < mappingPairs.size())
        {
            auto const header = static_cast<std::uint8_t>(mappingPairs[offset++]);
            if (header == 0)
            {
                return true;
            }
            std::size_t const lengthSize = header & 0x0Fu;
            std::size_t const deltaSize = header >> 4u;
            if (lengthSize == 0 || lengthSize > 8 || deltaSize > 8 ||
                mappingPairs.size() - offset < lengthSize + deltaSize)
            {
                return false;
            }
            std::uint64_t const clusters = number(lengthSize);
            if (deltaSize == 0)
            {
                out.push_back({ vcn, clusters, -1 });  // sparse: no clusters on the disk
            }
            else
            {
                bool const negative = (static_cast<std::uint8_t>(mappingPairs[offset + deltaSize - 1]) & 0x80u) != 0;
                std::uint64_t delta = number(deltaSize);
                if (negative && deltaSize < 8)
                {
                    delta |= ~std::uint64_t{ 0 } << (8 * deltaSize);  // sign-extend
                }
                lcn += delta;  // modulo 2^64, so a negative delta subtracts
                if (lcn > static_cast<std::uint64_t>(INT64_MAX))
                {
                    return false;
                }
                out.push_back({ vcn, clusters, static_cast<std::int64_t>(lcn) });
            }
            vcn += clusters;
        }
        return true;
    }

    bool ParseRecord(std::span<std::byte const> record, std::uint64_t number, FileRecord& out)
    {
        if (At<std::uint32_t>(record, 0) != kFileSignature)
        {
            return false;
        }
        auto const flags = At<std::uint16_t>(record, kRecordFlags);
        out.number = number;
        out.base = RecordOf(At<std::uint64_t>(record, kBaseRecord));
        out.inUse = (flags & kRecordInUse) != 0;
        out.directory = (flags & kRecordIsDirectory) != 0;
        if (!out.inUse)
        {
            return true;
        }

        std::uint32_t nameReparseTag = 0;
        bool const wellFormed = ForEachAttribute(record, [&](Attribute const& attribute) {
            switch (attribute.type)
            {
            case kStandardInformation:
                if (!attribute.nonResident && attribute.value.size() >= kStandardAttributes + sizeof(std::uint32_t))
                {
                    out.hasStandardInformation = true;
                    out.lastWriteTime = At<std::uint64_t>(attribute.value, kStandardLastWrite);
                    out.attributes = At<std::uint32_t>(attribute.value, kStandardAttributes);
                }
                break;
            case kAttributeList:
                out.hasAttributeList = true;
                if (!attribute.nonResident)
                {
                    ParseAttributeList(attribute.value, number, out.extensions);
                }
                else
                {
                    out.listLength = attribute.size;
                    if (!DecodeRuns(attribute.mappingPairs, attribute.lowestVcn, out.listRuns))
                    {
                        out.listRuns.clear();
                    }
                }
                break;
            case kFileNameAttribute:
            {
                std::span<std::byte const> const value = attribute.value;
                std::size_t const characters = At<std::uint8_t>(value, kNameCharacters);
                if (attribute.nonResident || At<std::uint8_t>(value, kNameSpace) == kDosNameSpace ||
                    value.size() < kNameText + characters * 2)
                {
                    break;
                }
                if ((At<std::uint32_t>(value, kNameAttributes) & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                {
                    nameReparseTag = At<std::uint32_t>(value, kNameReparseTag);
                }
                FileName name;
                name.parent = RecordOf(At<std::uint64_t>(value, kNameParent));
                name.offset = static_cast<std::uint32_t>(out.nameText.size());
#ifdef _WIN32
                name.length = static_cast<std::uint16_t>(characters);
                // The whole name at once: its length was checked against the value above.
                static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));
                out.nameText.resize(out.nameText.size() + characters);
                std::memcpy(out.nameText.data() + name.offset, value.data() + kNameText, characters * sizeof(wchar_t));
#else
                {
                    // UTF-16 on the disk; wide strings hold whole code points here.
                    std::u16string units(characters, u'\0');
                    std::memcpy(units.data(), value.data() + kNameText, characters * sizeof(char16_t));
                    std::wstring const text = posix::FromUtf16(units);
                    name.length = static_cast<std::uint16_t>(text.size());
                    out.nameText.append(text);
                }
#endif
                out.names.push_back(name);
                break;
            }
            case kData:
                if (attribute.name.empty())
                {
                    if (!attribute.nonResident)
                    {
                        // A small file lives inside its record.  Folder listings
                        // count its length rounded up to 8 bytes, and so does this.
                        out.hasData = true;
                        out.length = attribute.value.size();
                        out.allocated = (attribute.value.size() + 7) & ~std::size_t{ 7 };
                    }
                    else if (attribute.lowestVcn == 0)
                    {
                        out.hasData = true;
                        out.length = attribute.size;
                        out.allocated = attribute.allocated;
                    }
                }
                else if (NameIs(attribute.name, kWofStream) && attribute.nonResident && attribute.lowestVcn == 0)
                {
                    out.wofAllocated = attribute.allocated;
                }
                break;
            case kReparsePoint:
                if (!attribute.nonResident)
                {
                    out.reparseTag = At<std::uint32_t>(attribute.value, 0);
                }
                break;
            default: break;
            }
        });
        if (out.reparseTag == 0)
        {
            out.reparseTag = nameReparseTag;  // reparse data too big to sit in the record: its name has the tag too
        }
        return wellFormed;
    }

#ifdef _WIN32
    std::optional<Volume> Volume::Open(std::wstring const& root)
    {
        wchar_t name[64]{};
        if (!GetVolumeNameForVolumeMountPointW(root.c_str(), name, static_cast<DWORD>(std::size(name))))
        {
            return std::nullopt;
        }
        // "\\?\Volume{guid}\" names the root folder; without the final backslash, the volume itself.
        std::wstring device(name);
        if (!device.empty() && device.back() == L'\\')
        {
            device.pop_back();
        }
        return OpenDevice(device);
    }

    std::optional<Volume> Volume::OpenDevice(std::wstring const& device)
    {
        Volume volume;
        // Read-only: nothing here ever writes to the volume.  Unbuffered, so
        // reading gigabytes of MFT does not push everything else out of the
        // file cache.
        volume.handle_.reset(CreateFileW(device.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                         OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr));
        if (!volume.handle_)
        {
            return std::nullopt;  // most likely not running as administrator
        }
        NTFS_VOLUME_DATA_BUFFER data{};
        DWORD returned = 0;
        if (!DeviceIoControl(volume.handle_.get(), FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0, &data, sizeof(data),
                             &returned, nullptr))
        {
            return std::nullopt;  // not NTFS
        }
        // Unbuffered reads come in whole sectors: records must be whole sectors too.
        if (data.BytesPerCluster == 0 || data.BytesPerSector == 0 || data.BytesPerFileRecordSegment < kFixupStride ||
            data.BytesPerFileRecordSegment % kFixupStride != 0 ||
            data.BytesPerFileRecordSegment % data.BytesPerSector != 0 ||
            data.BytesPerCluster % data.BytesPerSector != 0 || data.MftValidDataLength.QuadPart <= 0 ||
            data.MftStartLcn.QuadPart < 0)
        {
            return std::nullopt;
        }
        volume.device_ = device;
        volume.bytesPerCluster_ = data.BytesPerCluster;
        volume.recordSize_ = data.BytesPerFileRecordSegment;
        volume.mftStartLcn_ = static_cast<std::uint64_t>(data.MftStartLcn.QuadPart);
        volume.recordCount_ = static_cast<std::uint64_t>(data.MftValidDataLength.QuadPart) / volume.recordSize_;
        if (!volume.LoadMftLayout())
        {
            return std::nullopt;
        }
        return volume;
    }

    bool Volume::ReadClusters(std::uint64_t lcn, std::span<std::byte> buffer) const
    {
        std::uint64_t const offset = lcn * bytesPerCluster_;
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset);
        position.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD read = 0;
        return buffer.size() <= MAXDWORD &&
               ::ReadFile(handle_.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, &position) &&
               read == buffer.size();
    }

    // An attribute list too long to sit in its record (a file in thousands of
    // pieces, or with dozens of hard links), read from the disk.  The disk can
    // lag behind the file system by a few seconds; an older copy of the list
    // still names the records holding the file's names and sizes (the records
    // themselves are read up to date, and checked to still belong to the file),
    // but a list just moved may not be on the disk at all.
    bool Volume::ReadAttributeList(FileRecord& file) const
    {
        constexpr std::uint64_t kMaxListBytes = std::uint64_t{ 1 } << 20;  // NTFS allows 256 KB
        if (file.listLength == 0 || file.listLength > kMaxListBytes || file.listRuns.empty())
        {
            return false;
        }
        std::uint64_t const clusters = (file.listLength + bytesPerCluster_ - 1) / bytesPerCluster_;
        AlignedBuffer const buffer(static_cast<std::size_t>(clusters * bytesPerCluster_));
        std::uint64_t done = 0;
        for (Extent const& extent : file.listRuns)
        {
            if (extent.vcn != done)
            {
                return false;
            }
            std::uint64_t const take = std::min(extent.clusters, clusters - done);
            std::span<std::byte> const target = buffer.Span().subspan(
                static_cast<std::size_t>(done * bytesPerCluster_), static_cast<std::size_t>(take * bytesPerCluster_));
            if (extent.lcn < 0)
            {
                std::ranges::fill(target, std::byte{ 0 });
            }
            else if (!ReadClusters(static_cast<std::uint64_t>(extent.lcn), target))
            {
                return false;
            }
            done += take;
            if (done == clusters)
            {
                break;
            }
        }
        if (done != clusters)
        {
            return false;
        }
        // A list starts with the file's standard information, in its own record.
        std::span<std::byte const> const list = buffer.Span().first(static_cast<std::size_t>(file.listLength));
        if (At<std::uint32_t>(list, 0) != kStandardInformation ||
            RecordOf(At<std::uint64_t>(list, kListEntryRecord)) != file.number)
        {
            return false;
        }
        ParseAttributeList(list, file.number, file.extensions);
        return true;
    }

    // Where the MFT is: the run list of its own record, record 0, which sits
    // at the start of the MFT - and, when the MFT is very fragmented, of the
    // extension records its attribute list points to.
    bool Volume::LoadMftLayout()
    {
        std::size_t const clusters = (recordSize_ + bytesPerCluster_ - 1) / bytesPerCluster_;
        AlignedBuffer const first(clusters * bytesPerCluster_);
        if (!ReadClusters(mftStartLcn_, first.Span()))
        {
            return false;
        }
        std::span<std::byte> const record = first.Span().first(recordSize_);
        if (!ApplyFixups(record))
        {
            return false;
        }

        std::vector<std::uint64_t> extensions;
        bool listOnDisk = false;
        auto const collect = [this, &extensions, &listOnDisk](Attribute const& attribute) {
            if (attribute.type == kData && attribute.name.empty() && attribute.nonResident)
            {
                (void)DecodeRuns(attribute.mappingPairs, attribute.lowestVcn, mft_);
            }
            else if (attribute.type == kAttributeList)
            {
                if (attribute.nonResident)
                {
                    listOnDisk = true;
                }
                else
                {
                    ParseAttributeList(attribute.value, 0, extensions);
                }
            }
        };
        mft_.clear();
        if (!ForEachAttribute(record, collect) || listOnDisk || mft_.empty())
        {
            return false;
        }
        std::ranges::sort(mft_, {}, &Extent::vcn);

        AlignedBuffer const extension(recordSize_);
        for (std::uint64_t const number : extensions)
        {
            if (number >= recordCount_ || !ReadRecords(number, 1, extension.Span()) || !ApplyFixups(extension.Span()) ||
                !ForEachAttribute(extension.Span(), collect))
            {
                return false;
            }
            std::ranges::sort(mft_, {}, &Extent::vcn);
        }

        // Only as much of the MFT as the runs cover can be read.
        std::uint64_t covered = 0;
        for (Extent const& extent : mft_)
        {
            if (extent.vcn != covered)
            {
                break;
            }
            covered += extent.clusters;
        }
        recordCount_ = std::min(recordCount_, covered * bytesPerCluster_ / recordSize_);
        return recordCount_ > kFirstUserRecord;
    }

    bool Volume::ReadRecords(std::uint64_t first, std::size_t count, std::span<std::byte> buffer) const
    {
        if (count > recordCount_ || first > recordCount_ - count || buffer.size() < count * recordSize_)
        {
            return false;
        }
        // The MFT as a stream of bytes, mapped onto its runs on the volume.
        std::uint64_t position = first * recordSize_;
        std::size_t done = 0;
        std::size_t const total = count * recordSize_;
        auto extent = mft_.begin();
        while (done < total)
        {
            std::uint64_t const vcn = position / bytesPerCluster_;
            while (extent != mft_.end() && vcn >= extent->vcn + extent->clusters)
            {
                ++extent;
            }
            if (extent == mft_.end() || vcn < extent->vcn)
            {
                return false;
            }
            std::uint64_t const within = position - extent->vcn * bytesPerCluster_;
            std::uint64_t const available = extent->clusters * bytesPerCluster_ - within;
            std::size_t const piece = static_cast<std::size_t>(std::min<std::uint64_t>(available, total - done));
            std::span<std::byte> const target = buffer.subspan(done, piece);
            if (extent->lcn < 0)
            {
                std::ranges::fill(target, std::byte{ 0 });
            }
            else
            {
                std::uint64_t const offset = static_cast<std::uint64_t>(extent->lcn) * bytesPerCluster_ + within;
                OVERLAPPED at{};
                at.Offset = static_cast<DWORD>(offset);
                at.OffsetHigh = static_cast<DWORD>(offset >> 32);
                DWORD read = 0;
                if (piece > MAXDWORD ||
                    !::ReadFile(handle_.get(), target.data(), static_cast<DWORD>(piece), &read, &at) || read != piece)
                {
                    return false;
                }
            }
            done += piece;
            position += piece;
        }
        return true;
    }

    Volume::ReadStatus Volume::ReadAll(std::stop_token const& stop,
                                       std::function<void(std::uint64_t, std::span<std::byte>)> const& consume) const
    {
        // Larger blocks or more reads at once do not read faster: on a fast
        // SSD six 4 MB reads already keep it at full speed.
        constexpr std::size_t kBlockBytes = std::size_t{ 4 } << 20;
        constexpr std::size_t kInFlight = 6;
        std::size_t const perBlock = std::max<std::size_t>(1, kBlockBytes / recordSize_);

        // Blocks of whole records, each within one run of the MFT.
        struct Block
        {
            std::uint64_t first = 0;
            std::size_t count = 0;
            std::int64_t offset = -1;  // on the volume; -1 for a sparse run
        };
        std::vector<Block> blocks;
        bool aligned = true;
        for (Extent const& extent : mft_)
        {
            std::uint64_t const begin = extent.vcn * bytesPerCluster_;
            std::uint64_t const end = (extent.vcn + extent.clusters) * bytesPerCluster_;
            aligned = aligned && begin % recordSize_ == 0 && end % recordSize_ == 0;
            for (std::uint64_t record = begin / recordSize_; record < std::min(end / recordSize_, recordCount_);)
            {
                auto const count = static_cast<std::size_t>(
                    std::min<std::uint64_t>(perBlock, std::min(end / recordSize_, recordCount_) - record));
                std::int64_t const offset =
                    extent.lcn < 0
                        ? -1
                        : static_cast<std::int64_t>(static_cast<std::uint64_t>(extent.lcn) * bytesPerCluster_ +
                                                    record * recordSize_ - begin);
                blocks.push_back({ record, count, offset });
                record += count;
            }
        }

        if (!aligned)
        {
            // Records straddle runs (tiny clusters): read them one stretch at a time instead.
            AlignedBuffer const buffer(perBlock * recordSize_);
            for (std::uint64_t first = 0; first < recordCount_; first += perBlock)
            {
                if (stop.stop_requested())
                {
                    return ReadStatus::Cancelled;
                }
                auto const count = static_cast<std::size_t>(std::min<std::uint64_t>(perBlock, recordCount_ - first));
                std::span<std::byte> const records = buffer.Span().first(count * recordSize_);
                if (!ReadRecords(first, count, records))
                {
                    return ReadStatus::Failed;
                }
                consume(first, records);
            }
            return ReadStatus::Done;
        }

        wil::unique_hfile const file(CreateFileW(device_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                 nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING,
                                                 nullptr));
        if (!file)
        {
            return ReadStatus::Failed;
        }
        struct Slot
        {
            explicit Slot(std::size_t bytes) : buffer(bytes), done(wil::EventOptions::ManualReset) {}
            AlignedBuffer buffer;
            OVERLAPPED overlapped{};
            wil::unique_event done;
            bool pending = false;
        };
        std::vector<std::unique_ptr<Slot>> slots;  // block k is read into slot k % slots.size()
        // Whatever happens, no read may still be writing into a buffer once it is freed.
        auto const settle = wil::scope_exit([&slots, &file]() noexcept {
            for (std::unique_ptr<Slot> const& slot : slots)
            {
                if (slot->pending)
                {
                    DWORD bytes = 0;
                    (void)CancelIoEx(file.get(), &slot->overlapped);
                    (void)GetOverlappedResult(file.get(), &slot->overlapped, &bytes, TRUE);
                    slot->pending = false;
                }
            }
        });
        auto const issue = [this, &file](Slot& slot, Block const& block) {
            std::span<std::byte> const target = slot.buffer.Span().first(block.count * recordSize_);
            if (block.offset < 0)
            {
                std::ranges::fill(target, std::byte{ 0 });  // no records there
                return true;
            }
            slot.overlapped = {};
            slot.overlapped.Offset = static_cast<DWORD>(block.offset);
            slot.overlapped.OffsetHigh = static_cast<DWORD>(static_cast<std::uint64_t>(block.offset) >> 32);
            slot.overlapped.hEvent = slot.done.get();
            if (!::ReadFile(file.get(), target.data(), static_cast<DWORD>(target.size()), nullptr, &slot.overlapped) &&
                GetLastError() != ERROR_IO_PENDING)
            {
                return false;
            }
            slot.pending = true;
            return true;
        };

        std::size_t next = 0;
        while (slots.size() < kInFlight && next < blocks.size())
        {
            slots.push_back(std::make_unique<Slot>(perBlock * recordSize_));
            if (!issue(*slots.back(), blocks[next++]))
            {
                return ReadStatus::Failed;
            }
        }
        for (std::size_t done = 0; done < blocks.size(); ++done)
        {
            if (stop.stop_requested())
            {
                return ReadStatus::Cancelled;
            }
            Slot& slot = *slots[done % slots.size()];
            Block const& block = blocks[done];
            std::span<std::byte> const records = slot.buffer.Span().first(block.count * recordSize_);
            if (slot.pending)
            {
                DWORD bytes = 0;
                BOOL const read = GetOverlappedResult(file.get(), &slot.overlapped, &bytes, TRUE);
                slot.pending = false;
                if (!read || bytes != records.size())
                {
                    return ReadStatus::Failed;
                }
            }
            consume(block.first, records);
            if (next < blocks.size())
            {
                if (!issue(slot, blocks[next]))
                {
                    return ReadStatus::Failed;
                }
                ++next;
            }
        }
        return ReadStatus::Done;
    }

    Volume::RecordRead Volume::ReadCurrentRecord(std::uint64_t number, std::vector<std::byte>& record) const
    {
        NTFS_FILE_RECORD_INPUT_BUFFER input{};
        input.FileReferenceNumber.QuadPart = static_cast<LONGLONG>(number);
        std::size_t const header = offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordBuffer);
        std::vector<std::byte> output(header + recordSize_);
        DWORD returned = 0;
        if (!DeviceIoControl(handle_.get(), FSCTL_GET_NTFS_FILE_RECORD, &input, sizeof(input), output.data(),
                             static_cast<DWORD>(output.size()), &returned, nullptr) ||
            returned < header)
        {
            return RecordRead::Failed;
        }
        // The file system answers with the nearest record in use at or below the one asked for.
        std::span<std::byte const> const view(output);
        if (RecordOf(At<std::uint64_t>(view, offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileReferenceNumber))) != number)
        {
            return RecordRead::NotInUse;
        }
        std::size_t const length = std::min<std::size_t>(
            At<std::uint32_t>(view, offsetof(NTFS_FILE_RECORD_OUTPUT_BUFFER, FileRecordLength)), returned - header);
        record.assign(output.begin() + static_cast<std::ptrdiff_t>(header),
                      output.begin() + static_cast<std::ptrdiff_t>(header + length));
        // This is NTFS's working copy, with its fixups undone already (and the
        // check bytes possibly stale: they are only renewed on the way to the disk).
        return At<std::uint32_t>(std::span<std::byte const>(record), 0) == kFileSignature ? RecordRead::Ok
                                                                                          : RecordRead::Failed;
    }

    bool Volume::ReadFile(std::uint64_t number, FileRecord& out) const
    {
        out.Clear();
        out.number = number;
        std::vector<std::byte> bytes;
        switch (ReadCurrentRecord(number, bytes))
        {
        case RecordRead::NotInUse: return true;
        case RecordRead::Failed: return false;
        case RecordRead::Ok: break;
        }
        if (!ParseRecord(bytes, number, out))
        {
            return false;
        }
        if (!out.inUse || out.base != 0 || !out.hasAttributeList)
        {
            return true;
        }
        if (out.listLength != 0 && !ReadAttributeList(out))
        {
            return false;
        }
        FileRecord extension;
        std::vector<std::uint64_t> const extensions = out.extensions;
        for (std::uint64_t const record : extensions)
        {
            extension.Clear();
            RecordRead const read = ReadCurrentRecord(record, bytes);
            if (read == RecordRead::Failed || (read == RecordRead::Ok && !ParseRecord(bytes, record, extension)))
            {
                return false;
            }
            if (read == RecordRead::Ok && extension.inUse && extension.base == number)
            {
                out.Merge(extension);
            }
        }
        // Its names and sizes, or the file cannot be shown.
        return !out.names.empty() && (out.directory || out.hasData);
    }
#endif

    TreeBuilder::TreeBuilder(std::uint64_t recordCount, ScanProgress* progress)
        : progress_(progress), folders_(static_cast<std::size_t>(recordCount))
    {
    }

    TreeBuilder::~TreeBuilder() = default;

    Folder& TreeBuilder::FolderOf(std::uint64_t record)
    {
        std::unique_ptr<Folder>& folder = folders_[static_cast<std::size_t>(record)];
        if (!folder)
        {
            folder = std::make_unique<Folder>();
        }
        return *folder;
    }

    void TreeBuilder::Add(FileRecord const& record)
    {
        if (!record.inUse)
        {
            return;
        }
        if (record.base != 0)
        {
            // An extension record: it belongs to its base record, which may come before or after it.
            deferred_[record.base].Merge(record);
            return;
        }
        if (record.hasAttributeList)
        {
            FileRecord& merged = deferred_[record.number];
            FileRecord whole = record;
            whole.Merge(merged);  // whatever extension records came first
            merged = std::move(whole);
            return;
        }
        Emit(record);
    }

    void TreeBuilder::Emit(FileRecord const& record)
    {
        if (record.number >= folders_.size() || (record.number < kFirstUserRecord && record.number != kRootRecord))
        {
            return;  // the file system's own files ($MFT, $LogFile, ...) stay in "unscanned"
        }
        if (record.directory)
        {
            (void)FolderOf(record.number);
            if (record.number == kRootRecord || record.IsAlias())
            {
                return;
            }
            for (FileName const& name : record.names)
            {
                if (name.parent != record.number)
                {
                    std::wstring_view const text = record.Name(name);
                    Directory directory;
                    directory.record = record.number;
                    directory.parent = name.parent;
                    directory.lastWriteTime = record.lastWriteTime;
                    directory.attributes = record.ListedAttributes();
                    directory.nameOffset = static_cast<std::uint32_t>(directoryNames_.size());
                    directory.nameLength = static_cast<std::uint16_t>(text.size());
                    directoryNames_.append(text);
                    directories_.push_back(directory);
                    if (progress_ != nullptr)
                    {
                        progress_->folders.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;  // a folder has a single name
                }
            }
            return;
        }
        if (record.IsAlias())
        {
            return;
        }

        std::uint64_t const size = record.SizeOnDisk();
        std::optional<std::uint32_t> const file = Record32(record.number);
        std::size_t const firstLink = links_.size();
        std::uint64_t links = 0;
        for (FileName const& name : record.names)
        {
            if (name.parent >= folders_.size())
            {
                continue;
            }
            Folder& folder = FolderOf(name.parent);
            std::wstring_view const text = record.Name(name);
            Entry entry;
            entry.kind = EntryKind::File;
            entry.size = size;
            entry.logicalSize = record.length;
            entry.attributes = record.ListedAttributes();
            entry.lastWriteTime = record.lastWriteTime;
            entry.record = file.value_or(0);
            entry.nameOffset = static_cast<std::uint32_t>(folder.names.size());
            entry.nameLength = static_cast<std::uint16_t>(text.size());
            folder.names.append(text);
            folder.entries.push_back(std::move(entry));
            if (std::optional<std::uint32_t> const parent = Record32(name.parent); file && parent)
            {
                links_.push_back({ *file, *parent, static_cast<std::uint32_t>(folder.entries.size() - 1) });
            }
            ++links;
        }
        if (links_.size() - firstLink < 2)
        {
            links_.resize(firstLink);  // a single name: nothing to count once
        }
        if (progress_ != nullptr && links != 0)
        {
            progress_->files.fetch_add(links, std::memory_order_relaxed);
            progress_->bytes.fetch_add(size * links, std::memory_order_relaxed);
        }
    }

    // Each file listed under several names in the tree is counted under one
    // of them, as CountHardLinksOnce chooses it: the one nearest the root,
    // then in the folder with the lowest record, then the lowest.  Its other
    // names take no space.  Runs before the folders are sorted: links_ still
    // says where each name's entry is.
    void TreeBuilder::CountLinksOnce(std::span<std::uint32_t const> depths)
    {
        std::ranges::sort(links_, {}, &Link::file);
        for (std::size_t first = 0; first < links_.size();)
        {
            std::size_t last = first + 1;
            while (last < links_.size() && links_[last].file == links_[first].file)
            {
                ++last;
            }
            std::span<Link const> const names(links_.data() + first, last - first);
            first = last;

            // Its names in the tree (those in folders left out of it do not count).
            Link const* counted = nullptr;
            std::size_t inTree = 0;
            auto const nameOf = [this](Link const& link) { return index_.folders[link.folder]->Name(link.index); };
            for (Link const& link : names)
            {
                if (index_.folders[link.folder] == nullptr)
                {
                    continue;
                }
                ++inTree;
                if (counted == nullptr || depths[link.folder] < depths[counted->folder] ||
                    (depths[link.folder] == depths[counted->folder] &&
                     (link.folder < counted->folder ||
                      (link.folder == counted->folder && nameOf(link) < nameOf(*counted)))))
                {
                    counted = &link;
                }
            }
            if (inTree < 2)
            {
                continue;
            }
            hardLinks_ = true;
            for (Link const& link : names)
            {
                if (&link != counted && index_.folders[link.folder] != nullptr)
                {
                    Entry& entry = index_.folders[link.folder]->entries[link.index];
                    entry.size = 0;
                    entry.countedElsewhere = true;
                }
            }
        }
    }

    std::unique_ptr<Folder> TreeBuilder::Finish(std::uint64_t rootRecord)
    {
        // Files with extension records, in record order so the tree is the same every time.
        std::vector<std::uint64_t> pending;
        pending.reserve(deferred_.size());
        for (auto const& [number, record] : deferred_)
        {
            if (record.inUse)
            {
                pending.push_back(number);
            }
        }
        std::ranges::sort(pending);
        for (std::uint64_t const number : pending)
        {
            Emit(deferred_[number]);
        }
        deferred_ = std::unordered_map<std::uint64_t, FileRecord>();

        std::size_t const records = folders_.size();
        if (rootRecord >= records || !folders_[static_cast<std::size_t>(rootRecord)])
        {
            return nullptr;
        }

        // Each folder's sub-folders: the directories bucketed by parent (a
        // counting sort, as record numbers are dense), in record order.
        std::vector<std::uint32_t> firstChild(records + 1, 0);  // bucket p: children[firstChild[p], firstChild[p + 1])
        for (Directory const& directory : directories_)
        {
            if (directory.parent < records)
            {
                ++firstChild[static_cast<std::size_t>(directory.parent) + 1];
            }
        }
        std::partial_sum(firstChild.begin(), firstChild.end(), firstChild.begin());
        std::vector<std::uint32_t> children(firstChild.back());
        {
            std::vector<std::uint32_t> next(firstChild.begin(), firstChild.end() - 1);
            for (std::size_t i = 0; i < directories_.size(); ++i)
            {
                if (std::uint64_t const parent = directories_[i].parent; parent < records)
                {
                    children[next[static_cast<std::size_t>(parent)]++] = static_cast<std::uint32_t>(i);
                }
            }
        }
        auto const childrenOf = [&](std::size_t parent) {
            return std::span<std::uint32_t const>(children.data() + firstChild[parent],
                                                  firstChild[parent + 1] - firstChild[parent]);
        };

        // Which folders the root leads to, level by level.  A folder it does
        // not lead to (the file system's own, or one left over by a damaged
        // record) is left out with everything in it.
        constexpr std::uint32_t kUnreached = UINT32_MAX;
        std::vector<std::uint32_t> depths(records, kUnreached);
        std::vector<std::uint32_t> order{ static_cast<std::uint32_t>(rootRecord) };
        depths[static_cast<std::size_t>(rootRecord)] = 0;
        for (std::size_t i = 0; i < order.size(); ++i)
        {
            std::uint32_t const parent = order[i];
            for (std::uint32_t const child : childrenOf(parent))
            {
                Directory& directory = directories_[child];
                auto const record = static_cast<std::size_t>(directory.record);
                if (record >= records || depths[record] != kUnreached || !folders_[record])
                {
                    continue;  // placed already: a damaged volume naming a folder twice
                }
                directory.placed = true;
                depths[record] = depths[parent] + 1;
                order.push_back(static_cast<std::uint32_t>(record));
            }
        }
        index_.folders.assign(records, nullptr);
        for (std::uint32_t const record : order)
        {
            index_.folders[record] = folders_[record].get();
        }
        std::unique_ptr<Folder> root = std::move(folders_[static_cast<std::size_t>(rootRecord)]);

        // Hang each folder's sub-folders in it, its entries and names
        // reallocated once at their final size - on every processor, as each
        // folder is changed by its own task only.
        constexpr std::size_t kFoldersPerTask = 256;
        auto const chunks = [](std::size_t count) { return (count + kFoldersPerTask - 1) / kFoldersPerTask; };
        ParallelFor(chunks(order.size()), [&](std::size_t chunk) {
            std::size_t const end = std::min(order.size(), (chunk + 1) * kFoldersPerTask);
            for (std::size_t i = chunk * kFoldersPerTask; i < end; ++i)
            {
                Folder& folder = *index_.folders[order[i]];
                std::size_t entries = folder.entries.size();
                std::size_t letters = folder.names.size();
                for (std::uint32_t const child : childrenOf(order[i]))
                {
                    if (directories_[child].placed)
                    {
                        ++entries;
                        letters += directories_[child].nameLength;
                    }
                }
                // Vectors grow geometrically while records come in: give the slack back,
                // which matters when millions of entries are held.
                if (folder.entries.capacity() < entries || folder.entries.capacity() - entries > 8)
                {
                    std::vector<Entry> exact;
                    exact.reserve(entries);
                    std::ranges::move(folder.entries, std::back_inserter(exact));
                    folder.entries.swap(exact);
                }
                if (folder.names.capacity() < letters || folder.names.capacity() - letters > 64)
                {
                    std::wstring exact;
                    exact.reserve(letters);
                    exact.append(folder.names);
                    folder.names.swap(exact);
                }
                for (std::uint32_t const child : childrenOf(order[i]))
                {
                    Directory const& directory = directories_[child];
                    if (!directory.placed)
                    {
                        continue;
                    }
                    Entry entry;
                    entry.kind = EntryKind::Folder;
                    entry.attributes = directory.attributes;
                    entry.lastWriteTime = directory.lastWriteTime;
                    entry.record = Record32(directory.record).value_or(0);
                    entry.nameOffset = static_cast<std::uint32_t>(folder.names.size());
                    entry.nameLength = directory.nameLength;
                    folder.names.append(
                        std::wstring_view(directoryNames_).substr(directory.nameOffset, directory.nameLength));
                    entry.child = std::move(folders_[static_cast<std::size_t>(directory.record)]);
                    entry.child->parent = &folder;
                    folder.entries.push_back(std::move(entry));
                }
            }
        });
        if (progress_ != nullptr)
        {
            progress_->folders.store(order.size() - 1, std::memory_order_relaxed);
        }

        CountLinksOnce(depths);
        // Done with: their memory goes, and with folders_ what was left out of the tree.
        directories_ = std::vector<Directory>();
        directoryNames_ = std::wstring();
        links_ = std::vector<Link>();
        folders_ = std::vector<std::unique_ptr<Folder>>();

        // Totals, from the deepest folders up: a level at a time, each level
        // on every processor.  The large files are found on the way.
        std::size_t levelEnd = order.size();
        while (levelEnd > 0)
        {
            std::uint32_t const depth = depths[order[levelEnd - 1]];
            std::size_t levelBegin = levelEnd;
            while (levelBegin > 0 && depths[order[levelBegin - 1]] == depth)
            {
                --levelBegin;
            }
            std::span<std::uint32_t const> const level(order.data() + levelBegin, levelEnd - levelBegin);
            std::vector<std::vector<std::uint32_t>> large(chunks(level.size()));
            ParallelFor(large.size(), [&](std::size_t chunk) {
                std::size_t const end = std::min(level.size(), (chunk + 1) * kFoldersPerTask);
                for (std::size_t i = chunk * kFoldersPerTask; i < end; ++i)
                {
                    Folder& folder = *index_.folders[level[i]];
                    UpdateTotals(folder);
                    for (Entry const& entry : folder.entries)
                    {
                        if (entry.kind == EntryKind::File && entry.record != 0 &&
                            (entry.size >= kLargeFile || entry.logicalSize >= kLargeFile))
                        {
                            large[chunk].push_back(entry.record);
                        }
                    }
                }
            });
            for (std::vector<std::uint32_t> const& found : large)
            {
                index_.largeFiles.insert(index_.largeFiles.end(), found.begin(), found.end());
            }
            levelEnd = levelBegin;
        }
        std::ranges::sort(index_.largeFiles);
        index_.largeFiles.erase(std::ranges::unique(index_.largeFiles).begin(), index_.largeFiles.end());
        return root;
    }

#ifdef _WIN32
    std::optional<ScanResult> ScanVolume(std::wstring const& root, VolumeInfo volume, ScanProgress& progress,
                                         std::stop_token const& stop, bool addSpecialEntries)
    {
        std::optional<Volume> ntfs = Volume::Open(root);
        if (!ntfs)
        {
            return std::nullopt;
        }
        // Where the change journal is before the table is read: what changes
        // from here on is applied afterwards.
        std::optional<JournalPosition> const journal = CurrentJournalPosition(*ntfs);
        std::uint64_t const started = Now();
        progress.SetCurrentPath(root);

        // Each block of records is decoded on every processor as soon as it
        // is read, while the block before it goes into the tree: the builder
        // takes the records one at a time, in order, one block behind.
        struct Decoded
        {
            std::vector<FileRecord> records;  // kept from block to block, with their memory
            std::vector<std::uint8_t> valid;  // records[i] is a file record
            std::size_t count = 0;
        };
        // A quarter of a 4 MB block of 1 KB records each: four helpers decode
        // a block well before the next one arrives, and waking more of them
        // for every block would cost more processor time than they save.
        constexpr std::size_t kRecordsPerTask = 1024;
        std::uint32_t const recordSize = ntfs->RecordSize();
        TreeBuilder builder(ntfs->RecordCount(), &progress);
        std::array<Decoded, 2> decoded;
        std::size_t current = 0;  // decoded[current] takes the next block; the other one waits to be added
        auto const add = [&builder](Decoded const& block) {
            for (std::size_t i = 0; i < block.count; ++i)
            {
                if (block.valid[i] != 0)
                {
                    builder.Add(block.records[i]);
                }
            }
        };
        Volume::ReadStatus const status = ntfs->ReadAll(stop, [&](std::uint64_t first, std::span<std::byte> bytes) {
            Decoded& block = decoded[current];
            Decoded const& previous = decoded[1 - current];
            block.count = bytes.size() / recordSize;
            if (block.records.size() < block.count)
            {
                block.records.resize(block.count);
                block.valid.resize(block.count);
            }
            std::size_t const tasks = (block.count + kRecordsPerTask - 1) / kRecordsPerTask;
            // Task 0 adds the previous block; the others decode this one, in place.
            ParallelFor(tasks + 1, [&](std::size_t task) {
                if (task == 0)
                {
                    add(previous);
                    return;
                }
                std::size_t const end = std::min(block.count, task * kRecordsPerTask);
                for (std::size_t i = (task - 1) * kRecordsPerTask; i < end; ++i)
                {
                    std::span<std::byte> const record = bytes.subspan(i * recordSize, recordSize);
                    block.records[i].Clear();
                    block.valid[i] = ApplyFixups(record) && ParseRecord(record, first + i, block.records[i]) ? 1 : 0;
                }
            });
            decoded[1 - current].count = 0;  // added
            current = 1 - current;
        });
        if (status == Volume::ReadStatus::Cancelled)
        {
            return ScanResult{ ScanStatus::Cancelled, nullptr };
        }
        if (status == Volume::ReadStatus::Failed)
        {
            return std::nullopt;
        }
        add(decoded[1 - current]);  // the last block
        decoded = {};

        std::unique_ptr<Folder> folder = builder.Finish();
        if (!folder)
        {
            return std::nullopt;
        }
        auto tree = std::make_unique<FolderTree>(std::move(folder), root, std::move(volume), addSpecialEntries);
        tree->method = ScanMethod::MasterFileTable;
        tree->threads = ParallelThreads();
        tree->rootRecord = static_cast<std::uint32_t>(kRootRecord);
        tree->hardLinks = builder.HardLinks();

        if (journal)
        {
            // Kept for the reloads, which then need not walk the whole tree.
            tree->journalIndex = builder.TakeIndex();
            // The disk can still hold older copies of records changed in the
            // moments before the read (NTFS writes them back lazily) and
            // during it.  The journal says which files those are; they are
            // read again through the file system, which has them up to date.
            constexpr std::int64_t kLookbackBytes = std::int64_t{ 8 } << 20;
            constexpr std::uint64_t kLookbackTime = 60ull * 10'000'000;  // 60 s, in FILETIME units
            JournalRead const read = ReadJournalChanges(*journal, stop, kLookbackBytes, started - kLookbackTime);
            if (read.status == JournalReadStatus::Cancelled)
            {
                return ScanResult{ ScanStatus::Cancelled, nullptr };
            }
            if (read.status == JournalReadStatus::Ready)
            {
                ApplyJournalChanges(*tree, read.changes);
            }
        }
        return ScanResult{ ScanStatus::Completed, std::move(tree) };
    }
#endif
}

#pragma once

#include <Core/Defines.h>
#include <Core/Types.h>
#include <Common/CurrentMetrics.h>
#include <Common/Exception.h>

#include <memory>
#include <utility>
#include <boost/noncopyable.hpp>


namespace CurrentMetrics
{
extern const Metric DiskSpaceReservedForMerge;
}

namespace DB
{
class DiskDirectoryIterator;

class IReservation;
using ReservationPtr = std::unique_ptr<IReservation>;

class ReadBuffer;
class WriteBuffer;

/**
 * Provide interface for reservation.
 */
class Space : public std::enable_shared_from_this<Space>
{
public:
    /// Return the name of the space object.
    virtual const String & getName() const = 0;

    /// Reserve the specified number of bytes.
    virtual ReservationPtr reserve(UInt64 bytes) = 0;

    virtual ~Space() = default;
};

using SpacePtr = std::shared_ptr<Space>;

/**
 * A unit of storage persisting data and metadata.
 * Abstract underlying storage technology.
 * Responsible for:
 * - file management;
 * - space accounting and reservation.
 */
class IDisk : public Space
{
public:
    /// Root path for all files stored on the disk.
    /// It's not required to be a local filesystem path.
    virtual const String & getPath() const = 0;

    /// Total available space on the disk.
    virtual UInt64 getTotalSpace() const = 0;

    /// Space currently available on the disk.
    virtual UInt64 getAvailableSpace() const = 0;

    /// Space available for reservation (available space minus reserved space).
    virtual UInt64 getUnreservedSpace() const = 0;

    /// Amount of bytes which should be kept free on the disk.
    virtual UInt64 getKeepingFreeSpace() const { return 0; }

    /// Return `true` if the specified file exists.
    virtual bool exists(const String & path) const = 0;

    /// Return `true` if the specified file exists and it's a regular file (not a directory or special file type).
    virtual bool isFile(const String & path) const = 0;

    /// Return `true` if the specified file exists and it's a directory.
    virtual bool isDirectory(const String & path) const = 0;

    /// Return size of the specified file.
    virtual size_t getFileSize(const String & path) const = 0;

    /// Create directory.
    virtual void createDirectory(const String & path) = 0;

    /// Create directory and all parent directories if necessary.
    virtual void createDirectories(const String & path) = 0;

    /// Remove all files from the directory.
    virtual void clearDirectory(const String & path) = 0;

    /// Move directory from `from_path` to `to_path`.
    virtual void moveDirectory(const String & from_path, const String & to_path) = 0;

    /// Return iterator to the contents of the specified directory.
    virtual DiskDirectoryIterator iterateDirectory(const String & path) = 0;

    /// Return `true` if the specified directory is empty.
    bool isDirectoryEmpty(const String & path);

    /// Move the file from `from_path` to `to_path`.
    virtual void moveFile(const String & from_path, const String & to_path) = 0;

    /// Copy the file from `from_path` to `to_path`.
    virtual void copyFile(const String & from_path, const String & to_path) = 0;

    /// Open the file for read and return ReadBuffer object.
    virtual std::unique_ptr<ReadBuffer> read(const String & path, size_t buf_size = DBMS_DEFAULT_BUFFER_SIZE) const = 0;

    /// Open the file for write and return WriteBuffer object.
    virtual std::unique_ptr<WriteBuffer> write(const String & path, size_t buf_size = DBMS_DEFAULT_BUFFER_SIZE) = 0;

    /// Open the file for write in append mode and return WriteBuffer object.
    virtual std::unique_ptr<WriteBuffer> append(const String & path, size_t buf_size = DBMS_DEFAULT_BUFFER_SIZE) = 0;
};

using DiskPtr = std::shared_ptr<IDisk>;
using Disks = std::vector<DiskPtr>;

/**
 * Interface for internal disk directory iterator implementation
 */
class IDiskDirectoryIteratorImpl
{
public:
    /// Iterate to the next file.
    virtual void next() = 0;

    /// Return `true` if the iterator points to a valid element.
    virtual bool isValid() const = 0;

    /// Name of the file that the iterator currently points to.
    virtual String name() const = 0;

    virtual ~IDiskDirectoryIteratorImpl() = default;
};

/**
 * Iterator of directory contents on particular disk.
 */
class DiskDirectoryIterator : public std::iterator<std::forward_iterator_tag, String>
{
public:
    DiskDirectoryIterator() = default;
    DiskDirectoryIterator(std::unique_ptr<IDiskDirectoryIteratorImpl> && impl_);

    bool operator==(DiskDirectoryIterator const &) const;

    bool operator!=(DiskDirectoryIterator const &) const;

    DiskDirectoryIterator & operator++();

    String operator*() const;

private:
    std::unique_ptr<IDiskDirectoryIteratorImpl> impl;
};

/**
 * Information about reserved size on particular disk.
 */
class IReservation
{
public:
    /// Get reservation size.
    virtual UInt64 getSize() const = 0;

    /// Get disk where reservation take place.
    virtual DiskPtr getDisk() const = 0;

    /// Changes amount of reserved space.
    virtual void update(UInt64 new_size) = 0;

    /// Unreserves reserved space.
    virtual ~IReservation() = default;
};

/// Return full path to a file on disk.
inline String fullPath(const DiskPtr & disk, const String & path)
{
    return disk->getPath() + path;
}
}

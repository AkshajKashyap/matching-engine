#include "matching/journal_storage.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace matching {
namespace {

class FileDescriptor {
public:
    explicit FileDescriptor(int file_descriptor = -1) noexcept : file_descriptor_(file_descriptor) {}

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : file_descriptor_(std::exchange(other.file_descriptor_, -1)) {}

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            close();
            file_descriptor_ = std::exchange(other.file_descriptor_, -1);
        }
        return *this;
    }

    ~FileDescriptor() {
        close();
    }

    [[nodiscard]] int get() const noexcept {
        return file_descriptor_;
    }

    [[nodiscard]] int release() noexcept {
        return std::exchange(file_descriptor_, -1);
    }

private:
    void close() noexcept {
        if (file_descriptor_ >= 0) {
            (void)::close(file_descriptor_);
            file_descriptor_ = -1;
        }
    }

    int file_descriptor_;
};

[[nodiscard]] JournalStorageError make_error(
    JournalStorageErrorCode code,
    int system_error = 0,
    std::optional<JournalCodecError> codec_error = std::nullopt) {
    return JournalStorageError{
        .code = code,
        .system_error = system_error == 0
                            ? std::error_code{}
                            : std::error_code(system_error, std::generic_category()),
        .codec_error = codec_error,
    };
}

[[nodiscard]] JournalStorageError open_error(int error_number) {
    if (error_number == ENOENT) {
        return make_error(JournalStorageErrorCode::FileNotFound, error_number);
    }
    return make_error(JournalStorageErrorCode::OpenFailed, error_number);
}

[[nodiscard]] std::optional<JournalStorageError> write_all(
    int file_descriptor,
    std::span<const std::byte> bytes) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto* data = bytes.data() + written;
        const std::size_t remaining = bytes.size() - written;
        const ssize_t result = ::write(file_descriptor, data, remaining);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result == 0) {
            return make_error(
                JournalStorageErrorCode::WriteFailed,
                static_cast<int>(std::errc::io_error));
        }
        if (errno == EINTR) {
            continue;
        }
        return make_error(JournalStorageErrorCode::WriteFailed, errno);
    }
    return std::nullopt;
}

[[nodiscard]] std::variant<std::vector<std::byte>, JournalStorageError> pread_at_most(
    int file_descriptor,
    std::uint64_t offset,
    std::size_t byte_count) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return make_error(JournalStorageErrorCode::ReadFailed, EOVERFLOW);
    }

    std::vector<std::byte> bytes(byte_count);
    std::size_t read_count = 0;
    while (read_count < byte_count) {
        const ssize_t result = ::pread(
            file_descriptor,
            bytes.data() + read_count,
            byte_count - read_count,
            static_cast<off_t>(offset + read_count));
        if (result > 0) {
            read_count += static_cast<std::size_t>(result);
            continue;
        }
        if (result == 0) {
            bytes.resize(read_count);
            return bytes;
        }
        if (errno == EINTR) {
            continue;
        }
        return make_error(JournalStorageErrorCode::ReadFailed, errno);
    }
    return bytes;
}

[[nodiscard]] std::variant<std::uint64_t, JournalStorageError> file_size(int file_descriptor) {
    struct stat status {};
    if (::fstat(file_descriptor, &status) != 0) {
        return make_error(JournalStorageErrorCode::ReadFailed, errno);
    }
    if (status.st_size < 0) {
        return make_error(JournalStorageErrorCode::ReadFailed, EOVERFLOW);
    }
    if (static_cast<std::uintmax_t>(status.st_size) >
        std::numeric_limits<std::uint64_t>::max()) {
        return make_error(JournalStorageErrorCode::ReadFailed, EOVERFLOW);
    }
    return static_cast<std::uint64_t>(status.st_size);
}

[[nodiscard]] JournalScanResult scan_descriptor(int file_descriptor) {
    const auto size_result = file_size(file_descriptor);
    if (const auto* error = std::get_if<JournalStorageError>(&size_result)) {
        return *error;
    }
    const std::uint64_t size = std::get<std::uint64_t>(size_result);
    if (size < kJournalFileHeaderSize) {
        return make_error(JournalStorageErrorCode::InvalidJournal, 0, JournalCodecError::TruncatedInput);
    }

    const auto header_bytes_result = pread_at_most(file_descriptor, 0, kJournalFileHeaderSize);
    if (const auto* error = std::get_if<JournalStorageError>(&header_bytes_result)) {
        return *error;
    }
    const std::vector<std::byte>& header_bytes =
        std::get<std::vector<std::byte>>(header_bytes_result);
    if (header_bytes.size() != kJournalFileHeaderSize) {
        return make_error(JournalStorageErrorCode::ReadFailed, EIO);
    }
    const DecodedJournalFileHeader decoded_header = decode_file_header(header_bytes);
    if (const auto* error = std::get_if<JournalCodecError>(&decoded_header)) {
        return make_error(JournalStorageErrorCode::InvalidJournal, 0, *error);
    }

    JournalScan scan{
        .records = {},
        .valid_size = kJournalFileHeaderSize,
        .has_truncated_tail = false,
        .next_sequence = JournalSequence{1},
    };

    std::uint64_t expected_sequence = 1;
    while (scan.valid_size < size) {
        const std::uint64_t remaining = size - scan.valid_size;
        const std::size_t read_size = static_cast<std::size_t>(std::min<std::uint64_t>(
            remaining,
            kJournalFrameHeaderSize + kJournalMaximumPayloadLength));
        const auto bytes_result = pread_at_most(file_descriptor, scan.valid_size, read_size);
        if (const auto* error = std::get_if<JournalStorageError>(&bytes_result)) {
            return *error;
        }
        const std::vector<std::byte>& bytes = std::get<std::vector<std::byte>>(bytes_result);
        if (bytes.size() != read_size) {
            return make_error(JournalStorageErrorCode::ReadFailed, EIO);
        }

        const DecodedJournalRecordResult decoded = decode_record_prefix(bytes);
        if (const auto* error = std::get_if<JournalCodecError>(&decoded)) {
            if (*error == JournalCodecError::TruncatedInput) {
                scan.has_truncated_tail = true;
                return scan;
            }
            return make_error(JournalStorageErrorCode::InvalidJournal, 0, *error);
        }

        const DecodedJournalRecord& record = std::get<DecodedJournalRecord>(decoded);
        if (record.record.sequence.value != expected_sequence) {
            return make_error(JournalStorageErrorCode::SequenceViolation);
        }
        scan.records.push_back(record.record);
        scan.valid_size += record.bytes_consumed;

        if (expected_sequence == std::numeric_limits<std::uint64_t>::max()) {
            scan.next_sequence.reset();
            if (scan.valid_size != size) {
                return make_error(JournalStorageErrorCode::SequenceViolation);
            }
            return scan;
        }
        ++expected_sequence;
        scan.next_sequence = JournalSequence{expected_sequence};
    }

    return scan;
}

[[nodiscard]] std::optional<JournalStorageError> sync_directory(const std::filesystem::path& path) {
    const std::filesystem::path parent = path.has_parent_path() ? path.parent_path() : ".";
    const std::string parent_name = parent.string();
    const int descriptor = ::open(parent_name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        return make_error(JournalStorageErrorCode::DirectorySyncFailed, errno);
    }
    FileDescriptor directory(descriptor);
    if (::fsync(directory.get()) != 0) {
        return make_error(JournalStorageErrorCode::DirectorySyncFailed, errno);
    }
    return std::nullopt;
}

[[nodiscard]] std::variant<FileDescriptor, JournalStorageError> open_existing_for_append(
    const std::filesystem::path& path) {
    const std::string filename = path.string();
    const int descriptor = ::open(filename.c_str(), O_RDWR | O_APPEND | O_CLOEXEC);
    if (descriptor < 0) {
        return open_error(errno);
    }
    return FileDescriptor{descriptor};
}

} // namespace

JournalScanResult JournalReader::scan(const std::filesystem::path& path) {
    const std::string filename = path.string();
    const int descriptor = ::open(filename.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return open_error(errno);
    }
    const FileDescriptor file(descriptor);
    return scan_descriptor(file.get());
}

JournalWriter::JournalWriter(
    int file_descriptor,
    std::optional<JournalSequence> next_sequence) noexcept
    : file_descriptor_(file_descriptor), next_sequence_(next_sequence) {}

JournalWriter::JournalWriter(JournalWriter&& other) noexcept
    : file_descriptor_(std::exchange(other.file_descriptor_, -1)),
      next_sequence_(std::exchange(other.next_sequence_, std::nullopt)),
      failed_(std::exchange(other.failed_, true)) {}

JournalWriter& JournalWriter::operator=(JournalWriter&& other) noexcept {
    if (this != &other) {
        if (file_descriptor_ >= 0) {
            (void)::close(file_descriptor_);
        }
        file_descriptor_ = std::exchange(other.file_descriptor_, -1);
        next_sequence_ = std::exchange(other.next_sequence_, std::nullopt);
        failed_ = std::exchange(other.failed_, true);
    }
    return *this;
}

JournalWriter::~JournalWriter() {
    if (file_descriptor_ >= 0) {
        (void)::close(file_descriptor_);
    }
}

std::variant<JournalWriter, JournalStorageError> JournalWriter::create(
    const std::filesystem::path& path) {
    const std::string filename = path.string();
    const int descriptor = ::open(filename.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (descriptor < 0) {
        if (errno == EEXIST) {
            return make_error(JournalStorageErrorCode::FileAlreadyExists, errno);
        }
        return open_error(errno);
    }
    FileDescriptor file(descriptor);

    const std::vector<std::byte> header = encode_file_header();
    if (const auto write_error = write_all(file.get(), header)) {
        return *write_error;
    }
    if (::fsync(file.get()) != 0) {
        return make_error(JournalStorageErrorCode::SyncFailed, errno);
    }
    if (const auto directory_sync_error = sync_directory(path)) {
        return *directory_sync_error;
    }
    return JournalWriter{file.release(), JournalSequence{1}};
}

std::variant<JournalWriter, JournalStorageError> JournalWriter::open_recovered(
    const std::filesystem::path& path) {
    auto file_result = open_existing_for_append(path);
    if (const auto* error = std::get_if<JournalStorageError>(&file_result)) {
        return *error;
    }
    FileDescriptor file = std::move(std::get<FileDescriptor>(file_result));
    const JournalScanResult scan_result = scan_descriptor(file.get());
    if (const auto* error = std::get_if<JournalStorageError>(&scan_result)) {
        return *error;
    }
    const JournalScan& scan = std::get<JournalScan>(scan_result);
    if (scan.has_truncated_tail) {
        return make_error(JournalStorageErrorCode::TruncatedTailRequiresRepair);
    }
    return JournalWriter{file.release(), scan.next_sequence};
}

std::variant<JournalWriter, JournalStorageError> JournalWriter::repair_truncated_tail_and_open(
    const std::filesystem::path& path) {
    auto file_result = open_existing_for_append(path);
    if (const auto* error = std::get_if<JournalStorageError>(&file_result)) {
        return *error;
    }
    FileDescriptor file = std::move(std::get<FileDescriptor>(file_result));
    const JournalScanResult scan_result = scan_descriptor(file.get());
    if (const auto* error = std::get_if<JournalStorageError>(&scan_result)) {
        return *error;
    }
    const JournalScan& scan = std::get<JournalScan>(scan_result);
    if (!scan.has_truncated_tail) {
        return make_error(JournalStorageErrorCode::NoTruncatedTail);
    }
    if (scan.valid_size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return make_error(JournalStorageErrorCode::TruncateFailed, EOVERFLOW);
    }
    if (::ftruncate(file.get(), static_cast<off_t>(scan.valid_size)) != 0) {
        return make_error(JournalStorageErrorCode::TruncateFailed, errno);
    }
    if (::fsync(file.get()) != 0) {
        return make_error(JournalStorageErrorCode::SyncFailed, errno);
    }
    return JournalWriter{file.release(), scan.next_sequence};
}

JournalAppendResult JournalWriter::append_and_sync(const JournalCommand& command) {
    if (failed_) {
        return make_error(JournalStorageErrorCode::WriterFailed);
    }
    if (!next_sequence_.has_value()) {
        return make_error(JournalStorageErrorCode::SequenceExhausted);
    }

    const JournalSequence assigned_sequence = *next_sequence_;
    const EncodedJournalRecord encoded = encode_record(assigned_sequence, command);
    if (const auto* codec_error = std::get_if<JournalCodecError>(&encoded)) {
        return make_error(JournalStorageErrorCode::CodecFailure, 0, *codec_error);
    }
    const std::vector<std::byte>& bytes = std::get<std::vector<std::byte>>(encoded);
    if (const auto write_error = write_all(file_descriptor_, bytes)) {
        failed_ = true;
        return *write_error;
    }
    if (::fsync(file_descriptor_) != 0) {
        failed_ = true;
        return make_error(JournalStorageErrorCode::SyncFailed, errno);
    }

    if (assigned_sequence.value == std::numeric_limits<std::uint64_t>::max()) {
        next_sequence_.reset();
    } else {
        next_sequence_ = JournalSequence{assigned_sequence.value + 1U};
    }
    return assigned_sequence;
}

std::optional<JournalSequence> JournalWriter::next_sequence() const noexcept {
    return next_sequence_;
}

bool JournalWriter::failed() const noexcept {
    return failed_;
}

} // namespace matching

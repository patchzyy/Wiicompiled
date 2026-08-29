#include "CardGciFolder.hpp"

#include <filesystem>
#include "../fs_helper.hpp"

#include "Directory.hpp"
#include "FileIO.hpp"
#include "../internal.hpp"

namespace {
aurora::Module Log("aurora::card");
}

namespace aurora::card {

CardGciFolder::GciFile* CardGciFolder::getFile(uint32_t idx) {
  if (m_files.size() > idx) {
    auto file = &m_files[idx];
    if (file->opened)
      return file;
  }
  return nullptr;
}

CardGciFolder::GciFile* CardGciFolder::getFile(FileHandle& fh) { return getFile(fh.getFileNo()); }

const CardGciFolder::GciFile* CardGciFolder::getFile(uint32_t idx) const {
  if (m_files.size() > idx) {
    auto file = &m_files[idx];
    if (file->opened)
      return file;
  }
  return nullptr;
}

const CardGciFolder::GciFile* CardGciFolder::getFile(FileHandle& fh) const { return getFile(fh.getFileNo()); }

CardGciFolder::CardGciFolder() {}

CardGciFolder::CardGciFolder(CardGciFolder&& other) {
  m_files = std::move(other.m_files);
  m_bat = std::move(other.m_bat);
  m_folderPath = other.m_folderPath;
  m_encoding = other.m_encoding;

  CardGciFolder::setCurrentGame(other.m_game);
  CardGciFolder::setCurrentMaker(other.m_maker);
}

CardGciFolder& CardGciFolder::operator=(CardGciFolder&& other) { return *this; }

void CardGciFolder::InitCard(const char* game, const char* maker) {
  setCurrentGame(game);
  setCurrentMaker(maker);
}

ECardResult CardGciFolder::openFile(const char* filename, FileHandle& handleOut) {
  int idx = 0;
  for (auto& gciFile : m_files) {
    if (strcmp(filename, gciFile.file.m_filename) == 0) {
      gciFile.opened = true;
      if (gciFile.fileSize == 0)
        gciFile.fileSize = std::filesystem::file_size(m_folderPath / gciFile.filename);

      handleOut = FileHandle(idx, 0);
      return ECardResult::READY;
    }
    idx++;
  }

  return ECardResult::NOCARD;
}

ECardResult CardGciFolder::openFile(uint32_t fileno, FileHandle& handleOut) {
  if (m_files.size() > fileno) {
    auto& gciFile = m_files[fileno];
    handleOut = FileHandle(fileno, 0);
    gciFile.opened = true;
    if (gciFile.fileSize == 0)
      gciFile.fileSize = std::filesystem::file_size(m_folderPath / gciFile.filename);

    return ECardResult::READY;
  }

  return ECardResult::NOCARD;
}

ECardResult CardGciFolder::createFile(const char* filename, size_t size, FileHandle& handleOut) {
  std::string gciFilename = fmt::format("{}-{}-{}.gci", m_maker, m_game, filename);
  uint16_t neededBlocks = ROUND_UP_8192(size) / BlockSize;
  size_t fileSize = sizeof(File) + size;
  constexpr uint16_t maxBlocks = ((uint16_t)ECardSize::Card2043Mb * MbitToBlocks) - FSTBlocks;

  std::vector<uint8_t> fileBuf(fileSize);

  File* gciFileHeader = (File*)fileBuf.data();

  std::memcpy(gciFileHeader->m_game, m_game, 4);
  std::memcpy(gciFileHeader->m_maker, m_maker, 2);
  strncpy(gciFileHeader->m_filename, filename, std::size(gciFileHeader->m_filename));

  gciFileHeader->m_modifiedTime = static_cast<uint32_t>(getGCTime());
  gciFileHeader->m_blockCount = neededBlocks;
  gciFileHeader->m_firstBlock = m_bat.allocateBlocks(neededBlocks, maxBlocks);

  // write big endian header for dolphin compat
  gciFileHeader->swapEndian();
  FileIO file(m_folderPath / gciFilename, true);
  if (!file || !file.fileWrite(fileBuf.data(), fileBuf.size(), 0)) {
    return ECardResult::IOERROR;
  }

  gciFileHeader->swapEndian();
  // push non-endian swapped header first
  m_files.push_back({*gciFileHeader, fileSize,
                     std::u8string(gciFilename.begin(), gciFilename.end()), false});
  handleOut = FileHandle(m_files.size() - 1, 0);

  return ECardResult::READY;
}

ECardResult CardGciFolder::closeFile(FileHandle& fh) {
  auto file = getFile(fh);
  if (file) {
    file->opened = false;
    return ECardResult::READY;
  }

  return ECardResult::NOFILE;
}

void CardGciFolder::deleteFile(const FileHandle& fh) {
  auto file = getFile(fh.getFileNo());
  if (!file)
    return;

  FileIO fileIO(m_folderPath / file->filename, true);
  if (fileIO)
    fileIO.deleteFile();
}

ECardResult CardGciFolder::deleteFile(const char* filename) { return ECardResult::NOCARD; }

ECardResult CardGciFolder::deleteFile(uint32_t fileno) { return ECardResult::NOCARD; }

ECardResult CardGciFolder::renameFile(const char* oldName, const char* newName) {
  for (auto& gciFile : m_files) {
    if (strcmp(oldName, gciFile.file.m_filename) == 0) {
      strncpy(gciFile.file.m_filename, newName, std::size(gciFile.file.m_filename));
      return ECardResult::READY;
    }
  }

  return ECardResult::NOCARD;
}

ECardResult CardGciFolder::fileWrite(FileHandle& fh, const void* buf, size_t size) {
  auto file = getFile(fh);
  if (file) {
    FileIO fileIO(m_folderPath / file->filename);
    if (fileIO) {
      if (fileIO.fileWrite(buf, size, sizeof(File) + fh.offset))
        return ECardResult::READY;
      return ECardResult::IOERROR;
    }
    return ECardResult::NOFILE;
  }

  return ECardResult::NOCARD;
}

ECardResult CardGciFolder::fileRead(FileHandle& fh, void* dst, size_t size) {
  auto file = getFile(fh);
  if (file) {
    FileIO fileIO(m_folderPath / file->filename);
    if (fileIO) {
      if (fileIO.fileRead(dst, size, sizeof(File) + fh.offset))
        return ECardResult::READY;
      return ECardResult::IOERROR;
    }
    return ECardResult::NOFILE;
  }

  return ECardResult::NOCARD;
}

void CardGciFolder::seek(FileHandle& fh, int32_t pos, SeekOrigin whence) {
  auto file = getFile(fh);
  if (file) {
    switch (whence) {
    case SeekOrigin::Begin:
      fh.offset = pos;
      break;
    case SeekOrigin::Current:
      fh.offset += pos;
      break;
    case SeekOrigin::End:
      fh.offset = file->fileSize - pos;
      break;
    }
  }
}

ECardResult CardGciFolder::getStatus(const FileHandle& fh, CardStat& statOut) const {
  return getStatus(fh.getFileNo(), statOut);
}

ECardResult CardGciFolder::getStatus(uint32_t fileNo, CardStat& statOut) const {
  auto gciFile = getFile(fileNo);

  if (!gciFile)
    return ECardResult::NOFILE;
  const auto file = &gciFile->file;

  std::strncpy(statOut.x0_fileName, file->m_filename, 32);
  statOut.x20_length = file->m_blockCount * BlockSize;
  statOut.x24_time = file->m_modifiedTime;
  memmove(statOut.x28_gameName.data(), file->m_game, 4);
  memmove(statOut.x2c_company.data(), file->m_maker, 4);

  statOut.x2e_bannerFormat = file->m_bannerFlags;
  statOut.x30_iconAddr = file->m_iconAddress;
  statOut.x34_iconFormat = file->m_iconFmt;
  statOut.x36_iconSpeed = file->m_animSpeed;
  statOut.x38_commentAddr = file->m_commentAddr;

  if (file->m_iconAddress == UINT32_MAX) {
    statOut.x3c_offsetBanner = UINT32_MAX;
    statOut.x40_offsetBannerTlut = UINT32_MAX;
    statOut.x44_offsetIcon.fill(UINT32_MAX);
    statOut.x64_offsetIconTlut = UINT32_MAX;
    statOut.x68_offsetData = file->m_commentAddr + 64;
  } else {
    uint32_t cur = file->m_iconAddress;
    statOut.x3c_offsetBanner = cur;
    cur += BannerSize(statOut.GetBannerFormat());
    statOut.x40_offsetBannerTlut = cur;
    cur += TlutSize(statOut.GetBannerFormat());
    bool palette = false;
    for (size_t i = 0; i < statOut.x44_offsetIcon.size(); ++i) {
      statOut.x44_offsetIcon[i] = cur;
      const EImageFormat fmt = statOut.GetIconFormat(static_cast<int>(i));
      if (fmt == EImageFormat::C8) {
        palette = true;
      }
      cur += IconSize(fmt);
    }
    if (palette) {
      statOut.x64_offsetIconTlut = cur;
      cur += TlutSize(EImageFormat::C8);
    } else
      statOut.x64_offsetIconTlut = UINT32_MAX;
    statOut.x68_offsetData = cur;
  }

  return ECardResult::READY;
}

ECardResult CardGciFolder::setStatus(const FileHandle& fh, const CardStat& stat) {
  return setStatus(fh.getFileNo(), stat);
}

ECardResult CardGciFolder::setStatus(uint32_t fileNo, const CardStat& stat) {
  auto gciFile = getFile(fileNo);

  if (!gciFile)
    return ECardResult::NOFILE;
  const auto file = &gciFile->file;

  file->m_bannerFlags = stat.x2e_bannerFormat;
  file->m_iconAddress = stat.x30_iconAddr;
  file->m_iconFmt = stat.x34_iconFormat;
  file->m_animSpeed = stat.x36_iconSpeed;
  file->m_commentAddr = stat.x38_commentAddr;

  return ECardResult::READY;
}

void CardGciFolder::setCurrentGame(const char* game) {
  if (game != nullptr && std::strlen(game) == 4) {
    std::memcpy(m_game, game, 4);
  }
}

const uint8_t* CardGciFolder::getCurrentGame() const {
  if (std::strlen(m_game) == sizeof(m_game) - 1) {
    return reinterpret_cast<const uint8_t*>(m_game);
  }

  return nullptr;
}

void CardGciFolder::setCurrentMaker(const char* maker) {
  if (maker != nullptr && std::strlen(maker) == 2) {
    std::memcpy(m_maker, maker, 2);
  }
}

const uint8_t* CardGciFolder::getCurrentMaker() const {
  if (std::strlen(m_maker) == sizeof(m_maker) - 1) {
    return reinterpret_cast<const uint8_t*>(m_maker);
  }

  return nullptr;
}

void CardGciFolder::getSerial(uint64_t& serial) {
  serial = 0; // TODO
}

void CardGciFolder::getChecksum(uint16_t& checksum, uint16_t& inverse) const {
  // TODO
  checksum = 0;
  inverse = 0;
}

void CardGciFolder::getFreeBlocks(int32_t& bytesNotUsed, int32_t& filesNotUsed) const {
  bytesNotUsed = m_bat.numFreeBlocks() * BlockSize;
  filesNotUsed = MaxFiles;
}

void CardGciFolder::getEncoding(uint16_t& encoding) const { encoding = (uint16_t)m_encoding; }

void CardGciFolder::format(ECardSlot deviceId, ECardSize size, EEncoding encoding) {
  m_encoding = encoding;

  if (!std::filesystem::create_directories(m_folderPath)) {
    Log.error("Failed to create directory: {}", fs_path_to_string(m_folderPath));
  }
}

void CardGciFolder::commit() {
  for (auto& gciFile : m_files) {
    if (gciFile.opened) {
      FileIO file(m_folderPath / gciFile.filename);

      File tempFile = gciFile.file;
      tempFile.swapEndian();
      file.fileWrite(&tempFile, sizeof(File), 0); // update header
    }
  }
}

bool CardGciFolder::open(const std::filesystem::path& filepath) {
  m_folderPath = filepath;

  std::error_code ec;
  if (!std::filesystem::exists(filepath, ec) || !std::filesystem::is_directory(filepath, ec)) {
    if (ec) {
      Log.warn("Failed to inspect GCI folder '{}': {}", fs_path_to_string(filepath), ec.message());
    }
    return false;
  }

  std::filesystem::directory_iterator it(
      filepath, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    Log.warn("Failed to enumerate GCI folder '{}': {}", fs_path_to_string(filepath), ec.message());
    return false;
  }

  const std::filesystem::directory_iterator end;
  while (it != end) {
    const auto path = it->path();
    const auto status = it->status(ec);
    if (ec) {
      Log.warn("Failed to inspect GCI folder entry '{}': {}", fs_path_to_string(path), ec.message());
      return false;
    }
    if (!std::filesystem::is_regular_file(status)) {
      it.increment(ec);
      if (ec) {
        Log.warn("Failed to continue enumerating GCI folder '{}': {}", fs_path_to_string(filepath), ec.message());
        return false;
      }
      continue;
    }

    if (path.extension() != ".gci") {
      it.increment(ec);
      if (ec) {
        Log.warn("Failed to continue enumerating GCI folder '{}': {}", fs_path_to_string(filepath), ec.message());
        return false;
      }
      continue;
    }

    FileIO file(path);
    if (!file) {
      Log.warn("Failed to open GCI file '{}'", fs_path_to_string(path));
      return false;
    }

    File fileData;
    if (!file.fileRead(&fileData, sizeof(File), 0)) {
      Log.warn("Failed to read GCI file '{}'", fs_path_to_string(path));
      return false;
    }
    fileData.swapEndian();

    m_files.push_back({fileData, file.fileSize(), path.filename().u8string(), false});

    it.increment(ec);
    if (ec) {
      Log.warn("Failed to continue enumerating GCI folder '{}': {}", fs_path_to_string(filepath), ec.message());
      return false;
    }
  }

  return true;
}

void CardGciFolder::close() {
  m_files.clear();
  m_folderPath = "";
  m_bat = BlockAllocationTable();
}

static std::filesystem::path g_cardFileNameEmpty = "";

const std::filesystem::path& CardGciFolder::cardFilename() const { return g_cardFileNameEmpty; }

ECardResult CardGciFolder::getError() const { return ECardResult::READY; }

ProbeResults CardGciFolder::probeCardFile(const std::filesystem::path& filename) {
  if (!std::filesystem::exists(filename) || !std::filesystem::is_directory(filename))
    return {ECardResult::NOCARD, 0, 0};
  return {ECardResult::READY, static_cast<uint32_t>(ECardSize::Card2043Mb), BlockSize};
}

} // namespace aurora::card

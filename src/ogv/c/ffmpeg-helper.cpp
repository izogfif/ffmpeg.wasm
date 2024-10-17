#include "ffmpeg-helper.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc.h>
#include <emscripten/em_asm.h>

bool loggingEnabled = true;

void logMessage(char const *format, ...)
{
  if (loggingEnabled)
  {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
  }
}

void writeInt32(uint8_t **pBuf, int32_t value_to_copy, uint32_t *pSizeCounter)
{
  const int data_size = 4;
  memcpy(*pBuf, &value_to_copy, data_size);
  *pBuf += data_size;
  *pSizeCounter += data_size;
  // logMessage("copy_int32: wrote %d\n", value_to_copy);
}

void writeInt64(uint8_t **pBuf, int64_t value_to_copy, uint32_t *pSizeCounter)
{
  const int data_size = 8;
  memcpy(*pBuf, &value_to_copy, data_size);
  *pBuf += data_size;
  *pSizeCounter += data_size;
  // logMessage("copy_int64: wrote %lld\n", value_to_copy);
}

int32_t readInt32(const char **pBuf)
{
  const int data_size = 4;
  int32_t result = -1;
  memcpy(&result, *pBuf, data_size);
  *pBuf += data_size;
  // logMessage("readInt32: got %d\n", result);
  return result;
}

int64_t readInt64(const char **pBuf)
{
  const int data_size = 8;
  int64_t result = -1;
  memcpy(&result, *pBuf, data_size);
  *pBuf += data_size;
  // logMessage("readInt64: got %lld\n", result);
  return result;
}

DemuxedPacket::DemuxedPacket(int64_t pts, int64_t dts, uint32_t dataSize, const uint8_t *pData)
    : m_pts(pts), m_dts(dts), m_pData(NULL), m_dataSize(dataSize)
{
  if (dataSize)
  {
    m_pData = (uint8_t *)malloc(m_dataSize);
    memcpy(m_pData, pData, dataSize);
  }
}
DemuxedPacket::~DemuxedPacket()
{
  if (m_pData)
  {
    free(m_pData);
  }
}

PacketBuffer::PacketBuffer(int maxSize)
    : m_maxSize(maxSize),
      m_sizeOfPtsQueueOfRecentlyRemovedPackets(1000)
{
}

void PacketBuffer::setMaxSize(int maxSize)
{
  m_maxSize = maxSize;
}

int PacketBuffer::size() const
{
  return m_videoPackets.size();
}

int PacketBuffer::getMaxSize() const
{
  return m_maxSize;
}

void PacketBuffer::clear()
{
  m_videoPackets.clear();
  m_ptsToRequest.clear();
  m_ptsOfPacketsEverAdded.clear();
  m_ptsOfRecentlyRemovedPackets.clear();
}

bool PacketBuffer::empty() const
{
  return size() == 0;
}

bool PacketBuffer::isFull() const
{
  return size() == m_maxSize;
}

void PacketBuffer::pop_front()
{
  const int64_t ptsOfFirstPacket = m_videoPackets.front().m_pts;
  m_videoPackets.pop_front();
  // We need to erase only the first (lowest) pts, not the pts of the packet we're popping
  m_ptsToRequest.erase(m_ptsToRequest.begin());
  m_ptsOfRecentlyRemovedPackets.push_back(ptsOfFirstPacket);
  if (m_ptsOfRecentlyRemovedPackets.size() > m_sizeOfPtsQueueOfRecentlyRemovedPackets)
  {
    const auto ptsToForgetForever = m_ptsOfRecentlyRemovedPackets.front();
    m_ptsOfRecentlyRemovedPackets.pop_front();
    m_ptsOfPacketsEverAdded.erase(ptsToForgetForever);
  }
}

void PacketBuffer::emplace_back(int64_t pts, int64_t dts, uint32_t dataSize, const uint8_t *pData)
{
  m_videoPackets.emplace_back(pts, dts, dataSize, pData);
  m_ptsToRequest.insert(pts);
  m_ptsOfPacketsEverAdded.insert(pts);
}

int64_t PacketBuffer::getMinPts() const
{
  return *m_ptsToRequest.begin();
}

std::deque<DemuxedPacket>::const_iterator PacketBuffer::begin() const
{
  return this->m_videoPackets.begin();
}

std::deque<DemuxedPacket>::const_iterator PacketBuffer::end() const
{
  return this->m_videoPackets.end();
}

bool PacketBuffer::hasPacketWithPts(int64_t pts) const
{
  return m_ptsOfPacketsEverAdded.find(pts) != m_ptsOfPacketsEverAdded.end();
}

const DemuxedPacket &PacketBuffer::front() const
{
  return m_videoPackets.front();
}

size_t getTotalMemory() {
  return (size_t)EM_ASM_PTR(return HEAP8.length);
}

size_t getFreeMemory() {
  struct mallinfo i = mallinfo();
  uintptr_t totalMemory = getTotalMemory();
  uintptr_t dynamicTop = (uintptr_t)sbrk(0);
  return totalMemory - dynamicTop + i.fordblks;
}

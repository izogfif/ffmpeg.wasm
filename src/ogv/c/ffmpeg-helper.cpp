#include "ffmpeg-helper.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void logCallback(char const *format, ...)
{
  if (DEBUG_ENABLED)
  {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
  }
}

void copyInt32(uint8_t **pBuf, int32_t value_to_copy, uint32_t *pSizeCounter)
{
  const int data_size = 4;
  memcpy(*pBuf, &value_to_copy, data_size);
  *pBuf += data_size;
  *pSizeCounter += data_size;
  // logCallback("copy_int32: wrote %d\n", value_to_copy);
}

void copyInt64(uint8_t **pBuf, int64_t value_to_copy, uint32_t *pSizeCounter)
{
  const int data_size = 8;
  memcpy(*pBuf, &value_to_copy, data_size);
  *pBuf += data_size;
  *pSizeCounter += data_size;
  // logCallback("copy_int64: wrote %lld\n", value_to_copy);
}

DemuxedPacket::DemuxedPacket(int64_t pts, int64_t dts, uint32_t dataSize, const uint8_t *pData)
    : m_pts(pts), m_dts(dts), m_dataSize(dataSize), m_pData(dataSize ? (uint8_t *)malloc(m_dataSize) : NULL)
{
  if (dataSize)
  {
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

PacketBuffer::PacketBuffer(int maxSize) : m_maxSize(maxSize)
{
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
  m_ptsToRequest.erase(m_ptsToRequest.begin());
}

void PacketBuffer::emplace_back(int64_t pts, int64_t dts, uint32_t dataSize, const uint8_t *pData)
{
  m_videoPackets.emplace_back(pts, dts, dataSize, pData);
  m_ptsToRequest.insert(pts);
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

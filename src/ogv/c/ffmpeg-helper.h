#include <deque>
#include <set>
#include <unordered_set>

#define FFMPEG_CODEC_NAME "ffmpeg"
#define DEBUG_ENABLED 1

#define PACKET_BUFFER_SIZE 10

#ifdef __cplusplus
extern "C"
{
#endif
#include <stdint.h>
  extern bool loggingEnabled;

  void logMessage(char const *format, ...);
  void writeInt32(uint8_t **pBuf, int32_t value_to_copy, uint32_t *pSizeCounter);
  void writeInt64(uint8_t **pBuf, int64_t value_to_copy, uint32_t *pSizeCounter);
  int32_t readInt32(const char **pBuf);
  int64_t readInt64(const char **pBuf);
  size_t getTotalMemory();
  size_t getFreeMemory();

#ifdef __cplusplus
}
#endif

class DemuxedPacket
{
public:
  DemuxedPacket(int64_t pts, int64_t dts, uint32_t dataSize, const uint8_t *pData);
  ~DemuxedPacket();

  const int64_t m_pts;
  const int64_t m_dts;
  const uint32_t m_dataSize;
  uint8_t *m_pData;
};

class PacketBuffer
{
public:
  /**
   * maxSize does not limit internal logic. It only affects isFull and getMaxSize methods
   */
  PacketBuffer(int maxSize);
  int size() const;
  int getMaxSize() const;
  void clear();
  bool isFull() const;
  void pop_front();
  void emplace_back(int64_t pts, int64_t dts, uint32_t dataSize, const uint8_t *pData);
  std::deque<DemuxedPacket>::const_iterator begin() const;
  std::deque<DemuxedPacket>::const_iterator end() const;
  int64_t getMinPts() const;
  bool empty() const;
  bool hasPacketWithPts(int64_t pts) const;
  const DemuxedPacket &front() const;
  void setMaxSize(int maxSize);

private:
  std::deque<DemuxedPacket> m_videoPackets;
  std::set<int64_t> m_ptsToRequest;
  std::deque<int64_t> m_ptsOfRecentlyRemovedPackets;
  std::unordered_set<int64_t> m_ptsOfPacketsEverAdded;
  int m_maxSize;
  const int m_sizeOfPtsQueueOfRecentlyRemovedPackets;
};
#include <deque>
#include <set>

#define FFMPEG_CODEC_NAME "ffmpeg"
#define DEBUG_ENABLED 0

#ifdef __cplusplus
extern "C"
{
#endif
#include <stdint.h>

  void logCallback(char const *format, ...);
  void copyInt32(uint8_t **pBuf, int32_t value_to_copy, uint32_t *pSizeCounter);
  void copyInt64(uint8_t **pBuf, int64_t value_to_copy, uint32_t *pSizeCounter);
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

private:
  std::deque<DemuxedPacket> m_videoPackets;
  std::set<int64_t> m_ptsToRequest;
  const int m_maxSize;
};
#pragma once

#include <memory>

struct AVFrame;

namespace caspar { namespace ffmpeg {

class SubstitutedA53CCQueue final
{
  public:
    SubstitutedA53CCQueue();
    ~SubstitutedA53CCQueue();

    SubstitutedA53CCQueue(const SubstitutedA53CCQueue&)            = delete;
    SubstitutedA53CCQueue& operator=(const SubstitutedA53CCQueue&) = delete;

    // Replaces caption data with stable keys before filtering, then restores the original data afterwards.
    // This prevents FFmpeg filters from splitting or merging caption packets and losing their frame association.
    void insert_and_replace_with_key(AVFrame* frame);
    void extract_by_key(AVFrame* frame);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}} // namespace caspar::ffmpeg
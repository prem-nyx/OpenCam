#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

enum class MediaPipelineState
{
    Stopped,
    Starting,
    Running,
    Failed
};

class MediaPipeline
{
public:
    MediaPipeline() = default;
    ~MediaPipeline();

    MediaPipeline(const MediaPipeline&) = delete;
    MediaPipeline& operator=(const MediaPipeline&) = delete;

    bool start(const std::string& authenticatedRtspUrl,
               const std::string& videoDevice);
    void stop();

    MediaPipelineState state() const noexcept;
    std::string errorMessage() const;

private:
    static int interruptCallback(void* opaque);
    void run(std::string authenticatedRtspUrl, std::string videoDevice);
    void setFailure(const std::string& message);

    std::atomic<bool> stopRequested_{false};
    std::atomic<MediaPipelineState> state_{MediaPipelineState::Stopped};
    mutable std::mutex errorMutex_;
    std::string errorMessage_;
    std::thread worker_;
};
